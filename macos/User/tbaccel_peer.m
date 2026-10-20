/*
 * tbaccel_peer.m - user-space compute peer for the macOS tbaccel driver,
 *                  with the matmul running on Metal.
 *
 * The macOS analogue of linux/tbaccel_peer.c.  Buffers come from this process
 * (IOSurface-backed so Metal and the driver see the same pages), and the
 * matmul is a Metal compute kernel rather than a CPU loop.
 *
 * Buffer model, following ggml-ane:
 *   mmap a page-aligned region
 *   IOSurfaceCreate with kIOSurfaceAddress pointing at it   <- wraps our memory
 *   newBufferWithBytesNoCopy over IOSurfaceGetBaseAddress   <- Metal's view
 *   kTbAccelMethodImportSurface with the same address+size  <- the driver's view
 * so the peer's data, Metal's compute and the DMA all touch the same pages.
 *
 * TBX1 messages travel as control payloads: the kext delivers the operation
 * that rode a transfer's ARM as a blob (slot id + message).
 *
 * Build:
 *   clang -fobjc-arc -I ../Shared tbaccel_peer.m -framework IOKit \
 *         -framework Metal -framework Foundation -framework IOSurface -o tbaccel_peer
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <time.h>

#include <IOKit/IOKitLib.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>

#include "TbAccelUAPI.h"

#define TBX_MAGIC 0x31584254u /* "TBX1" */

enum tbx_type {
    TBX_PREPARE  = 1,
    TBX_PREPARED = 2,
    TBX_LOAD     = 3,
    TBX_MATMUL   = 4,
    TBX_RESULT   = 5,
};

#define TBX_DTYPE_F32 0

struct tbx_msg {
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

/*
 * Control blobs are 48 bytes in practice.  Keeping the receive buffer small
 * matters: the kernel reports structureOutputSize as 0 when input + output
 * exceed a page (24 + 4096 did).
 */
#define RX_BLOB_MAX 1024
#define MAX_BUFS    32

struct pbuf {
    uint64_t id;        /* peer-side buffer id (PREPARE) */
    uint64_t handle;    /* driver buffer handle */
    uint64_t region;
    uint64_t slot;
    uint8_t *ptr;
    size_t   size;
    IOSurfaceRef surf;
    id<MTLBuffer> mtl;
    uint64_t fence;      /* last TX fence that wrote into this buffer */
};

static struct pbuf bufs[MAX_BUFS];
static int nbufs;
static struct pbuf ybuf;  /* result buffer, grown on demand */

static id<MTLDevice>              mtl_device;
static id<MTLCommandQueue>        mtl_queue;
static id<MTLComputePipelineState> mtl_matmul;
/*
 * The compute completion is an MTLSharedEvent the GPU writes itself: waiting on
 * it by spinning costs ~70-110 us for a tiny kernel, where waitUntilCompleted
 * costs ~200 us because it waits for the completion interrupt to be delivered.
 * This is also the event shape the driver-side fence will take later.
 */
static id<MTLComputePipelineState> mtl_signal;
/*
 * The compute fence page.  The GPU stores the operation's sequence number here
 * (atomic store with release semantics) after the matmul; the kext maps the
 * same page uncached and waits for it inside SUBMIT.  This is the part of
 * MTLSharedEvent we can actually hand to a kext: a value in memory.
 */
static struct pbuf                 evpage;
static uint32_t                    gpu_seq;

static int verbose = 1;
static int bench_verbose = 0;

#define LOG(lvl, ...) do { if (verbose >= (lvl)) { printf(__VA_ARGS__); fflush(stdout); } } while (0)

/* -------------------------------------------------------------- transport */

static io_connect_t open_client(void)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching("TbAccelClient"));

    if (!svc) {
        fprintf(stderr, "no TbAccelClient service (kext loaded? peer connected?)\n");
        return 0;
    }

    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);

    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen: 0x%x\n", kr);
        return 0;
    }
    return conn;
}

/* ------------------------------------------------------------------ metal */

static int metal_init(void)
{
    static const char *src =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "kernel void matmul(device const float *x [[buffer(0)]],\n"
        "                   device const float *w [[buffer(1)]],\n"
        "                   device float *y       [[buffer(2)]],\n"
        "                   constant uint &M      [[buffer(3)]],\n"
        "                   constant uint &N      [[buffer(4)]],\n"
        "                   constant uint &K      [[buffer(5)]],\n"
        "                   uint2 gid [[thread_position_in_grid]])\n"
        "{\n"
        "    uint m = gid.y, n = gid.x;\n"
        "    if (m >= M || n >= N) return;\n"
        "    float acc = 0.0f;\n"
        "    for (uint k = 0; k < K; k++)\n"
        "        acc += x[m * K + k] * w[n * K + k];\n"
        "    y[m * N + n] = acc;\n"
        "}\n"
        "kernel void signal_event(device atomic<uint, thread_scope_device> *ev [[buffer(0)]],\n"
        "                         constant uint &value [[buffer(1)]])\n"
        "{\n"
        "    atomic_store_explicit(ev, value, memory_order_release, mem_flags::mem_device);\n"
        "}\n";

    mtl_device = MTLCreateSystemDefaultDevice();
    if (!mtl_device) {
        fprintf(stderr, "metal: no device\n");
        return -1;
    }
    mtl_queue = [mtl_device newCommandQueue];
    if (!mtl_queue) {
        fprintf(stderr, "metal: no command queue\n");
        return -1;
    }

    NSError *err = nil;
    id<MTLLibrary> lib = [mtl_device newLibraryWithSource:
                          [NSString stringWithUTF8String:src] options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "metal: compile failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "?");
        return -1;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:@"matmul"];
    if (!fn) {
        fprintf(stderr, "metal: no matmul function\n");
        return -1;
    }
    mtl_matmul = [mtl_device newComputePipelineStateWithFunction:fn error:&err];
    if (!mtl_matmul) {
        fprintf(stderr, "metal: pipeline failed: %s\n",
                err ? [[err localizedDescription] UTF8String] : "?");
        return -1;
    }
    {
        id<MTLFunction> sf = [lib newFunctionWithName:@"signal_event"];
        if (!sf) {
            fprintf(stderr, "metal: no signal_event function\n");
            return -1;
        }
        mtl_signal = [mtl_device newComputePipelineStateWithFunction:sf error:&err];
        if (!mtl_signal) {
            fprintf(stderr, "metal: signal pipeline failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "?");
            return -1;
        }
    }
    LOG(1, "metal: %s ready\n", [[mtl_device name] UTF8String]);
    return 0;
}

/* ---------------------------------------------------------------- buffers */

/* Wrap this process's own pages in an IOSurface so Metal and the driver share
 * them, exactly as ggml-ane does for the ANE path. */
static int alloc_buffer(size_t size, struct pbuf *b)
{
    size_t len = (size + PAGE_SIZE - 1) & ~((size_t) PAGE_SIZE - 1);
    NSDictionary *props;
    void *p;

    p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    props = @{
        (id) kIOSurfaceWidth:           @(len),
        (id) kIOSurfaceHeight:          @1,
        (id) kIOSurfaceBytesPerElement: @1,
        (id) kIOSurfaceBytesPerRow:     @(len),
        (id) kIOSurfaceAllocSize:       @(len),
        (id) kIOSurfacePixelFormat:     @0,
        @"IOSurfaceAddress":            @((uintptr_t) p),
    };
    b->surf = IOSurfaceCreate((CFDictionaryRef) props);
    if (!b->surf) {
        fprintf(stderr, "IOSurfaceCreate(%zu) failed\n", len);
        munmap(p, len);
        return -1;
    }
    if ((void *) IOSurfaceGetBaseAddress(b->surf) != p) {
        fprintf(stderr, "IOSurface did not adopt our memory\n");
        CFRelease(b->surf);
        munmap(p, len);
        return -1;
    }

    b->mtl = [mtl_device newBufferWithBytesNoCopy:p length:len
                                          options:MTLResourceStorageModeShared
                                      deallocator:nil];
    if (!b->mtl) {
        fprintf(stderr, "metal buffer over %zu bytes failed\n", len);
        CFRelease(b->surf);
        munmap(p, len);
        return -1;
    }
    b->ptr = p;
    b->size = len;
    return 0;
}

static void free_buffer(struct pbuf *b)
{
    /* MTLBuffer and IOSurface both reference memory we own; drop them first. */
    if (b->mtl) { [b->mtl release]; b->mtl = nil; }
    if (b->surf) { CFRelease(b->surf); b->surf = NULL; }
    if (b->ptr) { munmap(b->ptr, b->size); b->ptr = NULL; }
}

static int make_region(io_connect_t conn, size_t size, struct pbuf *b)
{
    struct tbaccel_import_surface is;

    if (alloc_buffer(size, b))
        return -1;

    memset(&is, 0, sizeof(is));
    is.addr = (uint64_t) (uintptr_t) b->ptr;
    is.size = b->size;
    is.surface_id = (uint32_t) IOSurfaceGetID(b->surf);

    size_t outSize = sizeof(is);
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodImportSurface,
                                                 &is, sizeof(is), &is, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "ImportSurface: 0x%x\n", kr);
        return -1;
    }
    b->handle = is.handle;

    {
        tbaccel_region reg = { .handle = b->handle, .offset = 0, .size = b->size };
        outSize = sizeof(reg);
        kr = IOConnectCallStructMethod(conn, kTbAccelMethodRegion,
                                       &reg, sizeof(reg), &reg, &outSize);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "Region: 0x%x\n", kr);
            return -1;
        }
        b->region = reg.region;
    }
    return 0;
}

/* The GPU fence page: imported with flags=1 so the kext maps it uncached. */
static int make_event_page(io_connect_t conn)
{
    struct tbaccel_import_surface is;
    size_t outSize = sizeof(is);
    kern_return_t kr;

    if (alloc_buffer(4096, &evpage))
        return -1;
    memset(evpage.ptr, 0, evpage.size);

    memset(&is, 0, sizeof(is));
    is.addr = (uint64_t)(uintptr_t) evpage.ptr;
    is.size = evpage.size;
    is.surface_id = (uint32_t) IOSurfaceGetID(evpage.surf);
    is.flags = 1;

    kr = IOConnectCallStructMethod(conn, kTbAccelMethodImportSurface,
                                   &is, sizeof(is), &is, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "event page ImportSurface: 0x%x\n", kr);
        return -1;
    }
    evpage.handle = is.handle;
    LOG(1, "event page: handle %llu\n", (unsigned long long) evpage.handle);
    return 0;
}

static int post_recv(io_connect_t conn, struct pbuf *b, uint64_t *slot)
{
    tbaccel_post_recv pr = { .region = b->region };
    size_t outSize = sizeof(pr);
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodPostRecv,
                                                 &pr, sizeof(pr), &pr, &outSize);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "PostRecv: 0x%x\n", kr);
        return -1;
    }
    *slot = pr.slot;
    return 0;
}

/* Encode the matmul and signal 'ev' at 'value'; the caller waits on the event.
 * Unlike waitUntilCompleted this needs no completion-interrupt round trip: the
 * GPU writes the event value and a spin sees it directly. */
static int metal_matmul_signal(const struct pbuf *w, const struct pbuf *x,
                               struct pbuf *y, uint32_t M, uint32_t N, uint32_t K,
                               uint32_t value)
{
    id<MTLCommandBuffer> cb = [mtl_queue commandBuffer];
    id<MTLComputeCommandEncoder> enc;
    uint32_t dims[3] = { M, N, K };

    if (!cb)
        return -1;
    enc = [cb computeCommandEncoder];
    if (!enc) {
        [cb commit];
        return -1;
    }
    [enc setComputePipelineState:mtl_matmul];
    [enc setBuffer:x->mtl offset:0 atIndex:0];
    [enc setBuffer:w->mtl offset:0 atIndex:1];
    [enc setBuffer:y->mtl offset:0 atIndex:2];
    [enc setBytes:&dims[0] length:sizeof(uint32_t) atIndex:3];
    [enc setBytes:&dims[1] length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:&dims[2] length:sizeof(uint32_t) atIndex:5];
    {
        MTLSize grid = MTLSizeMake(N, M, 1);
        MTLSize tg = MTLSizeMake(8, 8, 1);

        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
    }
    [enc setComputePipelineState:mtl_signal];
    [enc setBuffer:evpage.mtl offset:0 atIndex:0];
    [enc setBytes:&value length:sizeof(uint32_t) atIndex:1];
    {
        MTLSize one = MTLSizeMake(1, 1, 1);

        [enc dispatchThreads:one threadsPerThreadgroup:one];
    }
    [enc endEncoding];
    [cb commit];
    return 0;
}

/* ------------------------------------------------------------- ctrl plane */

static int send_ctrl(io_connect_t conn, const struct tbx_msg *m)
{
    struct {
        tbaccel_send_ctrl sc;
        struct tbx_msg msg;
    } in;

    memset(&in, 0, sizeof(in));
    in.sc.len = sizeof(*m);
    memcpy(&in.msg, m, sizeof(*m));

    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodSendCtrl,
                                                 &in, sizeof(in), NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "SendCtrl: 0x%x\n", kr);
        return -1;
    }
    return 0;
}

static int recv_ctrl(io_connect_t conn, struct tbx_msg *m, uint64_t *slot)
{
    struct {
        tbaccel_recv_ctrl rc;
        char blob[RX_BLOB_MAX];
    } out;
    tbaccel_recv_ctrl in = { .len = RX_BLOB_MAX };
    size_t outSize = sizeof(out);

    memset(&out, 0, sizeof(out));
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodRecvCtrl,
                                                 &in, sizeof(in), &out, &outSize);
    if (kr != KERN_SUCCESS)
        return (int)kr;
    if (out.rc.len < sizeof(*m))
        return -1;
    memcpy(m, out.blob, sizeof(*m));
    *slot = out.rc.slot;
    return 0;
}

/*
 * The submit's fence is the number of transmit commands queued when it
 * returned; the driver reports each completion.  Waiting on it is what makes
 * a buffer reusable: until then the DMA engine may still be reading it.
 */
static int drain_wait(io_connect_t conn, uint64_t fence, uint32_t timeout_ms,
                      uint64_t *drained)
{
    uint64_t in[2] = { fence, timeout_ms };
    uint64_t out = 0;
    uint32_t outCnt = 1;
    kern_return_t kr = IOConnectCallMethod(conn, kTbAccelMethodDrain,
                                           in, 2, NULL, 0, &out, &outCnt,
                                           NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Drain(%llu): 0x%x\n", (unsigned long long) fence, kr);
        return (int) kr;
    }
    if (drained)
        *drained = out;
    return 0;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

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

/* ------------------------------------------------------------ operations */

static int peer_prepare(io_connect_t conn, uint64_t id, size_t size)
{
    struct pbuf *b;

    if (nbufs >= MAX_BUFS)
        return -1;
    b = &bufs[nbufs];
    memset(b, 0, sizeof(*b));
    if (make_region(conn, size, b))
        return -1;
    if (post_recv(conn, b, &b->slot))
        return -1;
    b->id = id;
    nbufs++;

    {
        struct tbx_msg r = { .magic = TBX_MAGIC, .type = TBX_PREPARED,
                             .id = id, .a = id, .b = b->slot };
        if (send_ctrl(conn, &r))
            return -1;
    }
    LOG(1, "prepared id=%llu size=%zu slot=%llu\n",
        (unsigned long long) id, size, (unsigned long long) b->slot);
    return 0;
}

/* Y = X * W^T on the GPU: X is MxK, W is NxK, Y is MxN, all f32. */
static int metal_matmul(const struct pbuf *w, const struct pbuf *x,
                        struct pbuf *y, uint32_t M, uint32_t N, uint32_t K)
{
    id<MTLCommandBuffer> cb;
    id<MTLComputeCommandEncoder> enc;
    uint32_t dims[3] = { M, N, K };

    uint64_t t0, t1, t2;
    t0 = now_us();
    cb = [mtl_queue commandBuffer];
    if (!cb)
        return -1;
    enc = [cb computeCommandEncoder];
    if (!enc) {
        [cb commit];
        return -1;
    }
    [enc setComputePipelineState:mtl_matmul];
    [enc setBuffer:x->mtl offset:0 atIndex:0];
    [enc setBuffer:w->mtl offset:0 atIndex:1];
    [enc setBuffer:y->mtl offset:0 atIndex:2];
    [enc setBytes:&dims[0] length:sizeof(uint32_t) atIndex:3];
    [enc setBytes:&dims[1] length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:&dims[2] length:sizeof(uint32_t) atIndex:5];

    {
        MTLSize grid = MTLSizeMake(N, M, 1);
        MTLSize tg = MTLSizeMake(8, 8, 1);

        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
    }
    [enc endEncoding];
    t1 = now_us();
    [cb commit];
    [cb waitUntilCompleted];
    t2 = now_us();
    if (bench_verbose) {
        printf("  metal: setup %llu us, commit %llu us, wait %llu us\n",
               (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
               0ull);
        fflush(stdout);
    }
    return cb.status == MTLCommandBufferStatusCompleted ? 0 : -1;
}

static void peer_matmul(io_connect_t conn, const struct tbx_msg *op, uint64_t in_slot,
                        uint64_t t_entry)
{
    struct pbuf *w = find_id(op->a);
    struct pbuf *x = find_slot(in_slot);
    size_t need = (size_t) op->m * op->n * sizeof(float);

    if (!w || !x) {
        LOG(0, "matmul: missing W(id=%llu %s) or X(slot=%llu %s)\n",
            (unsigned long long) op->a, w ? "found" : "MISSING",
            (unsigned long long) in_slot, x ? "found" : "MISSING");
        return;
    }
    uint64_t t_drain = 0, t_metal0 = 0, t_metal = 0, t_submit = 0;
    uint32_t gpu_value = 0;

    if (ybuf.ptr && ybuf.fence) {
        uint64_t t0 = now_us(), drained = 0;

        if (drain_wait(conn, ybuf.fence, 2000, &drained) == 0) {
            uint64_t dt = now_us() - t0;

            t_drain = dt;
            if (dt >= 50)
                LOG(1, "drain fence %llu done (%llu us)\n",
                    (unsigned long long) ybuf.fence, (unsigned long long) dt);
            else
                LOG(2, "drain fence %llu already done (%llu us)\n",
                    (unsigned long long) ybuf.fence, (unsigned long long) dt);
        }
        ybuf.fence = 0;
    }

    if (!ybuf.ptr || ybuf.size < need) {
        free_buffer(&ybuf);
        memset(&ybuf, 0, sizeof(ybuf));
        if (make_region(conn, need, &ybuf)) {
            LOG(0, "matmul: cannot allocate result buffer\n");
            return;
        }
    }

    t_metal0 = now_us();
    {
        uint32_t value = ++gpu_seq;

        if (metal_matmul_signal(w, x, &ybuf, op->m, op->n, op->k, value)) {
            LOG(0, "matmul: metal encode failed\n");
            return;
        }
        gpu_value = value;
    }

    {
        struct tbx_msg res = { .magic = TBX_MAGIC, .type = TBX_RESULT, .b = op->b };
        struct {
            tbaccel_submit sub;
            struct tbx_msg msg;
        } in;

        memset(&in, 0, sizeof(in));
        in.sub.slot = op->b;            /* the client's posted result slot */
        in.sub.region = ybuf.region;
        in.sub.in_fence_fd = -1;
        memcpy(&in.msg, &res, sizeof(res));

        uint64_t fence = 0;
        uint32_t fenceCnt = 1;
        uint64_t t_sub0;
        uint64_t wait[3];

        wait[0] = evpage.handle;
        wait[1] = gpu_value;
        wait[2] = 200;              /* ms bound on the GPU fence */

        t_metal = now_us() - t_metal0;
        t_sub0 = now_us();
        kern_return_t kr = IOConnectCallMethod(conn, kTbAccelMethodSubmit,
                                               wait, 3, &in, sizeof(in),
                                               &fence, &fenceCnt, NULL, NULL);
        if (kr != KERN_SUCCESS) {
            LOG(0, "matmul: result SUBMIT failed: 0x%x\n", kr);
        } else {
            ybuf.fence = fence;
            t_submit = now_us() - t_sub0;
            LOG(1, "matmul %ux%ux%u done (metal), RESULT -> slot %llu (fence %llu)\n",
                op->m, op->n, op->k, (unsigned long long) op->b,
                (unsigned long long) fence);
            {
                uint64_t total = now_us() - t_entry;
                uint64_t acc = t_drain + t_metal + t_submit;

                LOG(1, "  timing: total %llu us = drain %llu + metal %llu + submit %llu + other %llu\n",
                    (unsigned long long) total, (unsigned long long) t_drain,
                    (unsigned long long) t_metal, (unsigned long long) t_submit,
                    (unsigned long long) (total > acc ? total - acc : 0));
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *e = getenv("TBX_VERBOSE");
    io_connect_t conn;
    int info_only = argc > 1 && !strcmp(argv[1], "info");

    if (e)
        verbose = atoi(e);

    conn = open_client();
    if (!conn)
        return 1;

    {
        tbaccel_info info = {0};
        size_t outSize = sizeof(info);

        if (IOConnectCallStructMethod(conn, kTbAccelMethodGetInfo, NULL, 0,
                                      &info, &outSize) == KERN_SUCCESS)
            LOG(1, "peer: abi=%u max_ctrl=%u frame=%u features=0x%llx\n",
                info.abi_version, info.max_ctrl, info.frame_size,
                (unsigned long long) info.features);
        else
            LOG(0, "peer: GetInfo failed\n");
    }
    if (info_only) {
        IOServiceClose(conn);
        return 0;
    }

    if (metal_init()) {
        IOServiceClose(conn);
        return 1;
    }

    if (argc > 1 && (!strcmp(argv[1], "bench-event") || !strcmp(argv[1], "bench"))) {
        int use_event = !strcmp(argv[1], "bench-event");

        struct pbuf b[3];
        uint32_t dim = 64, i;
        uint64_t best = ~0ull, sum = 0;

        if (use_event) {
            memset(&evpage, 0, sizeof(evpage));
            if (alloc_buffer(4096, &evpage)) {
                fprintf(stderr, "bench: event page alloc failed\n");
                return 1;
            }
        }
        memset(b, 0, sizeof(b));
        for (i = 0; i < 3; i++) {
            if (alloc_buffer(16384, &b[i])) {
                fprintf(stderr, "bench: alloc failed\n");
                return 1;
            }
            for (uint32_t j = 0; j < 16384 / 4; j++)
                ((float *)(void *)b[i].ptr)[j] = 0.001f;
        }
        bench_verbose = verbose >= 2;
        for (i = 0; i < 6; i++) {   /* warm up */
            if (metal_matmul(&b[0], &b[1], &b[2], dim, dim, dim))
                return 1;
        }
        bench_verbose = 0;
        for (i = 0; i < 50; i++) {
            uint64_t t0 = now_us();
            uint64_t dt;
            if (use_event) {
                if (metal_matmul_signal(&b[0], &b[1], &b[2], dim, dim, dim,
                                        ++gpu_seq))
                    return 1;
                while (((volatile uint32_t *) evpage.ptr)[0] < gpu_seq) {
                    /* spin on the page the GPU writes - the kext does this */
                }
            } else if (metal_matmul(&b[0], &b[1], &b[2], dim, dim, dim)) {
                return 1;
            }
            dt = now_us() - t0;
            sum += dt;
            if (dt < best)
                best = dt;
        }
        printf("bench: metal matmul 64x64x64: min %llu us, mean %llu us\n",
               (unsigned long long) best, (unsigned long long) (sum / 50));
        return 0;
    }

    if (make_event_page(conn)) {
        LOG(0, "peer: cannot create the GPU fence page\n");
        IOServiceClose(conn);
        return 1;
    }

    LOG(1, "peer: waiting for operations\n");
    for (;;) {
        struct tbx_msg m;
        uint64_t slot = 0;
        int r = recv_ctrl(conn, &m, &slot);
        uint64_t t_recv = now_us();

        if (r) {
            LOG(0, "peer: RecvCtrl failed: 0x%x\n", r);
            break;
        }
        if (m.magic != TBX_MAGIC) {
            LOG(0, "peer: bad magic 0x%08x\n", m.magic);
            continue;
        }
        switch (m.type) {
        case TBX_PREPARE:
            LOG(1, "peer: PREPARE id=%llu size=%llu\n",
                (unsigned long long) m.id, (unsigned long long) m.a);
            peer_prepare(conn, m.id, (size_t) m.a);
            break;
        case TBX_LOAD:
            LOG(1, "peer: LOAD id=%llu (%s)\n", (unsigned long long) m.id,
                find_id(m.id) ? "ok" : "no buffer");
            break;
        case TBX_MATMUL:
            LOG(1, "peer: MATMUL w=%llu y_slot=%llu %ux%ux%u\n",
                (unsigned long long) m.a, (unsigned long long) m.b,
                m.m, m.n, m.k);
            peer_matmul(conn, &m, slot, t_recv);
            break;
        default:
            LOG(2, "peer: ignoring type %u\n", m.type);
            break;
        }
    }

    IOServiceClose(conn);
    return 0;
}
