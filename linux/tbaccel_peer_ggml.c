// SPDX-License-Identifier: GPL-2.0
/*
 * tbaccel_peer_ggml: the compute side, using ggml for allocation and compute.
 *
 * Buffers are allocated with ggml's DMA_BUF buffer type (the Vulkan device's
 * default buffer type in this fork), so the dma-buf fd can be handed to
 * tbaccel - incoming Thunderbolt data lands directly in the ggml tensor, and
 * the matmul runs on the ggml backend (Vulkan, else CPU).
 *
 *   ./tbaccel_peer_ggml [dev] [backend]   (default /dev/tbaccel0, Vulkan)
 *
 * Build (adjust the llama.cpp build dir):
 *   gcc -O2 -Wall -I$LLAMA/ggml/include \
 *       tbaccel_peer_ggml.c -L$LLAMA/build-vk-dmabuf/bin \
 *       -Wl,-rpath,$LLAMA/build-vk-dmabuf/bin -lggml -lggml-base -o tbaccel_peer_ggml
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-dmabuf.h"

#include "tbx_lib.h"

#define MAX_BUFS 32

struct gbuf {
	uint64_t    id;
	uint64_t    region;
	uint64_t    slot;
	int         fd;
	size_t      size;
	struct ggml_tensor * tensor;	/* base tensor (a 1D F32 view of the buffer) */
};

static struct gbuf bufs[MAX_BUFS];
static int nbufs;

static ggml_backend_t           backend;
static ggml_backend_buffer_type_t buft;

/* all tensors live in one context so views can reference the received bases */
static struct ggml_context * pctx;

static struct gbuf *find_id(uint64_t id)
{
	for (int i = 0; i < nbufs; i++)
		if (bufs[i].id == id)
			return &bufs[i];
	return NULL;
}

static struct gbuf *find_slot(uint64_t slot)
{
	for (int i = 0; i < nbufs; i++)
		if (bufs[i].slot == slot)
			return &bufs[i];
	return NULL;
}

static int peer_prepare(int fd, uint64_t id, size_t size)
{
	struct gbuf *b = &bufs[nbufs];
	uint64_t handle = 0, isize = 0;
	int fence_fd = -1;

	if (nbufs >= MAX_BUFS)
		return -1;

	/* a 1D F32 tensor over `size` bytes, allocated in a dma-buf */
	b->tensor = ggml_new_tensor_1d(pctx, GGML_TYPE_F32, size / sizeof(float));
	b->size   = size;
	ggml_backend_buffer_t buffer = ggml_backend_alloc_ctx_tensors_from_buft(pctx, buft);
	if (buffer == NULL) {
		TBX_ERR("peer: ggml alloc of %zu bytes from '%s' failed", size,
			ggml_backend_buft_name(buft));
		return -1;
	}
	b->fd = ggml_backend_dmabuf_buffer_fd(buffer);
	if (b->fd < 0) {
		TBX_ERR("peer: ggml buffer is not backed by a dma-buf (fd=%d)", b->fd);
		return -1;
	}

	b->id = id;
	if (tbx_import(fd, b->fd, &handle, &isize) < 0 ||
	    tbx_region(fd, handle, 0, size, &b->region) < 0 ||
	    tbx_post_recv(fd, b->region, &b->slot, &fence_fd) < 0) {
		TBX_ERR("peer: tbaccel import/region/post_recv failed");
		return -1;
	}
	close(fence_fd);
	nbufs++;

	tbx_send_msg(fd, id, TBX_PREPARED, id, id, b->slot, 0, 0, 0, 0);
	TBX_INFO("peer: prepared id=%llu size=%zu ggml_dmabuf_fd=%d slot=%llu",
		 (unsigned long long) id, size, b->fd, (unsigned long long) b->slot);
	return 0;
}

static int peer_matmul(int fd, uint64_t w_id, uint64_t input_slot, uint64_t y_slot,
		       uint32_t M, uint32_t N, uint32_t K)
{
	struct gbuf *w = find_id(w_id);
	struct gbuf *x = find_slot(input_slot);
	struct gbuf *y;
	struct ggml_tensor *tw, *tx, *ty;
	struct ggml_cgraph * graph;
	ggml_backend_buffer_t ybuf;
	uint64_t handle = 0, isize = 0, y_region = 0;
	int out_fence_fd;

	if (!w || !x) {
		TBX_ERR("peer: MATMUL missing W(id=%llu %s) / X(slot %llu %s, got %d buffers)",
			(unsigned long long) w_id, w ? "found" : "NOT FOUND",
			(unsigned long long) input_slot, x ? "found" : "NOT FOUND", nbufs);
		return -1;
	}

	/* views over the received buffers; ggml tensors are ne0-fastest, so
	 * W=[K,N] and X=[K,M] and the result [N,M] has the client's y[m*N+n] layout */
	TBX_INFO("peer: building graph W[%u,%u] x X[%u,%u] (W fd=%d, X fd=%d)",
		 K, N, K, M, w->fd, x->fd);
	tw = ggml_view_2d(pctx, w->tensor, K, N, K * sizeof(float), 0);
	tx = ggml_view_2d(pctx, x->tensor, K, M, K * sizeof(float), 0);
	ty = ggml_mul_mat(pctx, tw, tx);

	ybuf = ggml_backend_alloc_ctx_tensors_from_buft(pctx, buft);
	if (ybuf == NULL) {
		TBX_ERR("peer: result allocation (%u bytes) failed", M * N * (uint32_t) sizeof(float));
		return -1;
	}

	graph = ggml_new_graph(pctx);
	ggml_build_forward_expand(graph, ty);
	TBX_INFO("peer: ggml_backend_graph_compute on '%s'...", ggml_backend_name(backend));
	if (ggml_backend_graph_compute(backend, graph) != GGML_STATUS_SUCCESS) {
		TBX_ERR("peer: ggml graph compute FAILED");
		return -1;
	}
	ggml_backend_synchronize(backend);
	TBX_INFO("peer: compute done");

	/* ship the result back to the client's posted slot */
	y = &bufs[nbufs];	/* scratch entry for the result */
	if (nbufs >= MAX_BUFS)
		return -1;
	memset(y, 0, sizeof(*y));
	y->fd = ggml_backend_dmabuf_buffer_fd(ybuf);
	if (tbx_import(fd, y->fd, &handle, &isize) < 0 ||
	    tbx_region(fd, handle, 0, (size_t) M * N * sizeof(float), &y_region) < 0) {
		TBX_ERR("peer: tbaccel import/region of result fd=%d failed", y->fd);
		return -1;
	}

	{
		struct tbx_msg res = { TBX_MAGIC, TBX_RESULT, 0, 0, y_slot, 0, 0, 0, 0 };

		if (tbx_submit(fd, y_slot, y_region, &res, sizeof(res), -1, &out_fence_fd)) {
			TBX_ERR("peer: SUBMIT result to slot %llu failed", (unsigned long long) y_slot);
			return -1;
		}
		TBX_INFO("peer: RESULT sent to slot %llu (result fd=%d, out_fence_fd=%d)",
			 (unsigned long long) y_slot, y->fd, out_fence_fd);
		close(out_fence_fd);
	}

	ggml_backend_buffer_free(ybuf);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev        = argc > 1 ? argv[1] : "/dev/tbaccel0";
	const char *backend_nm = argc > 2 ? argv[2] : "Vulkan";
	int fd = open(dev, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		TBX_ERR("peer: cannot open %s: %s", dev, strerror(errno));
		return 1;
	}
	TBX_INFO("peer: opened %s", dev);

	ggml_backend_load_all();

	ggml_backend_dev_t dev_gpu = ggml_backend_dev_by_name(backend_nm);
	if (dev_gpu == NULL)
		dev_gpu = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
	if (dev_gpu == NULL) {
		fprintf(stderr, "peer: no ggml backend\n");
		return 1;
	}
	backend = ggml_backend_dev_init(dev_gpu, NULL);
	buft    = ggml_backend_dev_buffer_type(dev_gpu);
	TBX_INFO("peer: ggml backend=%s  buffer_type=%s  (needs DMA_BUF for tbaccel sharing)",
		 ggml_backend_dev_name(dev_gpu), ggml_backend_buft_name(buft));
	if (strcmp(ggml_backend_buft_name(buft), GGML_DMABUF_NAME) != 0)
		TBX_ERR("peer: buffer type is '%s', not '%s' - dma-buf sharing will not work",
			ggml_backend_buft_name(buft), GGML_DMABUF_NAME);

	pctx = ggml_init((struct ggml_init_params){
		.mem_size = 64 * 1024 * 1024,
		.mem_buffer = NULL,
		.no_alloc = true,
	});

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
		case TBX_LOAD:
			TBX_INFO("peer: LOAD id=%llu (weights)", (unsigned long long) msg.id);
			break;
		case TBX_MATMUL:
			TBX_INFO("peer: MATMUL w_handle=%llu y_slot=%llu M=%u N=%u K=%u (input slot=%llu)",
				 (unsigned long long) msg.a, (unsigned long long) msg.b,
				 msg.m, msg.n, msg.k, (unsigned long long) slot);
			peer_matmul(fd, msg.a, slot, msg.b, msg.m, msg.n, msg.k);
			break;
		default:
			TBX_INFO("peer: ignoring %s", tbx_type_name(msg.type));
			break;
		}
	}

	ggml_free(pctx);
	ggml_backend_free(backend);
	close(fd);
	return 0;
}
