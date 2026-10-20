/*
 * TbAccelProtocol.h - the txaccel control protocol (v1).
 *
 * Byte-for-byte the same as linux/tbaccel_proto.h.  The driver in macos/Kernel
 * and the Linux peer speak the same messages.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef TBACCEL_PROTOCOL_H
#define TBACCEL_PROTOCOL_H

#include <stdint.h>

#define TBX_MAGIC   0x31584254u  /* "TBX1" */

enum tbx_type {
    TBX_PREPARE  = 1,  /* client -> peer: reserve buffer id of `size` bytes  */
    TBX_PREPARED = 2,  /* peer -> client: {a=link handle, b=slot}            */
    TBX_LOAD     = 3,  /* client -> peer (with data): store weights as id    */
    TBX_MATMUL   = 4,  /* client -> peer (with data): Y = W * X              */
    TBX_RESULT   = 5,  /* peer -> client: result written to slot b           */
};

struct tbx_msg {
    uint32_t magic;
    uint32_t type;
    uint64_t id;    /* buffer/tensor id (PREPARE/PREPARED/LOAD)               */
    uint64_t a;     /* PREPARE: size | PREPARED: handle | MATMUL: w_handle    */
    uint64_t b;     /* PREPARE: align | PREPARED: slot | MATMUL/RESULT: y_slot*/
    uint32_t m;
    uint32_t n;
    uint32_t k;
    uint32_t dtype;
};

#define TBX_DTYPE_F32 0

#endif /* TBACCEL_PROTOCOL_H */
