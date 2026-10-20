/*
 * tbaccel_peer.c - user-space compute peer for the macOS tbaccel driver.
 *
 * The macOS analogue of linux/tbaccel_peer.c: it answers TBX1 requests from a
 * remote client.  Buffers live in this process (imported into the driver and
 * mapped back), so no kernel-side compute is involved and the same protocol
 * speaks to a stock Linux tbaccel_client.
 *
 * TBX1 messages travel as control payloads.  With a user client attached the
 * kext delivers the operation that rode a transfer's ARM as a blob (slot id +
 * message), which is what this loop reads.
 *
 * Build:
 *   clang -I ../Shared tbaccel_peer.c -framework IOKit -framework CoreFoundation -o tbaccel_peer
 *
 * Run (needs the peer side of the link up and a Linux client issuing requests):
 *   ./tbaccel_peer            # INFO level
 *   TBX_VERBOSE=2 ./tbaccel_peer
 *
 * Deliberately does not use MTLSharedEvent yet: fences are the slot ids the
 * driver signals, exactly like the Linux node.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

#include <IOKit/IOKitLib.h>

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

#define MAX_BUFS 32

struct pbuf {
    uint64_t id;        /* peer-side buffer id (PREPARE) */
    uint64_t handle;    /* driver buffer handle */
    uint64_t region;
    uint64_t slot;
    void    *ptr;
    size_t   size;
};

static struct pbuf bufs[MAX_BUFS];
static int nbufs;
static struct pbuf ybuf;  /* result buffer, grown on demand */

static int verbose = 1;

#define LOG(lvl, ...) do { if (verbose >= (lvl)) { printf(__VA_ARGS__); fflush(stdout); } } while (0)

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

static void *import_and_map(io_connect_t conn, size_t size, uint64_t *handle)
{
    tbaccel_import imp = {0};

    imp.size = size;

    size_t outSize = sizeof(imp);
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodImport,
                                                 &imp, sizeof(imp), &imp, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Import(%zu): 0x%x\n", size, kr);
        return NULL;
    }
    *handle = imp.handle;

    mach_vm_address_t addr = 0;
    mach_vm_size_t len = 0;

    kr = IOConnectMapMemory64(conn, (uint32_t)imp.handle, mach_task_self(),
                              &addr, &len, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "MapMemory: 0x%x\n", kr);
        return NULL;
    }
    return (void *)addr;
}

static int make_region(io_connect_t conn, size_t size, struct pbuf *b)
{
    b->ptr = import_and_map(conn, size, &b->handle);
    if (!b->ptr)
        return -1;
    b->size = size;

    tbaccel_region reg = { .handle = b->handle, .offset = 0, .size = size };
    size_t outSize = sizeof(reg);
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodRegion,
                                                 &reg, sizeof(reg), &reg, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Region: 0x%x\n", kr);
        return -1;
    }
    b->region = reg.region;
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
        char blob[TBACCEL_MAX_CTRL];
    } out;
    tbaccel_recv_ctrl in = { .len = TBACCEL_MAX_CTRL };
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
        (unsigned long long)id, size, (unsigned long long)b->slot);
    return 0;
}

static void peer_matmul(io_connect_t conn, const struct tbx_msg *op, uint64_t in_slot)
{
    struct pbuf *w = find_id(op->a);
    struct pbuf *x = find_slot(in_slot);
    size_t need = (size_t)op->m * op->n * sizeof(float);

    if (!w || !x) {
        LOG(0, "matmul: missing W(id=%llu %s) or X(slot=%llu %s)\n",
            (unsigned long long)op->a, w ? "found" : "MISSING",
            (unsigned long long)in_slot, x ? "found" : "MISSING");
        return;
    }
    if (!ybuf.ptr || ybuf.size < need) {
        if (make_region(conn, need, &ybuf)) {
            LOG(0, "matmul: cannot allocate result buffer\n");
            return;
        }
    }

    {
        const float *wp = w->ptr, *xp = x->ptr;
        float *yp = ybuf.ptr;

        for (uint32_t m = 0; m < op->m; m++)
            for (uint32_t n = 0; n < op->n; n++) {
                float acc = 0.0f;

                for (uint32_t k = 0; k < op->k; k++)
                    acc += xp[m * op->k + k] * wp[n * op->k + k];
                yp[m * op->n + n] = acc;
            }
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

        kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodSubmit,
                                                     &in, sizeof(in), NULL, NULL);
        if (kr != KERN_SUCCESS)
            LOG(0, "matmul: result SUBMIT failed: 0x%x\n", kr);
        else
            LOG(1, "matmul %ux%ux%u done, RESULT -> slot %llu\n",
                op->m, op->n, op->k, (unsigned long long)op->b);
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
                (unsigned long long)info.features);
        else
            LOG(0, "peer: GetInfo failed\n");
    }
    if (info_only) {
        IOServiceClose(conn);
        return 0;
    }

    LOG(1, "peer: waiting for operations\n");
    for (;;) {
        struct tbx_msg m;
        uint64_t slot = 0;
        int r = recv_ctrl(conn, &m, &slot);

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
                (unsigned long long)m.id, (unsigned long long)m.a);
            peer_prepare(conn, m.id, (size_t)m.a);
            break;
        case TBX_LOAD:
            LOG(1, "peer: LOAD id=%llu (%s)\n", (unsigned long long)m.id,
                find_id(m.id) ? "ok" : "no buffer");
            break;
        case TBX_MATMUL:
            LOG(1, "peer: MATMUL w=%llu y_slot=%llu %ux%ux%u\n",
                (unsigned long long)m.a, (unsigned long long)m.b,
                m.m, m.n, m.k);
            peer_matmul(conn, &m, slot);
            break;
        default:
            LOG(2, "peer: ignoring type %u\n", m.type);
            break;
        }
    }

    IOServiceClose(conn);
    return 0;
}
