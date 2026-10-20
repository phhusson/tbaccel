/*
 * TbAccelInternal.h - kernel-side data model shared by the tbaccel driver and
 * its user client. Mirrors the structures in linux/tbaccel.c (v2: credit-based
 * streaming).
 *
 * Records derive from OSObject so they can live in an OSArray.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef TBACCEL_INTERNAL_H
#define TBACCEL_INTERNAL_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>

#include "TbAccelUAPI.h"

class IOThunderboltReceiveCommand;

#define TBACCEL_MAGIC   0x54424143u /* "TBAC" */

/* control message types (identical to Linux) */
#define TBACCEL_MSG_ARM     0x10
#define TBACCEL_MSG_ACK     0x11
#define TBACCEL_MSG_DONE    0x12
#define TBACCEL_MSG_BLOB    0x13
#define TBACCEL_MSG_CREDIT  0x14

/* on-wire control header (identical to Linux struct tbaccel_wire) */
struct TbAccelWire {
    uint32_t magic;
    uint16_t type;
    uint16_t pad;
    uint64_t slot;
    uint32_t len;
    uint32_t payload_len;
} __attribute__((packed));

#define TBACCEL_CTRL_PAYLOAD (TB_MAX_FRAME_SIZE - sizeof(struct TbAccelWire))
#define TBACCEL_RING_SIZE    64
#define TBACCEL_WINDOW       (TBACCEL_RING_SIZE - 2)

/* Protocol Defined Frame codepoints.
 *
 * A Thunderbolt packet carries at most 256 bytes; a larger frame is a sequence
 * of packets delimited by the start-of-frame and end-of-frame codepoints in the
 * NHI header. Using the *same* codepoint for both (as tbaccel originally did,
 * SOF == EOF == 1) makes every 256-byte packet look like a complete frame, so
 * the hardware never reassembles multi-packet frames: frames above ~192 bytes
 * arrived as 256-byte pieces, 4 bytes short at the boundary, with
 * kIOReturnIOError. AppleThunderboltIP uses distinct values (1 and 2) and these
 * masks are what its receive queue is configured with. */
#define TBACCEL_PDF_SOF      1
#define TBACCEL_PDF_EOF      2
#define TBACCEL_PDF          TBACCEL_PDF_SOF   /* compatibility for frame stamping */

/* Payload bytes per data frame.
 *
 * TB_MAX_FRAME_SIZE (4096) is the ring's frame *capacity*, not the payload it
 * can deliver: a full-size frame comes back from the NHI split into two
 * completions (3840 + 252 = 4092) with kIOReturnIOError, i.e. four bytes per
 * frame are consumed by per-frame trailer/accounting and the remainder does not
 * fit. AppleThunderboltIP never sends a full-size frame either
 * (TBNET_MAX_PAYLOAD_SIZE = TB_MAX_FRAME_SIZE - frame header). Both ends of
 * tbaccel therefore use this chunk size; 4032 keeps a 64-byte aligned margin. */
#define TBACCEL_DATA_CHUNK    64u

/* Outbound frames must match what the peer arms with: Linux's tbaccel_arm_one()
 * now uses its `chunk` parameter, so both directions run with the same proven
 * frame size. Kept as a separate name because the two directions can diverge
 * again once the frame-size ceiling is understood. */
#define TBACCEL_TX_CHUNK      3840u

/* NHI ring size used for setRingSize(). AppleThunderboltIP uses 0x1000
 * (4096) here; the HAL creates a per-hop mapper sized by this, and a small
 * ring (64) fails to map -> allocate()/configure() fails. */
#define TBACCEL_NHI_RING_SIZE 4096

/* The peer's XDomain "path" hop id. Linux's tbaccel reports
 * out_hopid == in_hopid (its tb_xdomain_alloc_{in,out}_hopid both land on the
 * first free hop above the reserved range), while the DMA rings sit on the
 * NHI-assigned hop (2). In the fabric the hop carried in the frame is the
 * destination hop, so a path towards the peer must name this id - not our own
 * ring hop. Overridable at load time if the peer picks a different id. */
#define TBACCEL_PEER_PATH_HOP 9

/* Hop id this side advertises as its in/out path hop. Must be >= 8: 0..7 are
 * reserved (Linux's tbaccel enforces TBACCEL_HOPID_MIN = 8, and the family
 * rejects reserved ids, which is why every {2,*}/{*,2} path attempt failed).
 * We publish this as "inhopid"/"outhopid" in the XDomain service directory and
 * the peer mirrors it (its in = our out, its out = our in). */
#define TBACCEL_PATH_HOP 9

/* Shown as TbAccelBuildTag so the running build is identifiable from ioreg. */
#define TBACCEL_BUILD_TAG "peer-user"

/* NHI ring flags (mirror of Linux RING_FLAG_*; the family's ring manager
 * branches on bit0 = frame mode, bit1 = E2E). */
#define TBACCEL_RING_FLAG_FRAME  0x1
#define TBACCEL_RING_FLAG_E2E    0x2

/* ---------------------------------------------------------------- TBX1 ops
 *
 * The operation protocol that rides on top of tbaccel, identical to
 * linux/tbaccel_proto.h. PREPARE/PREPARED travel as BLOB control frames;
 * LOAD/MATMUL ride the ARM that carries the payload they operate on, and the
 * peer answers a MATMUL by ARMs its result into the client's posted slot.
 *
 * Used here so this kext can act as the *peer* (compute side) with a stock
 * Linux tbaccel_client, and later by the Mac client path.
 */
#define TBX_MAGIC 0x31584254u /* "TBX1" */

enum tbx_type {
    TBX_PREPARE  = 1,
    TBX_PREPARED = 2,
    TBX_LOAD     = 3,
    TBX_MATMUL   = 4,
    TBX_RESULT   = 5,
};

#define TBX_DTYPE_F32 0

struct TbxMsg {
    uint32_t magic;
    uint32_t type;
    uint64_t id;
    uint64_t a;
    uint64_t b;
    uint32_t m;
    uint32_t n;
    uint32_t k;
    uint32_t dtype;
} __attribute__((packed));

class TbAccelBuffer : public OSObject {
    OSDeclareDefaultStructors(TbAccelBuffer)
public:
    void free() APPLE_KEXT_OVERRIDE;
    uint64_t                  handle;
    IOBufferMemoryDescriptor *md;
    uint64_t                  size;
};

class TbAccelRegion : public OSObject {
    OSDeclareDefaultStructors(TbAccelRegion)
public:
    uint64_t        id;
    TbAccelBuffer  *buffer;
    uint64_t        offset;
    uint64_t        size;
};

class TbAccelSlot : public OSObject {
    OSDeclareDefaultStructors(TbAccelSlot)
public:
    uint64_t        id;
    TbAccelRegion  *region;
    uint64_t        total;       /* bytes to receive */
    uint64_t        armed_off;   /* next region-relative offset to arm */
    bool            signaled;
    /* Peer-side state (only set when isPeer): the TBX buffer id this slot was
     * PREPAREd for, the operation that rides the ARM that fills it, and the
     * time the transfer started, for latency accounting. */
    bool            isPeer;
    uint64_t        peerId;
    uint8_t         op[64];
    uint32_t        opLen;
    uint64_t        t0;
};

class TbAccelBlob : public OSObject {
    OSDeclareDefaultStructors(TbAccelBlob)
public:
    uint64_t slot;
    uint32_t len;
    uint8_t  data[TBACCEL_CTRL_PAYLOAD];
};

class TbAccelAck : public OSObject {
    OSDeclareDefaultStructors(TbAccelAck)
public:
    uint64_t slot;
    uint32_t len;
    uint64_t total;
};

/* one transmit transfer: counts outstanding frames for a SUBMIT */
class TbAccelXfer : public OSObject {
    OSDeclareDefaultStructors(TbAccelXfer)
public:
    uint32_t      frames;
    uint32_t      status;
    TbAccelSlot  *outSlot;   /* signaled when all tx frames have drained */
};

/* a control RX frame: a page descriptor plus the command that carries it */
class TbAccelCtrlRx : public OSObject {
    OSDeclareDefaultStructors(TbAccelCtrlRx)
public:
    IOBufferMemoryDescriptor   *md;
    IOThunderboltReceiveCommand *cmd;
};

/* driver-side mirror of an MTLSharedEvent */
class TbAccelEvent : public OSObject {
    OSDeclareDefaultStructors(TbAccelEvent)
public:
    uint64_t id;
    uint64_t signaled;
};

/* Argument blocks passed through IOCommandGate::runAction(). */
struct TbAccelSubmitArgs   { tbaccel_submit req; const void *blob; uint32_t blobLen; };
struct TbAccelRecvCtrlArgs { tbaccel_recv_ctrl req; void *out; uint32_t cap; };
struct TbAccelSendCtrlArgs { tbaccel_send_ctrl req; const void *blob; };
struct TbAccelWaitSlotArgs { uint64_t slot; uint32_t timeout; };
struct TbAccelMapBufferArgs{ uint64_t handle; IOMemoryDescriptor **md; };

#endif /* TBACCEL_INTERNAL_H */
