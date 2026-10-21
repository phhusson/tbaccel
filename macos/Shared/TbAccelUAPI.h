/*
 * TbAccelUAPI.h - userspace/kernel ABI for the macOS tbaccel driver.
 *
 * Byte-for-byte the same as linux/thunderbolt_accel.h (the real UAPI header
 * that now ships with the Linux driver). Where Linux issues
 * ioctl(fd, TBACCEL_IOCTL_*, &req), macOS issues
 * IOConnectCallStructMethod(conn, kTbAccelMethod*, ...).
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef TBACCEL_UAPI_H
#define TBACCEL_UAPI_H

#include <stdint.h>

#define TBACCEL_ABI_VERSION   1u

/* Maximum opaque control blob carried by one submit. */
#define TBACCEL_MAX_CTRL      4096u

/* No dma-buf / region / slot. */
#define TBACCEL_NONE          0ull

/* Frame size used by the NHI rings; the tbaccel wire header assumes it. */
#define TB_MAX_FRAME_SIZE     4096u

struct tbaccel_info {
    uint32_t abi_version;   /* OUT */
    uint32_t max_ctrl;      /* OUT: largest accepted control blob */
    uint32_t frame_size;    /* OUT: max bytes per on-wire frame */
    uint32_t pad;
    uint64_t features;      /* OUT: TBACCEL_FEAT_* */
};

#define TBACCEL_FEAT_FENCES      (1ull << 0)
#define TBACCEL_FEAT_RECV_CTRL   (1ull << 1)
#define TBACCEL_FEAT_ZEROCOPY    (1ull << 2)

struct tbaccel_import {
    uint32_t fd;            /* IN:  dma-buf fd (macOS: unused) */
    uint32_t flags;         /* IN:  reserved, must be 0 */
    uint64_t handle;        /* OUT: buffer handle */
    uint64_t size;          /* OUT: buffer size in bytes */
};

struct tbaccel_region {
    uint64_t handle;        /* IN:  buffer handle */
    uint64_t offset;        /* IN:  byte offset (must be 4096-aligned) */
    uint64_t size;          /* IN:  region size in bytes */
    uint64_t region;        /* OUT: region id */
};

struct tbaccel_free {
    uint64_t id;            /* IN:  region or buffer handle */
    uint32_t flags;         /* IN:  TBACCEL_FREE_* */
    uint32_t pad;
};

#define TBACCEL_FREE_REGION     0u
#define TBACCEL_FREE_BUFFER     1u

struct tbaccel_post_recv {
    uint64_t region;        /* IN:  local region to receive into */
    uint32_t flags;         /* IN:  reserved */
    uint32_t pad;
    uint64_t slot;          /* OUT: peer-visible slot id */
    int32_t  fence_fd;      /* OUT: sync_file (macOS: slot id) */
    uint32_t pad2;
};

struct tbaccel_submit {
    uint64_t slot;          /* IN:  peer slot id */
    uint64_t region;        /* IN:  local source region, or TBACCEL_NONE */
    uint64_t ctrl;          /* IN:  user pointer to opaque control blob */
    uint32_t ctrl_len;      /* IN:  blob length in bytes */
    uint32_t flags;         /* IN:  reserved */
    int32_t  in_fence_fd;   /* IN:  sync_file to wait for, or -1 */
    int32_t  out_fence_fd;  /* OUT: sync_file (macOS: slot id) */
};

struct tbaccel_recv_ctrl {
    uint64_t buf;           /* IN:  user pointer to receive the blob */
    uint32_t len;           /* IN:  buffer size; OUT: bytes written */
    uint32_t flags;         /* IN:  reserved */
    uint64_t slot;          /* OUT: slot the peer targeted, or TBACCEL_NONE */
};

struct tbaccel_send_ctrl {
    uint64_t slot;          /* IN:  peer slot / correlation id */
    uint64_t buf;           /* IN:  user pointer to the blob */
    uint32_t len;           /* IN:  blob length in bytes */
    uint32_t flags;         /* IN:  reserved */
};

/*
 * macOS external-method selectors. They mirror the ioctl numbering
 * (Linux uses _IOWR('B', 0x00..0x07, ...)) so the two APIs stay in step.
 */
/* Convenience typedefs so `tbaccel_info` and `struct tbaccel_info` both work. */
typedef struct tbaccel_info       tbaccel_info;
typedef struct tbaccel_import     tbaccel_import;
typedef struct tbaccel_region     tbaccel_region;
typedef struct tbaccel_free       tbaccel_free;
typedef struct tbaccel_post_recv  tbaccel_post_recv;
typedef struct tbaccel_submit     tbaccel_submit;
typedef struct tbaccel_recv_ctrl  tbaccel_recv_ctrl;
typedef struct tbaccel_send_ctrl  tbaccel_send_ctrl;

enum {
    kTbAccelMethodGetInfo   = 0x00,
    kTbAccelMethodImport    = 0x01,
    kTbAccelMethodRegion    = 0x02,
    kTbAccelMethodFree      = 0x03,
    kTbAccelMethodPostRecv  = 0x04,
    kTbAccelMethodSubmit    = 0x05,
    kTbAccelMethodRecvCtrl  = 0x06,
    kTbAccelMethodSendCtrl  = 0x07,
    kTbAccelMethodWaitSlot  = 0x08,   /* macOS-only: block on a slot id */

    /* macOS extensions for the ANE/MTL coop path (see docs). */
    kTbAccelMethodImportSurface = 0x40, /* import an IOSurface-backed region */
    kTbAccelMethodEvent         = 0x41, /* create/signal/wait/destroy an event  */
    kTbAccelMethodDrain         = 0x42, /* macOS-only: wait until TX has drained */
    kTbAccelMethodReset         = 0x43, /* macOS-only: tear the fabric down and redo bring-up */
};

/*
 * macOS extension: import an IOSurface (or any task-mapped page-aligned buffer)
 * as a tbaccel buffer. The surface is referenced by the task virtual address of
 * its base plus its allocation size; the kernel builds an IOMemoryDescriptor
 * over those pages, which is exactly what the ANE/Metal backend already holds.
 */
struct tbaccel_import_surface {
    uint64_t addr;          /* IN:  task virtual address of the surface base */
    uint64_t size;          /* IN:  allocation size (page aligned) */
    uint32_t surface_id;    /* IN:  IOSurfaceID (for debugging / peer share) */
    uint32_t flags;         /* IN:  reserved */
    uint64_t handle;        /* OUT: buffer handle */
};

#define TBACCEL_EVENT_CREATE  0u
#define TBACCEL_EVENT_SIGNAL  1u
#define TBACCEL_EVENT_WAIT    2u
#define TBACCEL_EVENT_DESTROY 3u

/*
 * A tbaccel event is the driver-side mirror of an MTLSharedEvent: a monotonically
 * increasing 64-bit signaled value. User space waits with WAIT and the peer's
 * signal is delivered through the protocol. This lets the same code that today
 * encodes waitForEvent/signalEvent against Metal use the link instead.
 */
struct tbaccel_event {
    uint32_t op;            /* IN:  TBACCEL_EVENT_* */
    uint32_t timeout_ms;    /* IN:  WAIT only */
    uint64_t id;            /* IN/OUT: event id (CREATE returns it) */
    uint64_t value;         /* IN:  value to signal / wait for */
};

typedef struct tbaccel_import_surface tbaccel_import_surface;
typedef struct tbaccel_event          tbaccel_event;

#endif /* TBACCEL_UAPI_H */
