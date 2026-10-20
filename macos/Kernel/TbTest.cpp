/*
 * TbTest.cpp - a deliberately minimal kext body: one IOService subclass with no
 * personalities and no protocol code. Used to bisect whether the kernel panic is
 * caused by our kext's class set / personalities or by the load path itself.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

class TbTest : public IOService
{
    OSDeclareDefaultStructors(TbTest)
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

#define super IOService
OSDefineMetaClassAndStructors(TbTest, IOService)

bool TbTest::start(IOService *provider)
{
    IOLog("TbTest: start provider=%p\n", provider);
    return super::start(provider);
}

void TbTest::stop(IOService *provider)
{
    IOLog("TbTest: stop\n");
    super::stop(provider);
}
