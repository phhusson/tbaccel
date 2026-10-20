/*
 * TbAccelUserClient.cpp - external-method dispatch (the macOS equivalent of the
 * Linux /dev/tbaccelN ioctls).
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>

#include "TbAccelUserClient.h"
#include "TbAccelClient.h"

#define super IOUserClient
OSDefineMetaClassAndStructors(TbAccelUserClient, IOUserClient)

bool TbAccelUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type))
        return false;
    fOwner = nullptr;
    return true;
}

bool TbAccelUserClient::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    fOwner = OSDynamicCast(TbAccelClient, provider);
    return fOwner != nullptr;
}

void TbAccelUserClient::stop(IOService *provider)
{
    fOwner = nullptr;
    super::stop(provider);
}

IOReturn TbAccelUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                           IOExternalMethodDispatch *dispatch,
                                           OSObject *target, void *reference)
{
    if (!fOwner)
        return kIOReturnNotAttached;
    IOCommandGate *gate = fOwner->getGate();
    if (!gate)
        return kIOReturnNotReady;

    switch (selector) {
    case kTbAccelMethodGetInfo:
        if (args->structureOutputSize < sizeof(tbaccel_info))
            return kIOReturnBadArgument;
        return gate->runAction(TbAccelClient::sGetInfo, args->structureOutput);

    case kTbAccelMethodImport:
        if (args->structureInputSize < sizeof(tbaccel_import) ||
            args->structureOutputSize < sizeof(tbaccel_import))
            return kIOReturnBadArgument;
        memcpy(args->structureOutput, args->structureInput, sizeof(tbaccel_import));
        return gate->runAction(TbAccelClient::sImport, args->structureOutput);

    case kTbAccelMethodImportSurface:
        if (args->structureInputSize < sizeof(tbaccel_import_surface) ||
            args->structureOutputSize < sizeof(tbaccel_import_surface))
            return kIOReturnBadArgument;
        memcpy(args->structureOutput, args->structureInput, sizeof(tbaccel_import_surface));
        return gate->runAction(TbAccelClient::sImportSurface, args->structureOutput);

    case kTbAccelMethodEvent:
        if (args->structureInputSize < sizeof(tbaccel_event) ||
            args->structureOutputSize < sizeof(tbaccel_event))
            return kIOReturnBadArgument;
        memcpy(args->structureOutput, args->structureInput, sizeof(tbaccel_event));
        return gate->runAction(TbAccelClient::sEvent, args->structureOutput);

    case kTbAccelMethodRegion:
        if (args->structureInputSize < sizeof(tbaccel_region) ||
            args->structureOutputSize < sizeof(tbaccel_region))
            return kIOReturnBadArgument;
        memcpy(args->structureOutput, args->structureInput, sizeof(tbaccel_region));
        return gate->runAction(TbAccelClient::sRegion, args->structureOutput);

    case kTbAccelMethodFree: {
        tbaccel_free tmp;
        if (args->structureInputSize < sizeof(tmp))
            return kIOReturnBadArgument;
        memcpy(&tmp, args->structureInput, sizeof(tmp));
        return gate->runAction(TbAccelClient::sFree, &tmp);
    }

    case kTbAccelMethodPostRecv:
        if (args->structureInputSize < sizeof(tbaccel_post_recv) ||
            args->structureOutputSize < sizeof(tbaccel_post_recv))
            return kIOReturnBadArgument;
        memcpy(args->structureOutput, args->structureInput, sizeof(tbaccel_post_recv));
        return gate->runAction(TbAccelClient::sPostRecv, args->structureOutput);

    case kTbAccelMethodSubmit: {
        TbAccelSubmitArgs a;
        if (args->structureInputSize < sizeof(tbaccel_submit))
            return kIOReturnBadArgument;
        memcpy(&a.req, args->structureInput, sizeof(tbaccel_submit));
        a.blob    = (const uint8_t *)args->structureInput + sizeof(tbaccel_submit);
        a.blobLen = args->structureInputSize - sizeof(tbaccel_submit);
        return gate->runAction(TbAccelClient::sSubmit, &a);
    }

    case kTbAccelMethodRecvCtrl: {
        TbAccelRecvCtrlArgs a;
        if (args->structureInputSize < sizeof(tbaccel_recv_ctrl) ||
            args->structureOutputSize < sizeof(tbaccel_recv_ctrl))
            return kIOReturnBadArgument;
        memcpy(&a.req, args->structureInput, sizeof(tbaccel_recv_ctrl));
        a.out = (uint8_t *)args->structureOutput + sizeof(tbaccel_recv_ctrl);
        a.cap = args->structureOutputSize - sizeof(tbaccel_recv_ctrl);
        IOReturn r = gate->runAction(TbAccelClient::sRecvCtrl, &a);
        if (r == kIOReturnSuccess)
            memcpy(args->structureOutput, &a.req, sizeof(tbaccel_recv_ctrl));
        return r;
    }

    case kTbAccelMethodSendCtrl: {
        TbAccelSendCtrlArgs a;
        if (args->structureInputSize < sizeof(tbaccel_send_ctrl))
            return kIOReturnBadArgument;
        memcpy(&a.req, args->structureInput, sizeof(tbaccel_send_ctrl));
        a.blob = (const uint8_t *)args->structureInput + sizeof(tbaccel_send_ctrl);
        return gate->runAction(TbAccelClient::sSendCtrl, &a);
    }

    case kTbAccelMethodWaitSlot: {
        TbAccelWaitSlotArgs a;
        if (args->scalarInputCount < 2)
            return kIOReturnBadArgument;
        a.slot = args->scalarInput[0];
        a.timeout = (uint32_t)args->scalarInput[1];
        return gate->runAction(TbAccelClient::sWaitSlot, &a);
    }

    default:
        return kIOReturnUnsupported;
    }
}

IOReturn TbAccelUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                IOMemoryDescriptor **memory)
{
    if (!fOwner)
        return kIOReturnNotAttached;
    IOMemoryDescriptor *md = nullptr;
    IOReturn r = fOwner->opMapBuffer(type, &md);
    if (r != kIOReturnSuccess)
        return r;
    md->retain();
    *memory = md;
    if (options)
        *options = kIOMapAnywhere;
    return kIOReturnSuccess;
}
