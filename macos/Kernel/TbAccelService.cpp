/*
 * TbAccelService.cpp - local-side tbaccel protocol service.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <IOKit/IOLib.h>
#include "TbAccelService.h"
#include "TbAccelInternal.h"
#include "TbAccelLog.h"

#define super IOService
OSDefineMetaClassAndStructors(TbAccelService, IOService)

#define TBACCEL_TAG "TbAccel"

/*
 * IOKit diagnostics: set ioreg-visible markers at each matching phase so we can
 * tell (without relying on log capture) whether init()/probe()/start() run.
 */
bool TbAccelService::init(OSDictionary *dictionary)
{
    if (!super::init(dictionary))
        return false;
    TBLOG2("TbAccelService::init\n");
    setProperty("TbAccelInit", kOSBooleanTrue);
    return true;
}

IOService *TbAccelService::probe(IOService *provider, SInt32 *score)
{
    TBLOG2("TbAccelService::probe provider=%p\n", provider);
    setProperty("TbAccelProbe", kOSBooleanTrue);
    return super::probe(provider, score);
}

bool TbAccelService::start(IOService *provider)
{
    TBLOG2("TbAccelService::start enter provider=%p\n", provider);
    setProperty("TbAccelStarted", kOSBooleanTrue);

    if (!super::start(provider)) {
        TBLOG2("TbAccelService super::start failed\n");
        return false;
    }

    fLocalNode = OSDynamicCast(IOThunderboltLocalNode, provider);
    if (!fLocalNode) {
        TBLOG2("provider is not IOThunderboltLocalNode\n");
        return false;
    }

    /*
     * Publish the protocol directory with our path HopIDs attached, so the peer
     * mirrors them instead of both ends guessing:
     *     its in  = our out,   its out = our in
     * (Linux: tbaccel_get_peer_hopids(), mirrored in tbaccel_start()).
     * HopIDs must be >= 8 - 0..7 are reserved, which is why every path attempt
     * with hop 2 failed.
     */
    OSDictionary *xd = OSDictionary::withCapacity(8);
    bool published = false;
    if (xd) {
        OSNumber *n;
        if ((n = OSNumber::withNumber((unsigned long long)1, 32)))              { xd->setObject("prtcid",   n); n->release(); }
        if ((n = OSNumber::withNumber((unsigned long long)1, 32)))              { xd->setObject("prtcvers", n); n->release(); }
        if ((n = OSNumber::withNumber((unsigned long long)0, 32)))              { xd->setObject("prtcrevs", n); n->release(); }
        if ((n = OSNumber::withNumber((unsigned long long)0, 64)))              { xd->setObject("prtcstns", n); n->release(); }
        if ((n = OSNumber::withNumber((unsigned long long)TBACCEL_PATH_HOP, 32))){ xd->setObject("inhopid",  n); n->release(); }
        if ((n = OSNumber::withNumber((unsigned long long)TBACCEL_PATH_HOP, 32))){ xd->setObject("outhopid", n); n->release(); }
        published = fLocalNode->publishXDServiceDictionary((char *)"accel", xd);
        xd->release();
        setProperty("TbAccelPublishDict", published ? "yes" : "no");
    }
    if (!published) {
        /* Fallback: plain advertisement without HopIDs. */
        published = fLocalNode->publishXDService((char *)"accel", 1, 1, 0, 0);
        setProperty("TbAccelPublishDict", "fallback");
    }
    if (!published) {
        TBLOG2("publishXDService(accel) failed\n");
        setProperty("TbAccelPublishFail", kOSBooleanTrue);
        return false;
    }
    {
        char v[32];
        snprintf(v, sizeof v, "%u/%u", TBACCEL_PATH_HOP, TBACCEL_PATH_HOP);
        setProperty("TbAccelHopAdvertised", v);
    }
    setProperty("TbAccelPublished", kOSBooleanTrue);
    fLocalNode->registerService(0);
    registerService(0);

    TBLOG2("advertised the 'accel' service\n");
    return true;
}

void TbAccelService::stop(IOService *provider)
{
    if (fLocalNode) {
        fLocalNode->unpublishXDService((char *)"accel");
        fLocalNode = nullptr;
    }
    super::stop(provider);
}
