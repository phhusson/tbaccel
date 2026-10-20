/*
 * TbAccelUserClient.h - user-space entry point for the tbaccel driver
 * (the macOS equivalent of /dev/tbaccelN + its ioctls).
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef TBACCEL_USERCLIENT_H
#define TBACCEL_USERCLIENT_H

#include <IOKit/IOUserClient.h>
#include "TbAccelInternal.h"

class TbAccelClient;

class TbAccelUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(TbAccelUserClient)

public:
    virtual bool     initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
    virtual bool     start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void     stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch,
                                    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;

    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                         IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;

private:
    TbAccelClient *fOwner;
};

#endif /* TBACCEL_USERCLIENT_H */
