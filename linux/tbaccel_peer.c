// SPDX-License-Identifier: GPL-2.0
/*
 * tbaccel peer daemon: the compute side.
 *
 * Reads control messages from /dev/tbaccelN, reserves buffers on request
 * (PREPARE), and runs the matmul when an op arrives (the op rides the SUBMIT
 * that delivered the input, so by the time we see it the data has landed).
 *
 * peer_run_matmul() is the reference implementation; that is where ggml goes.
 *
 *   ./tbaccel_peer [dev]        (default /dev/tbaccel0)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbx_lib.h"

#define MAX_BUFS 32

struct pbuf {
	uint64_t id;
	uint64_t region;
	uint64_t slot;
	int      dmabuf_fd;
	void    *ptr;
	size_t   size;
	int      loaded;
};

static struct pbuf bufs[MAX_BUFS];
static int nbufs;
static struct pbuf ybuf;	/* peer-side result buffer, grown on demand */

static struct pbuf *find_id(uint64_t id)
{
	for (int i = 0; i < nbufs; i++)
		if (bufs[i].id == id)
			return &bufs[i];
	return NULL;
}

static struct pbuf *find_slot(uint64_t slot)
{
	for (int i = 0; i < nbufs; i++)
		if (bufs[i].slot == slot)
			return &bufs[i];
	return NULL;
}

/* allocate a dma-buf, import it, carve a region */
static int buf_alloc(struct pbuf *b, int fd, size_t size)
{
	uint64_t handle = 0, isize = 0;

	memset(b, 0, sizeof(*b));
	b->dmabuf_fd = tbx_alloc_dmabuf(size, &b->ptr);
	if (b->dmabuf_fd < 0) {
		perror("dma-buf alloc");
		return -1;
	}
	b->size = size;
	if (tbx_import(fd, b->dmabuf_fd, &handle, &isize) < 0) {
		perror("IMPORT");
		return -1;
	}
	if (tbx_region(fd, handle, 0, size, &b->region) < 0) {
		perror("REGION");
		return -1;
	}
	return 0;
}

/* ---- reserve a peer buffer and post it to receive ----------------------- */

static int peer_prepare(int fd, uint64_t id, size_t size)
{
	struct pbuf *b = &bufs[nbufs];
	int fence_fd = -1;

	if (nbufs >= MAX_BUFS)
		return -1;
	if (buf_alloc(b, fd, size))
		return -1;
	b->id = id;
	if (tbx_post_recv(fd, b->region, &b->slot, &fence_fd) < 0) {
		perror("POST_RECV");
		return -1;
	}
	close(fence_fd);
	nbufs++;

	if (tbx_send_msg(fd, id, TBX_PREPARED, id, id /* handle */, b->slot, 0, 0, 0, 0))
		TBX_ERR("peer: SEND_CTRL PREPARED failed");
	TBX_INFO("peer: prepared id=%llu size=%zu dmabuf_fd=%d slot=%llu",
		 (unsigned long long) id, size, b->dmabuf_fd, (unsigned long long) b->slot);
	return 0;
}

/* ---- reference compute (swap for ggml) ---------------------------------- */

static void peer_run_matmul(float *y, const float *x, const float *w,
			    uint32_t M, uint32_t N, uint32_t K)
{
	for (uint32_t m = 0; m < M; m++)
		for (uint32_t n = 0; n < N; n++) {
			float acc = 0.0f;

			for (uint32_t k = 0; k < K; k++)
				acc += x[m * K + k] * w[n * K + k];
			y[m * N + n] = acc;
		}
}

/* send the computed ybuf back to the client's posted slot */
static int peer_send_result(int fd, uint64_t client_y_slot)
{
	struct tbx_msg res = { TBX_MAGIC, TBX_RESULT, 0, 0, client_y_slot, 0, 0, 0, 0 };
	int out_fence_fd;
	int r = tbx_submit(fd, client_y_slot, ybuf.region, &res, sizeof(res), -1, &out_fence_fd);

	if (r) {
		TBX_ERR("peer: SUBMIT result to slot %llu failed", (unsigned long long) client_y_slot);
		return -1;
	}
	TBX_INFO("peer: RESULT sent to slot %llu (region=%llu), out_fence_fd=%d",
		 (unsigned long long) client_y_slot, (unsigned long long) ybuf.region, out_fence_fd);
	close(out_fence_fd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/tbaccel0";
	int fd = open(dev, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		TBX_ERR("peer: cannot open %s: %s", dev, strerror(errno));
		return 1;
	}
	TBX_INFO("peer: using %s, waiting for control messages", dev);

	for (;;) {
		struct tbx_msg msg;
		uint64_t slot = 0;
		int r = tbx_recv_msg(fd, &msg, &slot);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			TBX_ERR("peer: RECV_CTRL failed: %s", strerror(errno));
			break;
		}
		if (r > 0)
			continue;

		switch (msg.type) {
		case TBX_PREPARE:
			TBX_INFO("peer: PREPARE id=%llu size=%llu", (unsigned long long) msg.id,
				 (unsigned long long) msg.a);
			peer_prepare(fd, msg.id, msg.a);
			break;

		case TBX_LOAD: {
			struct pbuf *b = find_id(msg.id);

			if (b)
				b->loaded = 1;
			TBX_INFO("peer: LOAD id=%llu (weights) %s", (unsigned long long) msg.id,
				 b ? "ok" : "BUT NO MATCHING BUFFER");
			break;
		}

		case TBX_MATMUL: {
			struct pbuf *w = find_id(msg.a);
			struct pbuf *x = find_slot(slot);	/* our input slot */

			if (!w || !x) {
				TBX_ERR("peer: MATMUL missing W(id=%llu %s) / X(input slot=%llu %s)",
					(unsigned long long) msg.a, w ? "found" : "NOT FOUND",
					(unsigned long long) slot, x ? "found" : "NOT FOUND");
				break;
			}
			if (ybuf.ptr == NULL ||
			    ybuf.size < (size_t) msg.m * msg.n * sizeof(float)) {
				if (ybuf.ptr) {
					munmap(ybuf.ptr, ybuf.size);
					close(ybuf.dmabuf_fd);
				}
				if (buf_alloc(&ybuf, fd, (size_t) msg.m * msg.n * sizeof(float))) {
					TBX_ERR("peer: cannot allocate result buffer");
					break;
				}
			}
			TBX_INFO("peer: MATMUL M=%u N=%u K=%u: computing into fd=%d, then SUBMIT to slot %llu",
				 msg.m, msg.n, msg.k, ybuf.dmabuf_fd, (unsigned long long) msg.b);
			peer_run_matmul(ybuf.ptr, x->ptr, w->ptr, msg.m, msg.n, msg.k);
			peer_send_result(fd, msg.b);
			break;
		}

		default:
			TBX_INFO("peer: ignoring %s", tbx_type_name(msg.type));
			break;
		}
	}
	close(fd);
	return 0;
}
