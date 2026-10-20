/*
 * TbAccelService.h - local-side protocol service.
 *
 * Bound to IOThunderboltLocalNode, this advertises (and later withdraws) the
 * "accel" protocol directory so that peers running tbaccel discover us. This is
 * the macOS analogue of tbaccel_init() calling tb_register_property_dir()
 * with prtcid=1 / prtcvers=1.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef TBACCEL_SERVICE_H
#define TBACCEL_SERVICE_H

#include <IOKit/IOService.h>
#include "IOThunderboltFamily.h"

class TbAccelService : public IOService
{
    OSDeclareDefaultStructors(TbAccelService)

public:
    virtual bool init(OSDictionary *dictionary = nullptr) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

private:
    IOThunderboltLocalNode *fLocalNode;
};

#endif /* TBACCEL_SERVICE_H */
