// SPDX-License-Identifier: GPL-2.0
/*
 * test_tbaccel - userspace exerciser for the tbaccel UAPI.
 *
 * Build (self-contained, uses the vendored thunderbolt_accel.h next to it):
 *   gcc -O2 -Wall test_tbaccel.c -o test_tbaccel
 *
 * Usage (two hosts, both running test_tbaccel against their local device):
 *   host A: ./test_tbaccel recv /dev/tbaccel0 4096
 *           -> prints "SLOT <id>" and blocks until a control blob arrives
 *   host B: ./test_tbaccel send /dev/tbaccel0 <peer-slot-id> 4096
 *           -> imports a dma-buf, posts a region, submits it to <peer-slot-id>
 *
 *   ./test_tbaccel info /dev/tbaccel0   # no peer needed, just GET_INFO
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "thunderbolt_accel.h"
#include <linux/dma-heap.h>

static int alloc_dmabuf(size_t size) {
    int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0)
        return -1;
    struct dma_heap_allocation_data data = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        close(heap);
        return -1;
    }
    close(heap);
    return (int) data.fd;
}

static int cmd_info(const char *dev) {
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    struct tbaccel_info info = {0};
    if (ioctl(fd, TBACCEL_IOCTL_GET_INFO, &info) < 0) {
        perror("GET_INFO");
        close(fd);
        return 1;
    }
    printf("abi_version=%u max_ctrl=%u frame_size=%u features=0x%llx\n",
           info.abi_version, info.max_ctrl, info.frame_size,
           (unsigned long long) info.features);
    close(fd);
    return 0;
}

static int wait_fence(int fence_fd) {
    struct pollfd p = { .fd = fence_fd, .events = POLLIN };
    int r;
    do {
        r = poll(&p, 1, 5000);
    } while (r < 0 && errno == EINTR);
    if (r == 0)
        return -1; /* timeout */
    return r < 0 ? -1 : 0;
}

/* post a receive slot and wait for the peer's control blob */
static int cmd_recv(const char *dev, size_t size) {
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }

    int buf_fd = alloc_dmabuf(size);
    if (buf_fd < 0) {
        fprintf(stderr, "no dma-buf (need /dev/dma_heap/system): %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    struct tbaccel_import imp = { .fd = (uint32_t) buf_fd };
    if (ioctl(fd, TBACCEL_IOCTL_IMPORT, &imp) < 0) { perror("IMPORT"); return 1; }

    struct tbaccel_region reg = { .handle = imp.handle, .offset = 0, .size = size };
    if (ioctl(fd, TBACCEL_IOCTL_REGION, &reg) < 0) { perror("REGION"); return 1; }

    struct tbaccel_post_recv pr = { .region = reg.region };
    if (ioctl(fd, TBACCEL_IOCTL_POST_RECV, &pr) < 0) { perror("POST_RECV"); return 1; }
    printf("SLOT %llu\n", (unsigned long long) pr.slot);
    fflush(stdout);

    /* wait for the peer's control blob */
    char ctrl[TBACCEL_MAX_CTRL];
    struct tbaccel_recv_ctrl rc = {
        .buf = (uint64_t) (uintptr_t) ctrl,
        .len = sizeof(ctrl),
    };
    if (ioctl(fd, TBACCEL_IOCTL_RECV_CTRL, &rc) < 0) { perror("RECV_CTRL"); return 1; }
    printf("CTRL slot=%llu len=%u: %.*s\n", (unsigned long long) rc.slot, rc.len,
           (int) rc.len, ctrl);

    /* the data should have landed: wait for the slot fence */
    if (wait_fence(pr.fence_fd) == 0)
        printf("DATA arrived into region\n");
    else
        printf("slot fence timed out\n");

    close(fd);
    return 0;
}

/* submit a control blob plus a region to a peer slot */
static int cmd_send(const char *dev, uint64_t peer_slot, size_t size) {
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }

    int buf_fd = alloc_dmabuf(size);
    if (buf_fd < 0) {
        fprintf(stderr, "no dma-buf (need /dev/dma_heap/system): %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    /* fill the source region with a recognisable pattern */
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, buf_fd, 0);
    if (p != MAP_FAILED) {
        memset(p, 0x5a, size);
        munmap(p, size);
    }

    struct tbaccel_import imp = { .fd = (uint32_t) buf_fd };
    if (ioctl(fd, TBACCEL_IOCTL_IMPORT, &imp) < 0) { perror("IMPORT"); return 1; }

    struct tbaccel_region reg = { .handle = imp.handle, .offset = 0, .size = size };
    if (ioctl(fd, TBACCEL_IOCTL_REGION, &reg) < 0) { perror("REGION"); return 1; }

    const char *op = "op=matmul";
    struct tbaccel_submit sub = {
        .slot = peer_slot,
        .region = reg.region,
        .ctrl = (uint64_t) (uintptr_t) op,
        .ctrl_len = strlen(op) + 1,
        .in_fence_fd = -1,
    };
    if (ioctl(fd, TBACCEL_IOCTL_SUBMIT, &sub) < 0) { perror("SUBMIT"); return 1; }

    printf("submitted %zu bytes + ctrl to slot %llu, out_fence_fd=%d\n",
           size, (unsigned long long) peer_slot, sub.out_fence_fd);
    if (wait_fence(sub.out_fence_fd) == 0)
        printf("tx drained (out-fence fired)\n");
    else
        printf("out-fence timed out\n");

    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s info <dev>\n", argv[0]);
        fprintf(stderr, "       %s recv <dev> <size>\n", argv[0]);
        fprintf(stderr, "       %s send <dev> <peer-slot> <size>\n", argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "info"))
        return cmd_info(argv[2]);
    if (!strcmp(argv[1], "recv"))
        return cmd_recv(argv[2], argc > 3 ? strtoul(argv[3], NULL, 0) : 4096);
    if (!strcmp(argv[1], "send"))
        return cmd_send(argv[2], argc > 3 ? strtoull(argv[3], NULL, 0) : 0,
                        argc > 4 ? strtoul(argv[4], NULL, 0) : 4096);
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 1;
}
