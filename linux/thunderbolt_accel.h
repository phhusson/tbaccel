/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Thunderbolt/USB4 accelerator offload interface (TBACCEL).
 *
 * This is a userspace API for shipping accelerator work over a Thunderbolt /
 * USB4 cable to a peer host and, more importantly, for sharing the *buffers*
 * and *synchronization* with the peer's kernel so that the whole thing looks
 * like a local accelerator instead of a socket.
 *
 * The model is deliberately small:
 *
 *   buffer   a dma-buf imported from userspace (llama.cpp allocates huge
 *            buffers and only a small tensor lives inside them)
 *   region   a {buffer, offset, size} sub-range - the unit that is transferred
 *   slot     a region the local host has posted to *receive* into; it carries
 *            an out-fence that fires when the peer's data has landed in it
 *   submit   the local host sends a small opaque control blob plus a source
 *            region to a peer slot, optionally waiting on an in-fence; it
 *            carries an out-fence that fires when the transfer is handed off
 *
 * The control blob is opaque to the kernel: it is whatever serialization the
 * userspace protocol wants (llama.cpp already serializes ops/graphs). The
 * kernel never parses it. This avoids inventing a "work id" grammar and keeps
 * the driver a pure DMA + fence mover.
 *
 * Fences are sync_file file descriptors (dma_fence), so the existing
 * ggml_backend_fence (an owned sync_file fd) plugs straight in.
 */

#ifndef _UAPI_LINUX_THUNDERBOLT_ACCEL_H
#define _UAPI_LINUX_THUNDERBOLT_ACCEL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define TBACCEL_ABI_VERSION      1

/* Maximum opaque control blob carried by one submit. */
#define TBACCEL_MAX_CTRL         4096

/* No dma-buf / region / slot. */
#define TBACCEL_NONE             0ULL

/* --- TBACCEL_GET_INFO ---------------------------------------------------- */

struct tbaccel_info {
	__u32 abi_version;      /* OUT */
	__u32 max_ctrl;         /* OUT: largest accepted control blob */
	__u32 frame_size;       /* OUT: max bytes per on-wire frame */
	__u32 pad;
	__u64 features;         /* OUT: TBACCEL_FEAT_* */
};

#define TBACCEL_FEAT_FENCES      (1ULL << 0)  /* sync_file in/out fences */
#define TBACCEL_FEAT_RECV_CTRL   (1ULL << 1)  /* control blobs can be read */
#define TBACCEL_FEAT_ZEROCOPY    (1ULL << 2)  /* regions DMA straight to/from the dma-buf */

/* --- buffers and regions ------------------------------------------------- */

struct tbaccel_import {
	__u32 fd;               /* IN:  dma-buf fd (owned by userspace) */
	__u32 flags;            /* IN:  reserved, must be 0 */
	__u64 handle;           /* OUT: buffer handle */
	__u64 size;             /* OUT: buffer size in bytes */
};

struct tbaccel_region {
	__u64 handle;           /* IN:  buffer handle */
	__u64 offset;           /* IN:  byte offset into the buffer */
	__u64 size;             /* IN:  region size in bytes */
	__u64 region;           /* OUT: region id */
};

struct tbaccel_free {
	__u64 id;               /* IN:  region or buffer handle */
	__u32 flags;            /* IN:  TBACCEL_FREE_* */
	__u32 pad;
};

#define TBACCEL_FREE_REGION     0
#define TBACCEL_FREE_BUFFER     1

/* --- receive side -------------------------------------------------------- */

struct tbaccel_post_recv {
	__u64 region;           /* IN:  local region to receive into */
	__u32 flags;            /* IN:  reserved */
	__u32 pad;
	__u64 slot;             /* OUT: peer-visible slot id */
	__s32 fence_fd;         /* OUT: sync_file, fires when data has landed */
	__u32 pad2;
};

/* --- send side ----------------------------------------------------------- */

struct tbaccel_submit {
	__u64 slot;             /* IN:  peer slot id (see tbaccel_recv_ctrl) */
	__u64 region;           /* IN:  local source region, or TBACCEL_NONE */
	__u64 ctrl;             /* IN:  user pointer to opaque control blob */
	__u32 ctrl_len;         /* IN:  blob length in bytes */
	__u32 flags;            /* IN:  reserved */
	__s32 in_fence_fd;      /* IN:  sync_file to wait for, or -1 */
	__s32 out_fence_fd;     /* OUT: sync_file, fires when the transfer is done */
};

struct tbaccel_recv_ctrl {
	__u64 buf;              /* IN:  user pointer to receive the blob */
	__u32 len;              /* IN:  buffer size; OUT: bytes written */
	__u32 flags;            /* IN:  reserved */
	__u64 slot;             /* OUT: slot the peer targeted, or TBACCEL_NONE */
};

/*
 * Send an opaque control blob with no associated data transfer. Used to
 * exchange link-scoped handles (e.g. "this is the handle of the weights you
 * just received") and any other out-of-band descriptor.
 */
struct tbaccel_send_ctrl {
	__u64 slot;             /* IN:  peer slot / correlation id */
	__u64 buf;              /* IN:  user pointer to the blob */
	__u32 len;              /* IN:  blob length in bytes */
	__u32 flags;            /* IN:  reserved */
};

/* --- ioctls -------------------------------------------------------------- */

#define TBACCEL_IOCTL_GET_INFO    _IOWR('B', 0x00, struct tbaccel_info)
#define TBACCEL_IOCTL_IMPORT      _IOWR('B', 0x01, struct tbaccel_import)
#define TBACCEL_IOCTL_REGION      _IOWR('B', 0x02, struct tbaccel_region)
#define TBACCEL_IOCTL_FREE        _IOW( 'B', 0x03, struct tbaccel_free)
#define TBACCEL_IOCTL_POST_RECV   _IOWR('B', 0x04, struct tbaccel_post_recv)
#define TBACCEL_IOCTL_SUBMIT      _IOWR('B', 0x05, struct tbaccel_submit)
#define TBACCEL_IOCTL_RECV_CTRL   _IOWR('B', 0x06, struct tbaccel_recv_ctrl)
#define TBACCEL_IOCTL_SEND_CTRL   _IOW( 'B', 0x07, struct tbaccel_send_ctrl)

#endif /* _UAPI_LINUX_THUNDERBOLT_ACCEL_H */
