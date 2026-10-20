// SPDX-License-Identifier: GPL-2.0
/*
 * Small helpers shared by the tbaccel client and peers, with tracing.
 *
 * Verbosity: TBX_VERBOSE=0 quiet, 1 info (default), 2 debug (every ioctl).
 *   TBX_VERBOSE=2 ./tbaccel_client /dev/tbaccel0 4 8 16
 */
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-heap.h>

#include "thunderbolt_accel.h"	/* vendored UAPI, sits next to this header */
#include "tbaccel_proto.h"

/* ------------------------------------------------------------------ logging */

static int tbx_log_level = -1;

static inline void tbx_log_init(void)
{
	if (tbx_log_level < 0) {
		const char *e = getenv("TBX_VERBOSE");

		tbx_log_level = e ? atoi(e) : 1;
		if (tbx_log_level < 0)
			tbx_log_level = 0;
	}
}

static inline void tbx_logf(int level, const char *fmt, ...)
{
	va_list ap;
	struct timespec ts;

	tbx_log_init();
	if (tbx_log_level < level)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%6ld.%03ld] ", (long)(ts.tv_sec % 100000), ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define TBX_ERR(...)  do { tbx_logf(0, "ERROR: " __VA_ARGS__); } while (0)
#define TBX_INFO(...) tbx_logf(1, __VA_ARGS__)
#define TBX_DBG(...)  tbx_logf(2, __VA_ARGS__)

static inline const char *tbx_type_name(uint32_t t)
{
	switch (t) {
	case TBX_PREPARE:  return "PREPARE";
	case TBX_PREPARED: return "PREPARED";
	case TBX_LOAD:     return "LOAD";
	case TBX_MATMUL:   return "MATMUL";
	case TBX_RESULT:   return "RESULT";
	default:           return "?";
	}
}

/* --------------------------------------------------------------- dma-buf */

static inline int tbx_alloc_dmabuf(size_t size, void **ptr)
{
	int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	struct dma_heap_allocation_data data = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
	int fd;

	TBX_DBG("dma-buf: allocating %zu bytes", size);
	*ptr = NULL;
	if (heap < 0) {
		TBX_ERR("dma-buf: cannot open /dev/dma_heap/system: %s", strerror(errno));
		return -1;
	}
	if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
		TBX_ERR("dma-buf: DMA_HEAP_IOCTL_ALLOC(%zu) failed: %s", size, strerror(errno));
		close(heap);
		return -1;
	}
	close(heap);
	fd = (int) data.fd;
	*ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*ptr == MAP_FAILED) {
		TBX_ERR("dma-buf: mmap failed: %s", strerror(errno));
		*ptr = NULL;
	}
	TBX_INFO("dma-buf: size=%zu fd=%d ptr=%p", size, fd, *ptr);
	return fd;
}

/* ----------------------------------------------------------- ioctl wrappers */

static inline int tbx_import(int fd, int dmabuf_fd, uint64_t *handle, uint64_t *size)
{
	struct tbaccel_import req = { .fd = (uint32_t) dmabuf_fd };
	int r = ioctl(fd, TBACCEL_IOCTL_IMPORT, &req);

	if (r < 0) {
		int e = errno;

		TBX_ERR("IMPORT(dmabuf_fd=%d) failed: %s", dmabuf_fd, strerror(e));
		errno = e;
		return r;
	}
	*handle = req.handle;
	*size = req.size;
	TBX_INFO("IMPORT(dmabuf_fd=%d) -> handle=%llu size=%llu",
		 dmabuf_fd, (unsigned long long) *handle, (unsigned long long) *size);
	return 0;
}

static inline int tbx_region(int fd, uint64_t handle, uint64_t off, uint64_t size, uint64_t *region)
{
	struct tbaccel_region req = { .handle = handle, .offset = off, .size = size };
	int r = ioctl(fd, TBACCEL_IOCTL_REGION, &req);

	if (r < 0) {
		int e = errno;

		TBX_ERR("REGION(handle=%llu off=%llu size=%llu) failed: %s",
			(unsigned long long) handle, (unsigned long long) off,
			(unsigned long long) size, strerror(e));
		errno = e;
		return r;
	}
	*region = req.region;
	TBX_INFO("REGION(handle=%llu off=%llu size=%llu) -> region=%llu",
		 (unsigned long long) handle, (unsigned long long) off,
		 (unsigned long long) size, (unsigned long long) *region);
	return 0;
}

static inline int tbx_post_recv(int fd, uint64_t region, uint64_t *slot, int *fence_fd)
{
	struct tbaccel_post_recv req = { .region = region };
	int r = ioctl(fd, TBACCEL_IOCTL_POST_RECV, &req);

	if (r < 0) {
		int e = errno;

		TBX_ERR("POST_RECV(region=%llu) failed: %s",
			(unsigned long long) region, strerror(e));
		errno = e;
		return r;
	}
	*slot = req.slot;
	*fence_fd = req.fence_fd;
	TBX_INFO("POST_RECV(region=%llu) -> slot=%llu fence_fd=%d",
		 (unsigned long long) region, (unsigned long long) *slot, *fence_fd);
	return 0;
}

static inline int tbx_submit(int fd, uint64_t slot, uint64_t region, const void *ctrl,
			     uint32_t ctrl_len, int in_fence_fd, int *out_fence_fd)
{
	struct tbaccel_submit req = {
		.slot = slot,
		.region = region,
		.ctrl = (uint64_t) (uintptr_t) ctrl,
		.ctrl_len = ctrl_len,
		.in_fence_fd = in_fence_fd,
	};
	int r = ioctl(fd, TBACCEL_IOCTL_SUBMIT, &req);

	if (r < 0) {
		int e = errno;

		TBX_ERR("SUBMIT(slot=%llu region=%llu ctrl_len=%u in_fence=%d) failed: %s",
			(unsigned long long) slot, (unsigned long long) region,
			ctrl_len, in_fence_fd, strerror(e));
		errno = e;
		return r;
	}
	*out_fence_fd = req.out_fence_fd;
	TBX_INFO("SUBMIT(slot=%llu region=%llu ctrl_len=%u) -> out_fence_fd=%d",
		 (unsigned long long) slot, (unsigned long long) region,
		 ctrl_len, *out_fence_fd);
	return 0;
}

static inline int tbx_send_ctrl(int fd, uint64_t slot, const void *blob, uint32_t len)
{
	struct tbaccel_send_ctrl req = { .slot = slot, .buf = (uint64_t) (uintptr_t) blob, .len = len };
	int r = ioctl(fd, TBACCEL_IOCTL_SEND_CTRL, &req);

	if (r < 0) {
		int e = errno;

		TBX_ERR("SEND_CTRL(slot=%llu len=%u) failed: %s",
			(unsigned long long) slot, len, strerror(e));
		errno = e;
		return r;
	}
	TBX_DBG("SEND_CTRL(slot=%llu len=%u) ok", (unsigned long long) slot, len);
	return 0;
}

static inline int tbx_recv_ctrl(int fd, void *blob, uint32_t cap, uint64_t *slot, uint32_t *len)
{
	struct tbaccel_recv_ctrl req = { .buf = (uint64_t) (uintptr_t) blob, .len = cap };
	int r;

	TBX_DBG("RECV_CTRL: blocking...");
	r = ioctl(fd, TBACCEL_IOCTL_RECV_CTRL, &req);
	if (r < 0) {
		int e = errno;

		TBX_ERR("RECV_CTRL failed: %s", strerror(e));
		errno = e;
		return r;
	}
	*slot = req.slot;
	*len = req.len;
	TBX_DBG("RECV_CTRL -> slot=%llu len=%u", (unsigned long long) *slot, *len);
	return 0;
}

static inline int tbx_wait_fence(int fence_fd, int timeout_ms)
{
	struct pollfd p = { .fd = fence_fd, .events = POLLIN };
	int r;

	TBX_DBG("wait fence fd=%d timeout=%dms ...", fence_fd, timeout_ms);
	do {
		r = poll(&p, 1, timeout_ms);
	} while (r < 0 && errno == EINTR);
	if (r < 0) {
		TBX_ERR("wait fence fd=%d: poll failed: %s", fence_fd, strerror(errno));
		return -1;
	}
	if (r == 0) {
		TBX_ERR("wait fence fd=%d: TIMEOUT after %dms", fence_fd, timeout_ms);
		return -1;
	}
	TBX_INFO("wait fence fd=%d: signalled", fence_fd);
	return 0;
}

/* typed control message helpers */

static inline int tbx_send_msg(int fd, uint64_t slot, uint32_t type, uint64_t id,
			       uint64_t a, uint64_t b, uint32_t m, uint32_t n, uint32_t k, uint32_t dtype)
{
	struct tbx_msg msg = { TBX_MAGIC, type, id, a, b, m, n, k, dtype };

	TBX_INFO("send %s id=%llu a=0x%llx b=0x%llx m=%u n=%u k=%u (to slot=%llu)",
		 tbx_type_name(type), (unsigned long long) id, (unsigned long long) a,
		 (unsigned long long) b, m, n, k, (unsigned long long) slot);
	return tbx_send_ctrl(fd, slot, &msg, sizeof(msg));
}

/* returns 0 = got a valid message, 1 = skipped garbage, -1 = error */
static inline int tbx_recv_msg(int fd, struct tbx_msg *out, uint64_t *slot)
{
	struct tbx_msg msg = {0};
	uint32_t len = 0;

	if (tbx_recv_ctrl(fd, &msg, sizeof(msg), slot, &len) < 0)
		return -1;
	if (len < sizeof(msg) || msg.magic != TBX_MAGIC) {
		TBX_ERR("RECV_CTRL: dropping malformed message (len=%u magic=0x%08x, want len>=%zu)",
			len, msg.magic, sizeof(msg));
		return 1;
	}
	*out = msg;
	TBX_INFO("recv %s id=%llu a=0x%llx b=0x%llx m=%u n=%u k=%u dtype=%u (slot=%llu)",
		 tbx_type_name(msg.type), (unsigned long long) msg.id,
		 (unsigned long long) msg.a, (unsigned long long) msg.b,
		 msg.m, msg.n, msg.k, msg.dtype, (unsigned long long) *slot);
	return 0;
}
