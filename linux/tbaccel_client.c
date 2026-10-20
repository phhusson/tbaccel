// SPDX-License-Identifier: GPL-2.0
/*
 * tbaccel client: the requester. Runs one matmul Y = X * W against the peer.
 *
 *   ./tbaccel_client [dev] [M] [N] [K] [iters]
 *       (default /dev/tbaccel0 4 8 16 1)
 *
 * With iters > 1 the weights and input stay loaded and only the matmul is
 * repeated, timing SUBMIT -> result fence for each iteration and printing
 * min/p50/p90/p99/max.  That is the steady state a real pipeline sees, and it
 * is the number to compare against later zero-copy/MTLSharedEvent work.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbx_lib.h"

#define ID_W 1
#define ID_X 2

/* wait for a PREPARED message with the given id */
static int expect_prepared(int fd, uint64_t id, uint64_t *handle, uint64_t *slot)
{
	TBX_INFO("client: waiting for PREPARED id=%llu", (unsigned long long) id);
	for (;;) {
		struct tbx_msg msg;
		uint64_t s = 0;
		int r = tbx_recv_msg(fd, &msg, &s);

		if (r < 0) {
			TBX_ERR("client: RECV_CTRL failed while waiting for PREPARED id=%llu",
				(unsigned long long) id);
			return -1;
		}
		if (r > 0)
			continue;
		if (msg.type == TBX_PREPARED && msg.id == id) {
			*handle = msg.a;
			*slot = msg.b;
			TBX_INFO("client: PREPARED id=%llu -> handle=%llu slot=%llu",
				 (unsigned long long) id, (unsigned long long) *handle,
				 (unsigned long long) *slot);
			return 0;
		}
		TBX_INFO("client: ignoring %s id=%llu while waiting for PREPARED id=%llu",
			 tbx_type_name(msg.type), (unsigned long long) msg.id,
			 (unsigned long long) id);
	}
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/tbaccel0";
	uint32_t M = argc > 2 ? strtoul(argv[2], NULL, 0) : 4;
	uint32_t N = argc > 3 ? strtoul(argv[3], NULL, 0) : 8;
	uint32_t K = argc > 4 ? strtoul(argv[4], NULL, 0) : 16;
	unsigned iters = argc > 5 ? (unsigned) strtoul(argv[5], NULL, 0) : 1;
	size_t w_size = (size_t) N * K * sizeof(float);
	size_t x_size = (size_t) M * K * sizeof(float);
	size_t y_size = (size_t) M * N * sizeof(float);
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	int w_fd, x_fd, y_fd;
	void *w_ptr, *x_ptr, *y_ptr;
	uint64_t w_peer, x_peer;			/* peer-side handles from PREPARED */
	uint64_t w_local, x_local, y_local;		/* our local import handles   */
	uint64_t w_region, x_region, y_region;
	uint64_t w_slot, in_slot, y_slot;
	uint64_t ignored;
	int y_fence, out_fence;
	int ret = 1;

	if (fd < 0) {
		TBX_ERR("client: cannot open %s: %s", dev, strerror(errno));
		return 1;
	}
	TBX_INFO("client: opened %s  M=%u N=%u K=%u  w=%zu x=%zu y=%zu bytes",
		 dev, M, N, K, w_size, x_size, y_size);

	/* ---- weights: prepare on peer, then ship once ---- */
	TBX_INFO("client: step 1/4: prepare weights on peer");
	if (tbx_send_msg(fd, ID_W, TBX_PREPARE, ID_W, w_size, 4096, 0, 0, 0, 0)) {
		TBX_ERR("client: PREPARE W failed");
		goto out;
	}
	if (expect_prepared(fd, ID_W, &w_peer, &w_slot)) {
		fprintf(stderr, "client: no PREPARED for W\n");
		goto out;
	}
	w_fd = tbx_alloc_dmabuf(w_size, &w_ptr);
	if (w_fd < 0 || tbx_import(fd, w_fd, &w_local, &ignored) < 0) {
		perror("W buf");
		goto out;
	}
	tbx_region(fd, w_local, 0, w_size, &w_region);
	{
		float *w = w_ptr;

		for (uint32_t n = 0; n < N; n++)
			for (uint32_t k = 0; k < K; k++)
				w[n * K + k] = (float) (n + 1) - 0.5f * k;
	}
	{
		struct tbx_msg ld = { TBX_MAGIC, TBX_LOAD, ID_W, 0, 0, 0, 0, 0, 0 };

		if (tbx_submit(fd, w_slot, w_region, &ld, sizeof(ld), -1, &out_fence)) {
			perror("SUBMIT W");
			goto out;
		}
		if (tbx_wait_fence(out_fence, 3000)) {
			fprintf(stderr, "client: weights submit timed out\n");
			goto out;
		}
		close(out_fence);
	}

	TBX_INFO("client: step 2/4: prepare input on peer");
	/* ---- input: prepare on peer ---- */
	if (tbx_send_msg(fd, ID_X, TBX_PREPARE, ID_X, x_size, 4096, 0, 0, 0, 0)) {
		TBX_ERR("client: PREPARE X failed");
		goto out;
	}
	if (expect_prepared(fd, ID_X, &x_peer, &in_slot)) {
		fprintf(stderr, "client: no PREPARED for X\n");
		goto out;
	}
	x_fd = tbx_alloc_dmabuf(x_size, &x_ptr);
	if (x_fd < 0 || tbx_import(fd, x_fd, &x_local, &ignored) < 0) {
		perror("X buf");
		goto out;
	}
	tbx_region(fd, x_local, 0, x_size, &x_region);
	{
		float *x = x_ptr;

		for (uint32_t m = 0; m < M; m++)
			for (uint32_t k = 0; k < K; k++)
				x[m * K + k] = (float) (m + 1) + 0.25f * k;
	}

	/* ---- output: local buffer, posted to receive the result ---- */
	y_fd = tbx_alloc_dmabuf(y_size, &y_ptr);
	if (y_fd < 0 || tbx_import(fd, y_fd, &y_local, &ignored) < 0) {
		perror("Y buf");
		goto out;
	}
	tbx_region(fd, y_local, 0, y_size, &y_region);
	if (tbx_post_recv(fd, y_region, &y_slot, &y_fence)) {
		perror("POST_RECV Y");
		goto out;
	}

	TBX_INFO("client: step 3/4: submit matmul (w_handle=%llu y_slot=%llu, input slot=%llu)",
		 (unsigned long long) w_peer, (unsigned long long) y_slot,
		 (unsigned long long) in_slot);
	/* ---- run the matmul ---- */
	{
		struct tbx_msg op = { TBX_MAGIC, TBX_MATMUL, 0, w_peer, y_slot, M, N, K, TBX_DTYPE_F32 };

		if (tbx_submit(fd, in_slot, x_region, &op, sizeof(op), -1, &out_fence)) {
			perror("SUBMIT X/matmul");
			goto out;
		}
		close(out_fence);
	}
	TBX_INFO("client: step 4/4: waiting for the result (y_slot fence fd=%d)", y_fence);
	if (tbx_wait_fence(y_fence, 5000)) {
		TBX_ERR("client: result fence did NOT arrive (peer did not send the result?)");
		goto out;
	}

	/* ---- verify ---- */
	{
		const float *x = x_ptr, *w = w_ptr, *y = y_ptr;
		int bad = 0;

		for (uint32_t m = 0; m < M && !bad; m++)
			for (uint32_t n = 0; n < N; n++) {
				float acc = 0.0f;

				for (uint32_t k = 0; k < K; k++)
					acc += x[m * K + k] * w[n * K + k];
				if (fabsf(y[m * N + n] - acc) > 1e-3f) {
					TBX_ERR("client: mismatch y[%u][%u]=%f want %f",
						m, n, y[m * N + n], acc);
					bad = 1;
					break;
				}
			}
		if (bad)
			goto out;
	}

	/* consume the RESULT control message */
	{
		struct tbx_msg msg;
		uint64_t s = 0;
		int r = tbx_recv_msg(fd, &msg, &s);

		if (r == 0 && msg.type == TBX_RESULT)
			TBX_INFO("client: RESULT received (slot %llu)", (unsigned long long) msg.b);
		else if (r == 0)
			TBX_INFO("client: unexpected %s after matmul", tbx_type_name(msg.type));
		else
			TBX_ERR("client: no RESULT control message from peer");
	}

	printf("OK: %ux%ux%u matmul verified over tbaccel\n", M, N, K);

	if (iters > 1) {
		uint64_t *s = calloc(iters, sizeof(*s));
		unsigned done = 0, i, j;

		for (i = 0; i < iters && s; i++) {
			struct tbx_msg op = { TBX_MAGIC, TBX_MATMUL, 0, w_peer, 0,
					      M, N, K, TBX_DTYPE_F32 };
			struct timespec t0, t1;
			uint64_t y_slot2;
			int y_fence2 = -1, of = -1, r;

			if (tbx_post_recv(fd, y_region, &y_slot2, &y_fence2)) {
				perror("POST_RECV");
				break;
			}
			op.b = y_slot2;

			clock_gettime(CLOCK_MONOTONIC, &t0);
			r = tbx_submit(fd, in_slot, x_region, &op, sizeof(op), -1, &of);
			if (r) {
				close(y_fence2);
				break;
			}
			r = tbx_wait_fence(y_fence2, 5000);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			close(of);
			close(y_fence2);
			if (r)
				break;
			s[done++] = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000ull
				  + (uint64_t)(t1.tv_nsec - t0.tv_nsec) / 1000ull;

			{	/* consume the RESULT control message */
				struct tbx_msg m;
				uint64_t sl = 0;

				tbx_recv_msg(fd, &m, &sl);
			}
		}

		if (done) {
			for (i = 1; i < done; i++) {
				uint64_t v = s[i];

				for (j = i; j > 0 && s[j - 1] > v; j--)
					s[j] = s[j - 1];
				s[j] = v;
			}
			printf("latency over %u iterations (submit -> result fence):\n", done);
			printf("  min %llu us  p50 %llu us  p90 %llu us  p99 %llu us  max %llu us\n",
			       (unsigned long long) s[0],
			       (unsigned long long) s[done / 2],
			       (unsigned long long) s[(done * 9) / 10],
			       (unsigned long long) s[(done * 99) / 100],
			       (unsigned long long) s[done - 1]);
			if (done != iters)
				printf("  (stopped after %u of %u)\n", done, iters);
		} else {
			printf("latency loop: no iterations completed\n");
		}
		free(s);
	}

	ret = 0;

out:
	close(fd);
	return ret;
}
