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
    if (fOwner)
        fOwner->setUserPeer(true);
    return fOwner != nullptr;
}

void TbAccelUserClient::stop(IOService *provider)
{
    if (fOwner)
        fOwner->setUserPeer(false);
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

    /* What the kernel actually received for this call.  Every selector that
     * takes structure input works except recv_ctrl, which fails its size guard,
     * so record the real numbers instead of inferring them. */
    {
        char v[96];
        snprintf(v, sizeof v, "sel%u in%u out%u",
                 (unsigned)selector,
                 (unsigned)args->structureInputSize,
                 (unsigned)args->structureOutputSize);
        fOwner->setProperty("TbAccelUcArgs", v);
    }

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
        a.fence   = 0;
        /* Optional GPU fence: scalarInput = { event page handle, value, ms }. */
        a.wait_handle = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;
        a.wait_value  = args->scalarInputCount > 1 ? args->scalarInput[1] : 0;
        a.wait_ms     = args->scalarInputCount > 2 ? (uint32_t)args->scalarInput[2] : 0;
        IOReturn r = gate->runAction(TbAccelClient::sSubmit, &a);
        /* The fence is the number of transmit commands queued by this submit;
         * kTbAccelMethodDrain waits for them to complete. */
        if (r == kIOReturnSuccess && args->scalarOutput && args->scalarOutputCount >= 1) {
            args->scalarOutput[0] = a.fence;
            args->scalarOutputCount = 1;
        }
        return r;
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

    case kTbAccelMethodDrain: {
        TbAccelDrainArgs a;
        if (args->scalarInputCount < 1)
            return kIOReturnBadArgument;
        a.fence   = args->scalarInput[0];
        a.timeout = args->scalarInputCount > 1 ? (uint32_t)args->scalarInput[1] : 2000;
        a.drained = 0;
        IOReturn r = gate->runAction(TbAccelClient::sDrain, &a);
        if (r == kIOReturnSuccess && args->scalarOutput && args->scalarOutputCount >= 1) {
            args->scalarOutput[0] = a.drained;
            args->scalarOutputCount = 1;
        }
        return r;
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
