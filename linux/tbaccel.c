// SPDX-License-Identifier: GPL-2.0
/*
 * tbaccel - Thunderbolt/USB4 accelerator offload driver.
 *
 * One XDomain service ("accel") per link, one character device (/dev/tbaccelN)
 * exposing tbaccel's UAPI (thunderbolt_accel.h).
 *
 * SINGLE RING: one TX and one RX HopID pair (2 HopIDs). Control messages and
 * data share each ring, ordered within a direction:
 *
 *   A -> B :  [ARM(op)] [data ...]        A's TX == B's RX
 *   B -> A :  [ACK]     [CREDIT ...]      B's TX == A's RX
 *
 * - RX data frames are zero-copy: their buffer_phy points into the receiving
 *   region's dma-buf pages (mapped for the NHI). RX control frames use a page.
 * - The receiver keeps exactly ONE control buffer armed while idle; on ARM it
 *   switches to arming the data window (region pages), then sends ACK. The
 *   sender does not stream until ACK, so the switch is race-free.
 * - There is no DONE frame: the receiver counts `total` bytes and completes.
 *   The op rides in ARM but is delivered to userspace only at completion.
 * - CREDITs are batched to keep the one-deep control posting safe.
 * - The allocated in/out HopIDs are published in the "accel" service
 *   properties ("inhopid"/"outhopid"). The peer mirrors them (its in is our
 *   out and vice versa) so both ends enable matching DMA paths.
 *
 * Trade-off (accepted, for the HopID budget): a station cannot emit a control
 * frame while its own TX is backed up with data. Fine for request/response;
 * would bite under full-duplex pipelining.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/uuid.h>
#include <linux/thunderbolt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/sync_file.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "thunderbolt_accel.h"

MODULE_IMPORT_NS("DMA_BUF");

#define TBACCEL_MAGIC		0x54424143	/* "TBAC" */

#define TBACCEL_MSG_ARM		0x10	/* len=total, pad2=oplen, payload=op */
#define TBACCEL_MSG_ACK		0x11	/* len=armed window, payload=u64 total */
#define TBACCEL_MSG_BLOB	0x13	/* pad2=len, payload=opaque */
#define TBACCEL_MSG_CREDIT	0x14	/* len=credited bytes */

/*
 * PDF codepoints. A Thunderbolt packet holds at most 256 bytes, so a frame
 * larger than that is a packet sequence delimited by these values. Using one
 * value for both (SOF == EOF) makes every packet look like a whole frame and
 * nothing above ~192 bytes can be reassembled; AppleThunderboltIP uses 1 and 2.
 */
#define TBACCEL_PDF_SOF		1
#define TBACCEL_PDF_EOF		2
#define TBACCEL_PDF		TBACCEL_PDF_SOF

/*
 * Payload bytes per data frame. TB_MAX_FRAME_SIZE is the ring's frame capacity,
 * not what the peer can deliver: large frames are rejected by the receiver
 * (kIOReturnIOError, no payload written). Module parameter so the threshold can
 * be found by sweeping without rebuilding, and logged at bring-up.
 *
 * The macOS receiver must be built with the same value (TBACCEL_DATA_CHUNK).
 */
/*
 * Send data frames from a module-owned DMA page instead of straight out of the
 * imported dma-buf. Control frames (which always use a page) are received fine
 * while data frames are rejected with kIOReturnIOError and no payload, and the
 * only structural difference is the source buffer - so this isolates a bad
 * dmabuf DMA mapping from a receiver-side problem.
 */
static bool bounce;
module_param(bounce, bool, 0644);
MODULE_PARM_DESC(bounce, "copy data frames through a DMA page (default 0)");

static unsigned int chunk_size = 1024;
module_param_named(chunk, chunk_size, uint, 0644);
MODULE_PARM_DESC(chunk, "payload bytes per transmitted data frame (default 1024)");

/*
 * Size of the descriptors this side arms for *incoming* data - i.e. the frame
 * size the peer must transmit with. Kept separate from `chunk` because the two
 * directions have different limits: this host can receive large frames from the
 * Mac, while the Mac's receive path tops out around 192 bytes.
 */
static unsigned int rx_chunk = 3840;
module_param_named(rx_chunk, rx_chunk, uint, 0644);
MODULE_PARM_DESC(rx_chunk, "descriptor size for received data frames (default 3840)");

static unsigned int ring_size = 64;
module_param(ring_size, uint, 0644);
MODULE_PARM_DESC(ring_size, "TX/RX ring size (default 64)");

/*
 * End-to-end (hop counter) flow control on the DMA rings.
 *
 * The Mac side of this prototype (com.phh.tbaccel) creates its queues with
 * E2E disabled and its paths carry no hop counters, so an E2E-tagged sender
 * is talking to a receiver that cannot account for the credits: the switch
 * happily routes frames one way (non-E2E -> E2E, which is why our control
 * probe showed up here) but not the other. Default this off so both ends
 * agree, and keep the knob for when the kext grows real counters.
 */
static bool e2e;
module_param(e2e, bool, 0644);
MODULE_PARM_DESC(e2e, "enable E2E hop-counter flow control on the rings (default 0)");

/* leave slack so the window never fills the ring */
#define TBACCEL_WINDOW		(ring_size - 2)
/* Batch credits: emit one CREDIT once this much has been re-armed. This has to
 * scale with the frame size: the receiver can only arm as many frames as it has
 * buffers (ring_size, one held for control), so with small frames the window is
 * a few KB and a 16 KB batch threshold would never be reached - the sender then
 * starves waiting for credit that is never reported. */
static inline u32 tbaccel_credit_batch(void)
{
	return chunk_size * 8;
}

struct tbaccel_wire {
	__le32 magic;
	__le16 type;
	__le16 pad;
	__le64 slot;
	__le32 len;
	__le32 pad2;
} __packed;

#define TBACCEL_CTRL_PAYLOAD	(TB_MAX_FRAME_SIZE - sizeof(struct tbaccel_wire))

/* ------------------------------------------------------------------ fences */

struct tbaccel_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static u64 tbaccel_fence_context;
static atomic_t tbaccel_fence_seq = ATOMIC_INIT(0);

static const char *tbaccel_fence_get_driver_name(struct dma_fence *f)
{
	return "tbaccel";
}

static const char *tbaccel_fence_get_timeline_name(struct dma_fence *f)
{
	return "tbaccel";
}

static const struct dma_fence_ops tbaccel_fence_ops = {
	.get_driver_name = tbaccel_fence_get_driver_name,
	.get_timeline_name = tbaccel_fence_get_timeline_name,
};

static struct dma_fence *tbaccel_fence_create(void)
{
	struct tbaccel_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &tbaccel_fence_ops, &f->lock,
		       tbaccel_fence_context, atomic_inc_return(&tbaccel_fence_seq));
	return &f->base;
}

static int tbaccel_fence_export(struct dma_fence *fence)
{
	struct sync_file *sf;
	int fd;

	sf = sync_file_create(fence);
	if (!sf)
		return -ENOMEM;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sf->file);
		return fd;
	}
	fd_install(fd, sf->file);
	return fd;
}

/* ------------------------------------------------------- buffers and friends */

struct tbaccel_buf {
	u64					handle;
	struct dma_buf				*dmabuf;
	struct dma_buf_attachment		*attach;
	struct sg_table				*sgt;
	struct iosys_map			vmap;	/* CPU view, for the bounce path */
	void					*vaddr;
	u64					size;
	struct list_head			list;
};

struct tbaccel_reg {
	u64			id;
	struct tbaccel_buf	*buf;
	u64			offset;
	u64			size;
	struct list_head	list;
};

struct tbaccel_slot {
	u64			id;
	struct tbaccel_reg	*region;
	struct dma_fence	*fence;
	struct list_head	list;
	u64			total;		/* bytes expected */
	u64			armed_off;	/* next region offset to arm */
	u8			*op;		/* op delivered at completion */
	u32			op_len;
	u64			t0;		/* ARM arrival, for latency accounting */
	bool			queued;		/* ARM deferred behind another stream */
};

struct tbaccel_blob {
	struct list_head	list;
	u64			slot;
	u32			len;
	u8			data[];
};

struct tbaccel_ack {
	struct list_head	list;
	u64			slot;
	u32			len;
	u64			total;
};

struct tbaccel_xfer {
	struct dma_fence	*fence;
	atomic_t		frames;
};

struct tbaccel_frame {
	struct ring_frame	frame;
	struct list_head	node;
	void			*page;		/* control frames only */
	dma_addr_t		page_phy;
	struct tbaccel_dev	*dev;
	bool			is_data;
	struct tbaccel_xfer	*xfer;		/* tx data only */
	struct tbaccel_slot	*slot;		/* rx data only */
	u64			off;		/* rx data region offset */
};

struct tbaccel_dev {
	struct tb_service	*svc;
	struct tb_xdomain	*xd;
	struct miscdevice	misc;
	char			name[32];
	int			index;
	struct kref		kref;
	struct mutex		lock;		/* tables */
	struct mutex		submit_lock;	/* one outbound transfer */
	spinlock_t		ctrl_lock;	/* blobs/acks/free pool/credit */
	wait_queue_head_t	ctrl_wait;
	wait_queue_head_t	ack_wait;
	wait_queue_head_t	credit_wait;
	struct list_head	buffers, regions, slots, blobs, acks, rx_free;
	u64			next_id;
	u64			credit;		/* sender: bytes it may emit */
	u64			pending_credit;	/* receiver: un-reported re-armed */
	int			users;
	bool			removed;
	struct device		*dma_dev;
	struct tb_ring		*tx, *rx;
	int			in_hopid, out_hopid;
	struct tbaccel_frame	*rx_frames;
	bool			rx_data_mode;
	struct tbaccel_slot	*rx_slot;
	struct list_head	rx_pending;	/* ARMs that arrived mid-stream */
	/* inbound transfer latency (ARM -> completion), no userspace involved */
	u64			lat_count;
	u64			lat_last_ns;
	u64			lat_min_ns;
	u64			lat_max_ns;
	struct dentry		*dbg;
};

static DEFINE_IDA(tbaccel_indices);
static struct tb_property_dir *tbaccel_dir;

/* ------------------------------------------------------------------ helpers */

static struct tbaccel_slot *tbaccel_find_slot(struct tbaccel_dev *dev, u64 id)
{
	struct tbaccel_slot *slot;

	list_for_each_entry(slot, &dev->slots, list)
		if (slot->id == id)
			return slot;
	return NULL;
}

static dma_addr_t tbaccel_sg_addr(struct sg_table *sgt, u64 off, u32 *avail)
{
	struct scatterlist *sg;
	size_t skip = off;
	int i;

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		size_t slen = sg_dma_len(sg);

		if (skip >= slen) {
			skip -= slen;
			continue;
		}
		*avail = slen - skip;
		return sg_dma_address(sg) + skip;
	}
	return 0;
}

static void tbaccel_tx_callback(struct tb_ring *ring, struct ring_frame *frame, bool canceled);
static void tbaccel_rx_callback(struct tb_ring *ring, struct ring_frame *frame, bool canceled);

/* ------------------------------------------------------------- ring posting */

/* one control buffer, page backed (kept exactly one deep while idle) */
static void tbaccel_rx_post_ctrl(struct tbaccel_dev *dev)
{
	struct tbaccel_frame *fr;

	spin_lock(&dev->ctrl_lock);
	fr = list_first_entry_or_null(&dev->rx_free, struct tbaccel_frame, node);
	if (fr)
		list_del(&fr->node);
	spin_unlock(&dev->ctrl_lock);
	if (!fr)
		return;

	fr->is_data = false;
	fr->slot = NULL;
	fr->frame.buffer_phy = fr->page_phy;
	fr->frame.size = 0;
	fr->frame.sof = TBACCEL_PDF_SOF;
	fr->frame.eof = TBACCEL_PDF_EOF;
	fr->frame.callback = tbaccel_rx_callback;
	tb_ring_rx(dev->rx, &fr->frame);
}

/* one data buffer, zero-copy into the slot region */
static u32 tbaccel_arm_one(struct tbaccel_dev *dev, struct tbaccel_slot *slot, u64 off)
{
	struct tbaccel_frame *fr;
	dma_addr_t addr;
	u32 chunk, avail;

	if (off >= slot->total)
		return 0;
	chunk = min_t(u64, rx_chunk, slot->total - off);
	addr = tbaccel_sg_addr(slot->region->buf->sgt, slot->region->offset + off, &avail);
	if (!addr || avail < chunk)
		return 0;

	spin_lock(&dev->ctrl_lock);
	fr = list_first_entry_or_null(&dev->rx_free, struct tbaccel_frame, node);
	if (fr)
		list_del(&fr->node);
	spin_unlock(&dev->ctrl_lock);
	if (!fr)
		return 0;

	fr->is_data = true;
	fr->slot = slot;
	fr->off = off;
	fr->frame.buffer_phy = addr;
	fr->frame.size = 0;
	fr->frame.sof = TBACCEL_PDF_SOF;
	fr->frame.eof = TBACCEL_PDF_EOF;
	fr->frame.callback = tbaccel_rx_callback;
	tb_ring_rx(dev->rx, &fr->frame);
	return chunk;
}

/* ------------------------------------------------------------------ tx path */

static int tbaccel_send_ctl(struct tbaccel_dev *dev, u16 type, u64 slot,
			    u32 len, const void *payload, u32 payload_len)
{
	struct tbaccel_frame *tf = kzalloc(sizeof(*tf), GFP_KERNEL);
	struct tbaccel_wire *w;

	if (!tf)
		return -ENOMEM;
	tf->page = (void *)__get_free_page(GFP_KERNEL);
	if (!tf->page) {
		kfree(tf);
		return -ENOMEM;
	}
	tf->page_phy = dma_map_single(dev->dma_dev, tf->page, TB_MAX_FRAME_SIZE, DMA_TO_DEVICE);
	if (dma_mapping_error(dev->dma_dev, tf->page_phy)) {
		free_page((unsigned long)tf->page);
		kfree(tf);
		return -ENOMEM;
	}
	tf->dev = dev;
	tf->is_data = false;
	tf->frame.buffer_phy = tf->page_phy;
	tf->frame.callback = tbaccel_tx_callback;
	w = tf->page;
	w->magic = cpu_to_le32(TBACCEL_MAGIC);
	w->type = cpu_to_le16(type);
	w->pad = 0;
	w->slot = cpu_to_le64(slot);
	w->len = cpu_to_le32(len);
	w->pad2 = cpu_to_le32(payload_len);
	if (payload_len)
		memcpy((u8 *)w + sizeof(*w), payload, payload_len);
	tf->frame.size = (sizeof(*w) + payload_len) < TB_MAX_FRAME_SIZE
			 ? (sizeof(*w) + payload_len) : 0;
	tf->frame.sof = TBACCEL_PDF_SOF;
	tf->frame.eof = TBACCEL_PDF_EOF;
	return tb_ring_tx(dev->tx, &tf->frame);
}

static void tbaccel_tx_callback(struct tb_ring *ring, struct ring_frame *frame, bool canceled)
{
	struct tbaccel_frame *tf = container_of(frame, struct tbaccel_frame, frame);
	struct tbaccel_xfer *xfer = tf->xfer;

	if (tf->is_data) {
		if (tf->page) {
			dma_unmap_single(tf->dev->dma_dev, tf->page_phy,
					 TB_MAX_FRAME_SIZE, DMA_TO_DEVICE);
			free_page((unsigned long)tf->page);
		}
		kfree(tf);	/* no page, phy pointed into the source dma-buf */
		if (xfer && atomic_dec_and_test(&xfer->frames)) {
			if (!canceled && xfer->fence)
				dma_fence_signal(xfer->fence);
			if (xfer->fence)
				dma_fence_put(xfer->fence);
			kfree(xfer);
		}
		return;
	}
	dma_unmap_single(tf->dev->dma_dev, tf->page_phy, TB_MAX_FRAME_SIZE, DMA_TO_DEVICE);
	free_page((unsigned long)tf->page);
	kfree(tf);
}

/* ------------------------------------------------------------ credit batch */

/* receiver: report re-armed bytes to the sender, batched */
static void tbaccel_flush_credit(struct tbaccel_dev *dev, bool force)
{
	if (dev->pending_credit == 0)
		return;
	if (!force && dev->pending_credit < tbaccel_credit_batch())
		return;
	tbaccel_send_ctl(dev, TBACCEL_MSG_CREDIT,
			 dev->rx_slot ? dev->rx_slot->id : 0,
			 (u32)dev->pending_credit, NULL, 0);
	dev->pending_credit = 0;
}

/* -------------------------------------------------------------- rx dispatch */

static void tbaccel_rx_complete(struct tbaccel_dev *dev, struct tbaccel_slot *slot);

/*
 * Only one inbound stream can be in flight: the wire carries no per-frame slot
 * tag, so a frame belongs to whichever slot armed the descriptor it lands in.
 * A second ARM therefore waits in rx_pending and is started - and ACKed - when
 * the current stream completes.  That keeps a pipelining peer from having its
 * ARM clobber the active slot's state.
 */
static void tbaccel_start_stream(struct tbaccel_dev *dev, struct tbaccel_slot *slot)
{
	u32 armed = 0;
	u64 total;

	mutex_lock(&dev->lock);
	dev->rx_slot = slot;
	dev->rx_data_mode = true;
	slot->queued = false;
	slot->t0 = ktime_get_ns();
	while (slot->armed_off < slot->total &&
	       armed < (u64) TBACCEL_WINDOW * TB_MAX_FRAME_SIZE) {
		u32 c = tbaccel_arm_one(dev, slot, slot->armed_off);

		if (!c)
			break;
		slot->armed_off += c;
		armed += c;
	}
	total = slot->total;
	mutex_unlock(&dev->lock);

	tbaccel_send_ctl(dev, TBACCEL_MSG_ACK, slot->id, armed, &total, sizeof(total));
	if (total == 0)
		tbaccel_rx_complete(dev, slot);
}

static void tbaccel_rx_complete(struct tbaccel_dev *dev, struct tbaccel_slot *slot)
{
	if (slot->op) {
		struct tbaccel_blob *blob = kmalloc(sizeof(*blob) + slot->op_len, GFP_KERNEL);

		if (blob) {
			blob->slot = slot->id;
			blob->len = slot->op_len;
			memcpy(blob->data, slot->op, slot->op_len);
			spin_lock(&dev->ctrl_lock);
			list_add_tail(&blob->list, &dev->blobs);
			spin_unlock(&dev->ctrl_lock);
			wake_up_interruptible(&dev->ctrl_wait);
		}
	}
	if (slot->t0) {
		u64 dt = ktime_get_ns() - slot->t0;

		dev->lat_count++;
		dev->lat_last_ns = dt;
		if (!dev->lat_min_ns || dt < dev->lat_min_ns)
			dev->lat_min_ns = dt;
		if (dt > dev->lat_max_ns)
			dev->lat_max_ns = dt;
	}

	dma_fence_signal(slot->fence);
	dma_fence_put(slot->fence);
	mutex_lock(&dev->lock);
	list_del(&slot->list);
	mutex_unlock(&dev->lock);
	kfree(slot->op);
	kfree(slot);

	dev->rx_slot = NULL;
	dev->rx_data_mode = false;
	tbaccel_flush_credit(dev, true);

	/* Start the next deferred ARM, if the peer queued one. */
	{
		struct tbaccel_slot *next = NULL;

		mutex_lock(&dev->lock);
		if (!list_empty(&dev->rx_pending)) {
			next = list_first_entry(&dev->rx_pending,
						struct tbaccel_slot, list);
			list_del(&next->list);
			next->queued = false;
		}
		mutex_unlock(&dev->lock);

		if (next) {
			tbaccel_start_stream(dev, next);
			return;
		}
	}
	tbaccel_rx_post_ctrl(dev);
}

static void tbaccel_rx_control(struct tbaccel_dev *dev, struct tbaccel_wire *w, u32 len)
{
	const u8 *payload = (const u8 *)w + sizeof(*w);
	u64 slot_id = le64_to_cpu(w->slot);

	switch (le16_to_cpu(w->type)) {
	case TBACCEL_MSG_ARM: {
		u32 total_req = le32_to_cpu(w->len);
		u32 oplen = min_t(u32, le32_to_cpu(w->pad2), len);
		struct tbaccel_slot *slot;

		mutex_lock(&dev->lock);
		slot = tbaccel_find_slot(dev, slot_id);
		if (slot) {
			slot->total = min_t(u64, total_req, slot->region->size);
			slot->armed_off = 0;
			kfree(slot->op);
			slot->op = oplen ? kmemdup(payload, oplen, GFP_KERNEL) : NULL;
			slot->op_len = oplen;
			if (dev->rx_slot && dev->rx_slot != slot) {
				/* another stream is active: queue this ARM */
				slot->queued = true;
				list_add_tail(&slot->list, &dev->rx_pending);
				mutex_unlock(&dev->lock);
				break;
			}
		}
		mutex_unlock(&dev->lock);

		if (slot && !slot->queued)
			tbaccel_start_stream(dev, slot);
		break;
	}
	case TBACCEL_MSG_ACK: {
		struct tbaccel_ack *ack = kmalloc(sizeof(*ack), GFP_KERNEL);

		if (ack) {
			ack->slot = slot_id;
			ack->len = le32_to_cpu(w->len);
			ack->total = 0;
			if (len >= sizeof(u64))
				memcpy(&ack->total, payload, sizeof(u64));
			spin_lock(&dev->ctrl_lock);
			list_add_tail(&ack->list, &dev->acks);
			spin_unlock(&dev->ctrl_lock);
			wake_up_interruptible(&dev->ack_wait);
		}
		break;
	}
	case TBACCEL_MSG_CREDIT:
		spin_lock(&dev->ctrl_lock);
		dev->credit += le32_to_cpu(w->len);
		spin_unlock(&dev->ctrl_lock);
		wake_up_interruptible(&dev->credit_wait);
		break;
	case TBACCEL_MSG_BLOB: {
		u32 blen = min_t(u32, le32_to_cpu(w->pad2), len);
		struct tbaccel_blob *blob = kmalloc(sizeof(*blob) + blen, GFP_KERNEL);

		if (blob) {
			blob->slot = slot_id;
			blob->len = blen;
			memcpy(blob->data, payload, blen);
			spin_lock(&dev->ctrl_lock);
			list_add_tail(&blob->list, &dev->blobs);
			spin_unlock(&dev->ctrl_lock);
			wake_up_interruptible(&dev->ctrl_wait);
		}
		break;
	}
	default:
		break;
	}
}

static void tbaccel_rx_callback(struct tb_ring *ring, struct ring_frame *frame, bool canceled)
{
	struct tbaccel_frame *tf = container_of(frame, struct tbaccel_frame, frame);
	struct tbaccel_dev *dev = tf->dev;
	u32 chunk = frame->size ? frame->size : TB_MAX_FRAME_SIZE;

	if (canceled)
		return;

	pr_info_ratelimited("tbaccel: rx %s pkt len=%u\n",
			    tf->is_data ? "data" : "ctrl", chunk);

	if (!tf->is_data) {
		/* recycle first so an ARM can immediately reuse this buffer */
		spin_lock(&dev->ctrl_lock);
		list_add_tail(&tf->node, &dev->rx_free);
		spin_unlock(&dev->ctrl_lock);

		tbaccel_rx_control(dev, tf->page, chunk);

		if (!dev->rx_data_mode)
			tbaccel_rx_post_ctrl(dev);
		return;
	}

	{
		struct tbaccel_slot *slot = tf->slot;
		u64 off = tf->off;

		spin_lock(&dev->ctrl_lock);
		list_add_tail(&tf->node, &dev->rx_free);
		spin_unlock(&dev->ctrl_lock);

		if (slot && off + chunk < slot->total) {
			u32 c = tbaccel_arm_one(dev, slot, slot->armed_off);

			if (c) {
				slot->armed_off += c;
				dev->pending_credit += c;
				tbaccel_flush_credit(dev, false);
			}
		} else if (slot) {
			tbaccel_rx_complete(dev, slot);
		}
	}
}

static void tbaccel_rx_frames_free(struct tbaccel_dev *dev)
{
	int i;

	if (!dev->rx_frames)
		return;
	for (i = 0; i < ring_size; i++) {
		struct tbaccel_frame *tf = &dev->rx_frames[i];

		if (tf->page_phy)
			dma_unmap_single(dev->dma_dev, tf->page_phy, TB_MAX_FRAME_SIZE, DMA_FROM_DEVICE);
		if (tf->page)
			free_page((unsigned long)tf->page);
	}
	kfree(dev->rx_frames);
	dev->rx_frames = NULL;
}

static int tbaccel_rx_frames_alloc(struct tbaccel_dev *dev)
{
	int i;

	dev->rx_frames = kcalloc(ring_size, sizeof(*dev->rx_frames), GFP_KERNEL);
	if (!dev->rx_frames)
		return -ENOMEM;
	for (i = 0; i < ring_size; i++) {
		struct tbaccel_frame *tf = &dev->rx_frames[i];

		tf->page = (void *)__get_free_page(GFP_KERNEL);
		if (!tf->page)
			return -ENOMEM;
		tf->page_phy = dma_map_single(dev->dma_dev, tf->page, TB_MAX_FRAME_SIZE, DMA_FROM_DEVICE);
		if (dma_mapping_error(dev->dma_dev, tf->page_phy))
			return -ENOMEM;
		tf->dev = dev;
		list_add_tail(&tf->node, &dev->rx_free);
	}
	return 0;
}

/* ------------------------------------------------------------ properties */

/*
 * The HopID pair is advertised in the "accel" service directory so the peer
 * can pick the mirror image of it (our in is its out and vice versa) instead
 * of both ends guessing. Same scheme as the kernel's stream.c.
 */
#define TBACCEL_HOPID_MIN	8

static void tbaccel_get_peer_hopids(struct tbaccel_dev *dev,
				    int *in_hopid, int *out_hopid)
{
	struct tb_property_dir *dir;
	struct tb_property *p;

	*in_hopid = -1;
	*out_hopid = -1;

	mutex_lock(&dev->svc->lock);
	dir = dev->svc->remote_properties;
	if (!dir)
		goto out;

	/* Reverse: the peer's out HopID is our in HopID and vice versa. */
	p = tb_property_find(dir, "outhopid", TB_PROPERTY_TYPE_VALUE);
	if (p && p->value.immediate >= TBACCEL_HOPID_MIN)
		*in_hopid = p->value.immediate;
	p = tb_property_find(dir, "inhopid", TB_PROPERTY_TYPE_VALUE);
	if (p && p->value.immediate >= TBACCEL_HOPID_MIN)
		*out_hopid = p->value.immediate;
out:
	mutex_unlock(&dev->svc->lock);
}

static int tbaccel_update_properties(struct tbaccel_dev *dev)
{
	struct tb_property_dir *dir;
	struct tb_property *p;
	int ret = 0;

	mutex_lock(&dev->svc->lock);

	dir = dev->svc->local_properties;
	if (!dir) {
		dir = tb_property_copy_dir(tbaccel_dir);
		if (!dir) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		dev->svc->local_properties = dir;
	}

	p = tb_property_find(dir, "inhopid", TB_PROPERTY_TYPE_VALUE);
	if (p)
		p->value.immediate = dev->in_hopid;
	else
		ret = tb_property_add_immediate(dir, "inhopid", dev->in_hopid);
	if (ret)
		goto out_unlock;

	p = tb_property_find(dir, "outhopid", TB_PROPERTY_TYPE_VALUE);
	if (p)
		p->value.immediate = dev->out_hopid;
	else
		ret = tb_property_add_immediate(dir, "outhopid", dev->out_hopid);

out_unlock:
	mutex_unlock(&dev->svc->lock);

	if (!ret)
		tb_service_properties_changed(dev->svc);
	return ret;
}

static void tbaccel_remove_properties(struct tbaccel_dev *dev)
{
	struct tb_property_dir *dir;
	struct tb_property *p;

	mutex_lock(&dev->svc->lock);

	dir = dev->svc->local_properties;
	if (dir) {
		p = tb_property_find(dir, "inhopid", TB_PROPERTY_TYPE_VALUE);
		if (p)
			tb_property_remove(p);
		p = tb_property_find(dir, "outhopid", TB_PROPERTY_TYPE_VALUE);
		if (p)
			tb_property_remove(p);
		tb_property_free_dir(dir);
		dev->svc->local_properties = NULL;
	}

	mutex_unlock(&dev->svc->lock);
}

/* -------------------------------------------------------------- link setup */

static int tbaccel_start(struct tbaccel_dev *dev)
{
	struct tb_xdomain *xd = dev->xd;
	unsigned int flags = RING_FLAG_FRAME;
	int in_hopid, out_hopid, ret;

	if (e2e)
		flags |= RING_FLAG_E2E;
	dev_info(&xd->dev, "tbaccel: ring flags = 0x%x (e2e=%d, chunk=%u)\n",
		 flags, e2e, chunk_size);

	/*
	 * If the peer already published its HopIDs use the mirror image of
	 * them, otherwise let the core pick the next available ones.
	 */
	tbaccel_get_peer_hopids(dev, &in_hopid, &out_hopid);
	if (in_hopid < 0 && out_hopid < 0)
		dev_info(&xd->dev, "tbaccel: peer provided no HopIDs, using next available\n");
	else
		dev_info(&xd->dev, "tbaccel: peer provided HopIDs (our in=%d, out=%d)\n",
			 in_hopid, out_hopid);

	dev->in_hopid = tb_xdomain_alloc_in_hopid(xd, in_hopid);
	if (dev->in_hopid < 0)
		return dev->in_hopid;
	dev->out_hopid = tb_xdomain_alloc_out_hopid(xd, out_hopid);
	if (dev->out_hopid < 0) {
		ret = dev->out_hopid;
		goto err_in;
	}

	dev->tx = tb_ring_alloc_tx(xd->tb->nhi, -1, ring_size, flags);
	if (!dev->tx) {
		ret = -ENOMEM;
		goto err_out;
	}
	dev->dma_dev = tb_ring_dma_device(dev->tx);

	dev->rx = tb_ring_alloc_rx(xd->tb->nhi, -1, ring_size, flags, dev->tx->hop,
				   BIT(TBACCEL_PDF_SOF), BIT(TBACCEL_PDF_EOF), NULL, NULL);
	if (!dev->rx) {
		ret = -ENOMEM;
		goto err_tx;
	}

	tb_ring_start(dev->tx);
	tb_ring_start(dev->rx);

	ret = tbaccel_rx_frames_alloc(dev);
	if (ret)
		goto err_stop;

	/* arm the single idle control buffer */
	tbaccel_rx_post_ctrl(dev);

	/*
	 * Enable the DMA paths last: arming the idle control buffer above
	 * primes the Rx ring so nothing can arrive before we are ready to
	 * receive it. Matches the kernel's TCP/IP-over-Thunderbolt driver.
	 */
	ret = tb_xdomain_enable_paths(xd, dev->out_hopid, dev->tx->hop,
				      dev->in_hopid, dev->rx->hop);
	if (ret)
		goto err_stop;

	dev_info(&xd->dev, "tbaccel: enabled HopIDs in=%d out=%d (tx ring=%d, rx ring=%d)\n",
		 dev->in_hopid, dev->out_hopid, dev->tx->hop, dev->rx->hop);

	/* Let the peer know which HopIDs it should mirror. */
	ret = tbaccel_update_properties(dev);
	if (ret)
		goto err_paths;
	return 0;

err_paths:
	tbaccel_remove_properties(dev);
	tb_xdomain_disable_paths(xd, dev->out_hopid, dev->tx->hop,
				 dev->in_hopid, dev->rx->hop);
err_stop:
	tb_ring_stop(dev->rx);
	tb_ring_stop(dev->tx);
	tbaccel_rx_frames_free(dev);
	tb_ring_free(dev->rx);
err_tx:
	tb_ring_free(dev->tx);
err_out:
	tb_xdomain_release_out_hopid(xd, dev->out_hopid);
err_in:
	tb_xdomain_release_in_hopid(xd, dev->in_hopid);
	return ret;
}

static void tbaccel_stop(struct tbaccel_dev *dev)
{
	/*
	 * Tear the DMA paths down first, while the rings are still running and
	 * their buffers mapped. Stopping a ring first zeroes its descriptor
	 * base and leaves in-flight frames with nowhere to drain; the router's
	 * hop then never reports its 'pending' bit clear and deactivation
	 * fails. Matches the kernel's TCP/IP-over-Thunderbolt teardown.
	 */
	if (dev->xd && dev->tx && dev->rx)
		tb_xdomain_disable_paths(dev->xd, dev->out_hopid, dev->tx->hop,
					 dev->in_hopid, dev->rx->hop);
	if (dev->tx) {
		tb_ring_flush(dev->tx, 500);
		tb_ring_stop(dev->tx);
	}
	if (dev->rx) {
		tb_ring_flush(dev->rx, 500);
		tb_ring_stop(dev->rx);
	}
	tbaccel_rx_frames_free(dev);
	if (dev->rx) {
		tb_ring_free(dev->rx);
		dev->rx = NULL;
	}
	if (dev->tx) {
		tb_ring_free(dev->tx);
		dev->tx = NULL;
	}
	if (dev->xd) {
		tb_xdomain_release_in_hopid(dev->xd, dev->in_hopid);
		tb_xdomain_release_out_hopid(dev->xd, dev->out_hopid);
	}
}

/* ------------------------------------------------------------- data transfer */

static int tbaccel_wait_credit(struct tbaccel_dev *dev, u64 need)
{
	long t = wait_event_interruptible_timeout(dev->credit_wait,
			({ u64 c;
			   spin_lock(&dev->ctrl_lock);
			   c = dev->credit;
			   spin_unlock(&dev->ctrl_lock);
			   c >= need; }), msecs_to_jiffies(5000));

	return t > 0 ? 0 : (t == 0 ? -ETIMEDOUT : (int) t);
}

static int tbaccel_wait_ack(struct tbaccel_dev *dev, u64 slot, u32 *armed, u64 *total)
{
	struct tbaccel_ack *ack, *tmp;
	long timeout;

	timeout = wait_event_interruptible_timeout(dev->ack_wait,
			({ bool found = false;
			   spin_lock(&dev->ctrl_lock);
			   list_for_each_entry(ack, &dev->acks, list)
				   if (ack->slot == slot) { found = true; break; }
			   spin_unlock(&dev->ctrl_lock);
			   found; }), msecs_to_jiffies(2000));
	if (timeout <= 0)
		return timeout == 0 ? -ETIMEDOUT : (int) timeout;

	spin_lock(&dev->ctrl_lock);
	list_for_each_entry_safe(ack, tmp, &dev->acks, list) {
		if (ack->slot == slot) {
			*armed = ack->len;
			*total = ack->total;
			list_del(&ack->list);
			kfree(ack);
			break;
		}
	}
	spin_unlock(&dev->ctrl_lock);
	return 0;
}

static int tbaccel_stream(struct tbaccel_dev *dev, struct tbaccel_reg *region,
			  struct tbaccel_xfer *xfer, u64 total)
{
	u64 sent = 0;
	int ret;

	while (sent < total) {
		u32 chunk = min_t(u64, chunk_size, total - sent);
		struct tbaccel_frame *tf;
		dma_addr_t addr;
		u32 avail;

		ret = tbaccel_wait_credit(dev, sent + chunk);
		if (ret)
			return ret;
		addr = tbaccel_sg_addr(region->buf->sgt, region->offset + sent, &avail);
		if (!addr || avail < chunk)
			return -EINVAL;

		tf = kzalloc(sizeof(*tf), GFP_KERNEL);
		if (!tf)
			return -ENOMEM;
		tf->dev = dev;
		tf->is_data = true;
		tf->xfer = xfer;
		tf->frame.buffer_phy = addr;
		tf->frame.size = chunk;

		if (bounce && region->buf->vaddr) {
			tf->page = (void *)__get_free_page(GFP_KERNEL);
			if (!tf->page) {
				kfree(tf);
				return -ENOMEM;
			}
			memcpy(tf->page,
			       (u8 *)region->buf->vaddr + region->offset + sent, chunk);
			tf->page_phy = dma_map_single(dev->dma_dev, tf->page,
						      TB_MAX_FRAME_SIZE, DMA_TO_DEVICE);
			if (dma_mapping_error(dev->dma_dev, tf->page_phy)) {
				free_page((unsigned long)tf->page);
				kfree(tf);
				return -ENOMEM;
			}
			tf->frame.buffer_phy = tf->page_phy;
		}
		tf->frame.sof = TBACCEL_PDF_SOF;
		tf->frame.eof = TBACCEL_PDF_EOF;
		tf->frame.callback = tbaccel_tx_callback;
		ret = tb_ring_tx(dev->tx, &tf->frame);
		if (ret)
			return ret;
		sent += chunk;
	}
	return 0;
}

static int tbaccel_transfer(struct tbaccel_dev *dev, struct tbaccel_reg *region,
			    u64 peer_slot, const void *op, u32 op_len,
			    struct dma_fence **out_fence)
{
	struct tbaccel_xfer *xfer;
	u64 total = 0;
	u32 armed = 0;
	int ret;

	ret = tbaccel_send_ctl(dev, TBACCEL_MSG_ARM, peer_slot,
			       region ? (u32)region->size : 0, op, op_len);
	if (ret)
		return ret;
	ret = tbaccel_wait_ack(dev, peer_slot, &armed, &total);
	if (ret)
		return ret;

	if (region)
		total = min_t(u64, total, region->size);

	xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer)
		return -ENOMEM;
	xfer->fence = tbaccel_fence_create();
	if (!xfer->fence) {
		kfree(xfer);
		return -ENOMEM;
	}

	if (region && total && armed) {
		/*
		 * The xfer owns an extra reference so the fence cannot be freed
		 * (by the caller dropping its reference, or the sync_file being
		 * closed) before every queued TX frame has completed and
		 * signalled it. The TX callback drops this reference.
		 */
		dma_fence_get(xfer->fence);
		atomic_set(&xfer->frames, DIV_ROUND_UP(total, chunk_size));
		spin_lock(&dev->ctrl_lock);
		dev->credit = armed;
		spin_unlock(&dev->ctrl_lock);
		wake_up_interruptible(&dev->credit_wait);

		ret = tbaccel_stream(dev, region, xfer, total);
		if (ret) {
			/*
			 * Frames already queued keep xfer alive and will drop
			 * the extra reference. Leak on this rare error path
			 * rather than risk a use-after-free.
			 */
			return ret;
		}
	} else {
		dma_fence_signal(xfer->fence);
		/* no frames: nothing references xfer, caller owns the fence */
		kfree(xfer);
	}

	*out_fence = xfer->fence;
	return 0;
}

/* ------------------------------------------------------------------- ioctls */

static int tbaccel_ioctl_get_info(struct tbaccel_dev *dev, void __user *argp)
{
	struct tbaccel_info info = {
		.abi_version = TBACCEL_ABI_VERSION,
		.max_ctrl = TBACCEL_CTRL_PAYLOAD,
		.frame_size = TB_MAX_FRAME_SIZE,
		.features = TBACCEL_FEAT_FENCES | TBACCEL_FEAT_RECV_CTRL | TBACCEL_FEAT_ZEROCOPY,
	};

	if (copy_to_user(argp, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static int tbaccel_ioctl_import(struct tbaccel_dev *dev, void __user *argp)
{
	struct tbaccel_import req;
	struct tbaccel_buf *buf;
	int ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	buf->dmabuf = dma_buf_get(req.fd);
	if (IS_ERR(buf->dmabuf)) {
		ret = PTR_ERR(buf->dmabuf);
		kfree(buf);
		return ret;
	}
	buf->attach = dma_buf_attach(buf->dmabuf, dev->dma_dev);
	if (IS_ERR(buf->attach)) {
		ret = PTR_ERR(buf->attach);
		goto err_put;
	}
	buf->sgt = dma_buf_map_attachment(buf->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(buf->sgt)) {
		ret = PTR_ERR(buf->sgt);
		goto err_detach;
	}
	buf->size = buf->dmabuf->size;
	if (dma_buf_vmap(buf->dmabuf, &buf->vmap))
		iosys_map_clear(&buf->vmap);
	buf->vaddr = iosys_map_is_set(&buf->vmap) ? buf->vmap.vaddr : NULL;
	dev_info(&dev->xd->dev, "tbaccel: import size=%llu vmap=%s bounce=%d\n",
		 (unsigned long long)buf->size, buf->vaddr ? "ok" : "FAILED", bounce);

	mutex_lock(&dev->lock);
	buf->handle = dev->next_id++;
	list_add_tail(&buf->list, &dev->buffers);
	mutex_unlock(&dev->lock);

	req.handle = buf->handle;
	req.size = buf->size;
	if (copy_to_user(argp, &req, sizeof(req)))
		ret = -EFAULT;
	return ret;

err_detach:
	dma_buf_detach(buf->dmabuf, buf->attach);
err_put:
	dma_buf_put(buf->dmabuf);
	kfree(buf);
	return ret;
}

static struct tbaccel_buf *tbaccel_find_buf(struct tbaccel_dev *dev, u64 id)
{
	struct tbaccel_buf *b;

	list_for_each_entry(b, &dev->buffers, list)
		if (b->handle == id)
			return b;
	return NULL;
}

static int tbaccel_ioctl_region(struct tbaccel_dev *dev, void __user *argp)
{
	struct tbaccel_region req;
	struct tbaccel_buf *buf;
	struct tbaccel_reg *region;
	int ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	mutex_lock(&dev->lock);
	buf = tbaccel_find_buf(dev, req.handle);
	if (!buf) {
		mutex_unlock(&dev->lock);
		return -ENOENT;
	}
	if (req.offset > buf->size || req.size > buf->size - req.offset) {
		mutex_unlock(&dev->lock);
		return -EINVAL;
	}
	if (req.offset % TB_MAX_FRAME_SIZE) {
		mutex_unlock(&dev->lock);
		return -EINVAL;	/* chunks are cut on the offset; both sides must agree */
	}
	if (req.size > U32_MAX) {
		mutex_unlock(&dev->lock);
		return -E2BIG;
	}
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region) {
		mutex_unlock(&dev->lock);
		return -ENOMEM;
	}
	region->buf = buf;
	region->offset = req.offset;
	region->size = req.size;
	region->id = dev->next_id++;
	list_add_tail(&region->list, &dev->regions);
	mutex_unlock(&dev->lock);

	req.region = region->id;
	if (copy_to_user(argp, &req, sizeof(req)))
		ret = -EFAULT;
	return ret;
}

static int tbaccel_ioctl_free(struct tbaccel_dev *dev, void __user *argp)
{
	struct tbaccel_free req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	mutex_lock(&dev->lock);
	if (req.flags == TBACCEL_FREE_REGION) {
		struct tbaccel_reg *r, *tmp;

		list_for_each_entry_safe(r, tmp, &dev->regions, list)
			if (r->id == req.id) { list_del(&r->list); kfree(r); }
	} else if (req.flags == TBACCEL_FREE_BUFFER) {
		struct tbaccel_buf *b, *tmp;

		list_for_each_entry_safe(b, tmp, &dev->buffers, list)
			if (b->handle == req.id) {
				list_del(&b->list);
				if (iosys_map_is_set(&b->vmap))
					dma_buf_vunmap(b->dmabuf, &b->vmap);
				dma_buf_unmap_attachment(b->attach, b->sgt, DMA_BIDIRECTIONAL);
				dma_buf_detach(b->dmabuf, b->attach);
				dma_buf_put(b->dmabuf);
				kfree(b);
			}
	}
	mutex_unlock(&dev->lock);
	return 0;
}

static int tbaccel_ioctl_post_recv(struct tbaccel_dev *dev, void __user *argp)
{
	struct tbaccel_post_recv req;
	struct tbaccel_reg *region = NULL;
	struct tbaccel_slot *slot;
	int fd, ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	mutex_lock(&dev->lock);
	{
		struct tbaccel_reg *r;

		list_for_each_entry(r, &dev->regions, list)
			if (r->id == req.region) { region = r; break; }
	}
	if (!region) {
		mutex_unlock(&dev->lock);
		return -ENOENT;
	}
	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot) {
		mutex_unlock(&dev->lock);
		return -ENOMEM;
	}
	slot->fence = tbaccel_fence_create();
	if (!slot->fence) {
		kfree(slot);
		mutex_unlock(&dev->lock);
		return -ENOMEM;
	}
	slot->region = region;
	slot->id = dev->next_id++;
	list_add_tail(&slot->list, &dev->slots);
	mutex_unlock(&dev->lock);

	fd = tbaccel_fence_export(slot->fence);
	if (fd < 0) {
		mutex_lock(&dev->lock);
		list_del(&slot->list);
		mutex_unlock(&dev->lock);
		dma_fence_put(slot->fence);
		kfree(slot);
		return fd;
	}
	req.slot = slot->id;
	req.fence_fd = fd;
	if (copy_to_user(argp, &req, sizeof(req))) {
		close_fd(fd);
		ret = -EFAULT;
	}
	return ret;
}

static int tbaccel_ioctl_submit(struct tbaccel_dev *dev, void __user *argp)
{
	struct tbaccel_submit req;
	struct tbaccel_reg *region = NULL;
	struct dma_fence *in_fence = NULL, *out_fence = NULL;
	void *op = NULL;
	int fd, ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (req.ctrl_len > TBACCEL_CTRL_PAYLOAD)
		return -EINVAL;

	if (req.ctrl_len) {
		op = memdup_user((void __user *)(uintptr_t)req.ctrl, req.ctrl_len);
		if (IS_ERR(op))
			return PTR_ERR(op);
	}
	if (req.in_fence_fd >= 0) {
		in_fence = sync_file_get_fence(req.in_fence_fd);
		if (!in_fence) {
			ret = -EINVAL;
			goto out_op;
		}
		ret = dma_fence_wait(in_fence, true);
		dma_fence_put(in_fence);
		if (ret) {
			ret = ret > 0 ? -ERESTARTSYS : ret;
			goto out_op;
		}
	}

	mutex_lock(&dev->submit_lock);

	if (req.region != TBACCEL_NONE) {
		mutex_lock(&dev->lock);
		list_for_each_entry(region, &dev->regions, list)
			if (region->id == req.region)
				break;
		mutex_unlock(&dev->lock);
		if (!region || region->id != req.region) {
			ret = -ENOENT;
			goto out_unlock;
		}
	}

	ret = tbaccel_transfer(dev, region, req.slot, op, req.ctrl_len, &out_fence);
	if (ret)
		goto out_unlock;

	fd = tbaccel_fence_export(out_fence);
	dma_fence_put(out_fence);
	if (fd < 0) {
		ret = fd;
		goto out_unlock;
	}
	req.out_fence_fd = fd;
	if (copy_to_user(argp, &req, sizeof(req))) {
		close_fd(fd);
		ret = -EFAULT;
		goto out_unlock;
	}
	ret = 0;

out_unlock:
	mutex_unlock(&dev->submit_lock);
out_op:
	kfree(op);
	return ret;
}

static int tbaccel_ioctl_recv_ctrl(struct tbaccel_dev *dev, void __user *argp)
{
	struct tbaccel_recv_ctrl req;
	struct tbaccel_blob *blob;
	int ret = 0;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	for (;;) {
		spin_lock(&dev->ctrl_lock);
		blob = list_first_entry_or_null(&dev->blobs, struct tbaccel_blob, list);
		if (blob)
			list_del(&blob->list);
		spin_unlock(&dev->ctrl_lock);
		if (blob)
			break;
		if (dev->users <= 0)
			return -ENODEV;
		if (wait_event_interruptible(dev->ctrl_wait, !list_empty(&dev->blobs)))
			return -ERESTARTSYS;
	}

	if (blob->len > req.len) {
		kfree(blob);
		return -EMSGSIZE;
	}
	if (copy_to_user((void __user *)(uintptr_t)req.buf, blob->data, blob->len)) {
		ret = -EFAULT;
	} else {
		req.len = blob->len;
		req.slot = blob->slot;
		req.flags = 0;
		if (copy_to_user(argp, &req, sizeof(req)))
			ret = -EFAULT;
	}
	kfree(blob);
	return ret;
}

static int tbaccel_ioctl_send_ctrl(struct tbaccel_dev *dev, void __user *argp)
{
	struct tbaccel_send_ctrl req;
	void *blob;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (req.len > TBACCEL_CTRL_PAYLOAD)
		return -EINVAL;
	if (!req.len)
		return 0;

	blob = memdup_user((void __user *)(uintptr_t)req.buf, req.len);
	if (IS_ERR(blob))
		return PTR_ERR(blob);

	mutex_lock(&dev->submit_lock);
	ret = tbaccel_send_ctl(dev, TBACCEL_MSG_BLOB, req.slot, 0, blob, req.len);
	mutex_unlock(&dev->submit_lock);
	kfree(blob);
	return ret;
}

static long tbaccel_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct tbaccel_dev *dev = file->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case TBACCEL_IOCTL_GET_INFO:
		return tbaccel_ioctl_get_info(dev, argp);
	case TBACCEL_IOCTL_IMPORT:
		return tbaccel_ioctl_import(dev, argp);
	case TBACCEL_IOCTL_REGION:
		return tbaccel_ioctl_region(dev, argp);
	case TBACCEL_IOCTL_FREE:
		return tbaccel_ioctl_free(dev, argp);
	case TBACCEL_IOCTL_POST_RECV:
		return tbaccel_ioctl_post_recv(dev, argp);
	case TBACCEL_IOCTL_SUBMIT:
		return tbaccel_ioctl_submit(dev, argp);
	case TBACCEL_IOCTL_RECV_CTRL:
		return tbaccel_ioctl_recv_ctrl(dev, argp);
	case TBACCEL_IOCTL_SEND_CTRL:
		return tbaccel_ioctl_send_ctrl(dev, argp);
	default:
		return -ENOTTY;
	}
}

static int tbaccel_open(struct inode *inode, struct file *file)
{
	struct tbaccel_dev *dev = container_of(file->private_data, struct tbaccel_dev, misc);

	mutex_lock(&dev->lock);
	if (dev->removed) {
		mutex_unlock(&dev->lock);
		return -ENXIO;
	}
	dev->users++;
	mutex_unlock(&dev->lock);
	file->private_data = dev;
	return 0;
}

static int tbaccel_release(struct inode *inode, struct file *file)
{
	struct tbaccel_dev *dev = file->private_data;

	mutex_lock(&dev->lock);
	dev->users--;
	mutex_unlock(&dev->lock);
	wake_up_interruptible(&dev->ctrl_wait);
	return 0;
}

static const struct file_operations tbaccel_fops = {
	.owner = THIS_MODULE,
	.open = tbaccel_open,
	.release = tbaccel_release,
	.unlocked_ioctl = tbaccel_unlocked_ioctl,
	.llseek = noop_llseek,
};

/* ------------------------------------------------------------ service glue */

static void tbaccel_dev_release(struct kref *kref)
{
	struct tbaccel_dev *dev = container_of(kref, struct tbaccel_dev, kref);
	struct tbaccel_blob *blob, *btmp;
	struct tbaccel_ack *ack, *atmp;
	struct tbaccel_slot *slot, *stmp;

	list_for_each_entry_safe(blob, btmp, &dev->blobs, list) { list_del(&blob->list); kfree(blob); }
	list_for_each_entry_safe(ack, atmp, &dev->acks, list) { list_del(&ack->list); kfree(ack); }
	list_for_each_entry_safe(slot, stmp, &dev->slots, list) {
		list_del(&slot->list);
		if (slot->fence) dma_fence_put(slot->fence);
		kfree(slot->op);
		kfree(slot);
	}
	tbaccel_stop(dev);
	ida_free(&tbaccel_indices, dev->index);
	kfree(dev);
}

static void tbaccel_dev_put(struct tbaccel_dev *dev)
{
	kref_put(&dev->kref, tbaccel_dev_release);
}

/* --------------------------------------------------------- debugfs counters */

static struct dentry *tbaccel_dbg_root;

static int tbaccel_stats_show(struct seq_file *m, void *v)
{
	struct tbaccel_dev *dev = m->private;

	seq_printf(m, "transfers %llu\n", dev->lat_count);
	seq_printf(m, "last_us %llu\n", dev->lat_last_ns / 1000);
	seq_printf(m, "min_us %llu\n", dev->lat_min_ns / 1000);
	seq_printf(m, "max_us %llu\n", dev->lat_max_ns / 1000);
	seq_printf(m, "tx_chunk %u\n", chunk_size);
	seq_printf(m, "rx_chunk %u\n", rx_chunk);
	return 0;
}

static int tbaccel_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, tbaccel_stats_show, inode->i_private);
}

static const struct file_operations tbaccel_stats_fops = {
	.owner		= THIS_MODULE,
	.open		= tbaccel_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int tbaccel_probe(struct tb_service *svc, const struct tb_service_id *id)
{
	struct tbaccel_dev *dev;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->svc = svc;
	dev->xd = tb_service_parent(svc);
	dev->next_id = 1;
	mutex_init(&dev->lock);
	mutex_init(&dev->submit_lock);
	spin_lock_init(&dev->ctrl_lock);
	init_waitqueue_head(&dev->ctrl_wait);
	init_waitqueue_head(&dev->ack_wait);
	init_waitqueue_head(&dev->credit_wait);
	INIT_LIST_HEAD(&dev->rx_pending);
	INIT_LIST_HEAD(&dev->buffers);
	INIT_LIST_HEAD(&dev->regions);
	INIT_LIST_HEAD(&dev->slots);
	INIT_LIST_HEAD(&dev->blobs);
	INIT_LIST_HEAD(&dev->acks);
	INIT_LIST_HEAD(&dev->rx_free);
	kref_init(&dev->kref);

	ret = tbaccel_start(dev);
	if (ret) {
		kfree(dev);
		return ret;
	}
	dev->index = ida_alloc(&tbaccel_indices, GFP_KERNEL);
	if (dev->index < 0) {
		ret = dev->index;
		tbaccel_stop(dev);
		kfree(dev);
		return ret;
	}
	snprintf(dev->name, sizeof(dev->name), "tbaccel%d", dev->index);
	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = dev->name;
	dev->misc.fops = &tbaccel_fops;

	ret = misc_register(&dev->misc);
	if (ret) {
		ida_free(&tbaccel_indices, dev->index);
		tbaccel_stop(dev);
		kfree(dev);
		return ret;
	}
	tb_service_set_drvdata(svc, dev);
	dev->dbg = debugfs_create_dir(dev->name, tbaccel_dbg_root);
	if (!IS_ERR_OR_NULL(dev->dbg))
		debugfs_create_file("stats", 0444, dev->dbg, dev,
				    &tbaccel_stats_fops);
	dev_info(&svc->dev, "tbaccel%d registered (single ring)\n", dev->index);
	return 0;
}

static void tbaccel_remove(struct tb_service *svc)
{
	struct tbaccel_dev *dev = tb_service_get_drvdata(svc);

	if (!dev)
		return;
	mutex_lock(&dev->lock);
	dev->removed = true;
	mutex_unlock(&dev->lock);
	wake_up_interruptible(&dev->ctrl_wait);
	tbaccel_remove_properties(dev);
	tb_service_properties_changed(dev->svc);
	misc_deregister(&dev->misc);
	debugfs_remove_recursive(dev->dbg);
	dev->dbg = NULL;
	tbaccel_dev_put(dev);
}

static const struct tb_service_id tbaccel_ids[] = {
	{ TB_SERVICE("accel", 1) },
	{ }
};
MODULE_DEVICE_TABLE(tbsvc, tbaccel_ids);

static struct tb_service_driver tbaccel_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "thunderbolt_accel",
	},
	.probe = tbaccel_probe,
	.remove = tbaccel_remove,
	.id_table = tbaccel_ids,
};

static const uuid_t tbaccel_dir_uuid =
	UUID_INIT(0x6b6a6f21, 0x9c1e, 0x4f2a,
		  0x8e, 0x3d, 0x2b, 0x77, 0x41, 0x0d, 0x9a, 0x55);

static int __init tbaccel_init(void)
{
	int ret;

	if (ring_size < 8)
		ring_size = 8;

	tbaccel_fence_context = dma_fence_context_alloc(1);
	tbaccel_dir = tb_property_create_dir(&tbaccel_dir_uuid);
	if (!tbaccel_dir)
		return -ENOMEM;
	tb_property_add_immediate(tbaccel_dir, "prtcid", 1);
	tb_property_add_immediate(tbaccel_dir, "prtcvers", 1);
	tb_property_add_immediate(tbaccel_dir, "prtcrevs", 0);
	tb_property_add_immediate(tbaccel_dir, "prtcstns", 0);
	ret = tb_register_property_dir("accel", tbaccel_dir);
	if (ret)
		goto err_dir;
	tbaccel_dbg_root = debugfs_create_dir("tbaccel", NULL);

	ret = tb_register_service_driver(&tbaccel_driver);
	if (ret)
		goto err_prop;
	return 0;
err_prop:
	tb_unregister_property_dir("accel", tbaccel_dir);
err_dir:
	tb_property_free_dir(tbaccel_dir);
	return ret;
}
module_init(tbaccel_init);

static void __exit tbaccel_exit(void)
{
	tb_unregister_service_driver(&tbaccel_driver);
	tb_unregister_property_dir("accel", tbaccel_dir);
	tb_property_free_dir(tbaccel_dir);
	ida_destroy(&tbaccel_indices);
	debugfs_remove_recursive(tbaccel_dbg_root);
}
module_exit(tbaccel_exit);

MODULE_AUTHOR("first cut");
MODULE_DESCRIPTION("Thunderbolt/USB4 accelerator offload (tbaccel)");
MODULE_LICENSE("GPL");
