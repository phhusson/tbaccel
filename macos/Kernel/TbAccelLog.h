/*
 * TbAccelLog.h - os_log-based logging for the kext.
 *
 * macOS 27 does not surface legacy IOLog() from third-party kexts in the
 * unified log; Apple's own kexts use os_log(). Use TBLOG()/TBLOGP() so the
 * kernel ring carries our messages (visible to `log show`).
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef TBACCEL_LOG_H
#define TBACCEL_LOG_H

#include <os/log.h>
#include <IOKit/IOLib.h>

#ifndef TBACCEL_TAG
#define TBACCEL_TAG "TbAccel"
#endif

/* Plain-text message. os_log requires a literal format string. */
#define TBLOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "TbAccel: " fmt, ##__VA_ARGS__)

/* Same, but mark pointers public so they are not redacted as <private>. */
#define TBLOGP(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "TbAccel: " fmt, ##__VA_ARGS__)

/* Keep IOLog too: harmless on older kernels, and some flows still capture it. */
#define TBLOG2(fmt, ...) do { \
    TBLOG(fmt, ##__VA_ARGS__); \
    IOLog("%s: " fmt, TBACCEL_TAG, ##__VA_ARGS__); \
} while (0)

#endif /* TBACCEL_LOG_H */
