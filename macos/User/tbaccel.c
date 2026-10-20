/*
 * tbaccel.c - user-space exerciser for the macOS tbaccel driver.
 *
 * The macOS counterpart of linux/test_tbaccel.c. Where Linux opens
 * /dev/tbaccel0 and issues ioctls, this opens the TbAccelClient IOService and
 * issues external methods; the request structures are identical.
 *
 * Build:
 *   clang -I ../Shared tbaccel.c -framework IOKit -framework CoreFoundation -o tbaccel
 *
 * Usage:
 *   ./tbaccel info
 *   ./tbaccel recv <size>
 *   ./tbaccel send <peer-slot> <size>
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

/*
 * Control blobs are 48 bytes in practice.  Keeping the receive buffer small
 * matters: the kernel reports structureOutputSize as 0 when input + output
 * exceed a page (24 + 4096 did), which made recv_ctrl fail its size guard.
 */
#define RX_BLOB_MAX 1024

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

static int cmd_info(void)
{
    io_connect_t conn = open_client();
    if (!conn)
        return 1;

    tbaccel_info info = {0};
    size_t outSize = sizeof(info);
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodGetInfo,
                                                 NULL, 0, &info, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "GetInfo: 0x%x\n", kr);
        IOServiceClose(conn);
        return 1;
    }
    printf("abi_version=%u max_ctrl=%u frame_size=%u features=0x%llx\n",
           info.abi_version, info.max_ctrl, info.frame_size,
           (unsigned long long)info.features);
    IOServiceClose(conn);
    return 0;
}

/* import a kernel buffer and map it into our address space */
static void *import_and_map(io_connect_t conn, size_t size, uint64_t *handle)
{
    tbaccel_import imp = {0};
    imp.size = size;
    size_t outSize = sizeof(imp);
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodImport,
                                                 &imp, sizeof(imp), &imp, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Import: 0x%x\n", kr);
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

static int cmd_recv(size_t size)
{
    io_connect_t conn = open_client();
    if (!conn)
        return 1;

    uint64_t handle = 0;
    void *p = import_and_map(conn, size, &handle);
    if (!p)
        return 1;

    tbaccel_region reg = { .handle = handle, .offset = 0, .size = size };
    size_t outSize = sizeof(reg);
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodRegion,
                                                 &reg, sizeof(reg), &reg, &outSize);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "Region: 0x%x\n", kr); return 1; }

    tbaccel_post_recv pr = { .region = reg.region };
    outSize = sizeof(pr);
    kr = IOConnectCallStructMethod(conn, kTbAccelMethodPostRecv, &pr, sizeof(pr), &pr, &outSize);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "PostRecv: 0x%x\n", kr); return 1; }
    printf("SLOT %llu\n", (unsigned long long)pr.slot);
    fflush(stdout);

    /* wait for a control blob */
    char ctrl[RX_BLOB_MAX];
    struct { tbaccel_recv_ctrl rc; char blob[RX_BLOB_MAX]; } out;
    tbaccel_recv_ctrl in = { .len = sizeof(ctrl) };
    outSize = sizeof(out);
    kr = IOConnectCallStructMethod(conn, kTbAccelMethodRecvCtrl, &in, sizeof(in), &out, &outSize);
    if (kr != KERN_SUCCESS) {
        /* Keep the client (and therefore the armed slot) alive so the peer can
         * target it; the blob read path is exercised separately. */
        fprintf(stderr, "RecvCtrl: 0x%x - holding slot armed\n", kr);
        fflush(stdout);
        sleep(600);
        return 0;
    }
    printf("CTRL slot=%llu len=%u\n", (unsigned long long)out.rc.slot, out.rc.len);

    uint64_t scalarIn[2] = { pr.slot, 5000 };
    uint64_t outScalar = 1;
    uint32_t outCnt = 1;
    kr = IOConnectCallScalarMethod(conn, kTbAccelMethodWaitSlot, scalarIn, 2, &outScalar, &outCnt);
    printf(kr == KERN_SUCCESS ? "DATA arrived into region\n" : "slot wait failed\n");

    IOServiceClose(conn);
    return 0;
}

static int cmd_send(uint64_t peer_slot, size_t size)
{
    io_connect_t conn = open_client();
    if (!conn)
        return 1;

    uint64_t handle = 0;
    void *p = import_and_map(conn, size, &handle);
    if (!p)
        return 1;
    memset(p, 0x5a, size);

    tbaccel_region reg = { .handle = handle, .offset = 0, .size = size };
    size_t outSize = sizeof(reg);
    kern_return_t kr = IOConnectCallStructMethod(conn, kTbAccelMethodRegion,
                                                 &reg, sizeof(reg), &reg, &outSize);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "Region: 0x%x\n", kr); return 1; }

    const char *op = "op=matmul";
    struct {
        tbaccel_submit sub;
        char blob[64];
    } in;
    memset(&in, 0, sizeof(in));
    in.sub.slot = peer_slot;
    in.sub.region = reg.region;
    in.sub.in_fence_fd = -1;
    size_t blobLen = strlen(op) + 1;
    memcpy(in.blob, op, blobLen);

    kr = IOConnectCallStructMethod(conn, kTbAccelMethodSubmit, &in,
                                   sizeof(in.sub) + blobLen, NULL, NULL);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "Submit: 0x%x\n", kr); return 1; }
    printf("submitted %zu bytes + ctrl to slot %llu\n", size, (unsigned long long)peer_slot);

    IOServiceClose(conn);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s info\n", argv[0]);
        fprintf(stderr, "       %s recv <size>\n", argv[0]);
        fprintf(stderr, "       %s send <peer-slot> <size>\n", argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "info"))
        return cmd_info();
    if (!strcmp(argv[1], "recv"))
        return cmd_recv(argc > 2 ? strtoul(argv[2], NULL, 0) : 4096);
    if (!strcmp(argv[1], "send"))
        return cmd_send(argc > 2 ? strtoull(argv[2], NULL, 0) : 0,
                        argc > 3 ? strtoul(argv[3], NULL, 0) : 4096);
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 1;
}
