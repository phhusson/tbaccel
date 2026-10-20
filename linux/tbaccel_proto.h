// SPDX-License-Identifier: GPL-2.0
/*
 * tbaccel control protocol (v1) - sits on top of the tbaccel UAPI.
 *
 * Two message classes:
 *   - out-of-band control (TBACCEL_IOCTL_SEND_CTRL / RECV_CTRL):
 *       PREPARE / PREPARED, RESULT
 *   - tied to a data transfer (the ctrl that travels with SUBMIT, delivered
 *     on DONE): LOAD, MATMUL
 *
 * Typical matmul (client = requester, peer = compute side):
 *
 *   client SEND_CTRL PREPARE{id=W, size, align}
 *   peer   SEND_CTRL PREPARED{id=W, handle=W, slot=Sw}
 *   client SUBMIT{slot=Sw, region=W, ctrl=LOAD{id=W}}
 *   client SEND_CTRL PREPARE{id=X, size, align}
 *   peer   SEND_CTRL PREPARED{id=X, handle=X, slot=Sx}
 *   client POST_RECV(Y) -> Sy, fence
 *   client SUBMIT{slot=Sx, region=X, ctrl=MATMUL{W, Sy, m,n,k,dtype}}
 *   peer   runs Y = W * X, then SUBMIT{slot=Sy, region=Y, ctrl=RESULT}
 *   client waits on Sy's fence
 */

#pragma once

#include <stdint.h>

#define TBX_MAGIC   0x31584254u  /* "TBX1" */

enum tbx_type {
	TBX_PREPARE  = 1,  /* client -> peer: reserve buffer id of `size` bytes */
	TBX_PREPARED = 2,  /* peer -> client: {a=link handle, b=slot}          */
	TBX_LOAD     = 3,  /* client -> peer (with data): store weights as id  */
	TBX_MATMUL   = 4,  /* client -> peer (with data): Y = W * X            */
	TBX_RESULT   = 5,  /* peer -> client: result written to slot b         */
};

struct tbx_msg {
	uint32_t magic;
	uint32_t type;
	uint64_t id;    /* buffer/tensor id (PREPARE/PREPARED/LOAD) */
	uint64_t a;     /* PREPARE: size      | PREPARED: handle | MATMUL: w_handle */
	uint64_t b;     /* PREPARE: alignment | PREPARED: slot   | MATMUL/RESULT: y_slot */
	uint32_t m;     /* matmul rows (M) */
	uint32_t n;     /* matmul cols (N) */
	uint32_t k;     /* matmul inner (K) */
	uint32_t dtype; /* element type (ggml_type value) */
};

/* F32 for the prototype */
#define TBX_DTYPE_F32 0
