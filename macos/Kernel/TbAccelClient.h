/*
 * TbAccelClient.h - the macOS tbaccel protocol engine (v2, credit streaming).
 *
 * Bound to a peer's `accel` IOThunderboltXDomainService. Unlike the first cut,
 * this is a plain IOService subclass (as AppleThunderboltIPTransmitter is) and
 * drives the family's queues/paths/commands directly.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef TBACCEL_CLIENT_H
#define TBACCEL_CLIENT_H

#include <IOKit/IOService.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSArray.h>

#include "IOThunderboltFamily.h"
#include "TbAccelInternal.h"

class TbAccelClient : public IOService
{
    OSDeclareDefaultStructors(TbAccelClient)

public:
    virtual bool     start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void     stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void     free() APPLE_KEXT_OVERRIDE;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                   UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;

    /* operations (run on the command gate) */
    IOReturn opGetInfo(tbaccel_info *info);
    IOReturn opImport(tbaccel_import *req);
    IOReturn opRegion(tbaccel_region *req);
    IOReturn opFree(const tbaccel_free *req);
    IOReturn opPostRecv(tbaccel_post_recv *req);
    IOReturn opSubmit(const tbaccel_submit *req, const void *blob, uint32_t blobLen);
    IOReturn opRecvCtrl(tbaccel_recv_ctrl *req, void *out, uint32_t outCap);
    IOReturn opSendCtrl(const tbaccel_send_ctrl *req, const void *blob);
    IOReturn opWaitSlot(uint64_t slot, uint32_t timeoutMs);
    IOReturn opMapBuffer(uint64_t handle, IOMemoryDescriptor **md);
    IOReturn opImportSurface(tbaccel_import_surface *req);
    IOReturn opEvent(tbaccel_event *req);

    IOCommandGate *getGate()       const { return fGate; }
    IOWorkLoop    *getWorkLoopG()  const { return fWorkLoop; }

    /* gate trampolines */
    static IOReturn sGetInfo(OSObject *, void *, void *, void *, void *);
    static IOReturn sImport(OSObject *, void *, void *, void *, void *);
    static IOReturn sRegion(OSObject *, void *, void *, void *, void *);
    static IOReturn sFree(OSObject *, void *, void *, void *, void *);
    static IOReturn sPostRecv(OSObject *, void *, void *, void *, void *);
    static IOReturn sSubmit(OSObject *, void *, void *, void *, void *);
    static IOReturn sRecvCtrl(OSObject *, void *, void *, void *, void *);
    static IOReturn sSendCtrl(OSObject *, void *, void *, void *, void *);
    static IOReturn sWaitSlot(OSObject *, void *, void *, void *, void *);
    static IOReturn sMapBuffer(OSObject *, void *, void *, void *, void *);
    static IOReturn sImportSurface(OSObject *, void *, void *, void *, void *);
    static IOReturn sEvent(OSObject *, void *, void *, void *, void *);

private:
    /* setup */
    bool setupPaths();
    bool setupQueues();
    bool tryXDomainRx();
    bool startControlRx();
    void teardown();

    /*
     * Path creation is a separate phase from ring bring-up. Thunderbolt-net
     * does rings first, then a login exchange, and enables its paths LAST
     * (tbnet_connected_work); the hop ids are a product of that handshake,
     * not something either side can pick unilaterally. Our XDomain service
     * properties (inhopid/outhopid) are the same handshake, so a path may
     * only be attempted once the peer has published them.
     */
    bool readPeerHops(uint16_t *inHop, uint16_t *outHop);
    bool tryPaths();

    /* fabric bring-up is retried via a timer: the NHI/link is not always ready
     * at the moment the XDomainService matches. */
    void attemptBringup();
    static void retryTimeout(OSObject *owner, IOTimerEventSource *timer);
    void armRetry(unsigned int ms);

    /* control frames */
    IOReturn sendCtl(uint16_t type, uint64_t slot, uint32_t len,
                     const void *payload, uint32_t payloadLen);
    void     handleCtl(const TbAccelWire *w, const uint8_t *payload, uint32_t payLen);
    /* data */
    IOReturn armOne(TbAccelSlot *slot, uint64_t off);
    IOReturn armWindow(TbAccelSlot *slot);
    IOReturn streamRegion(TbAccelRegion *region, uint64_t total);
    IOReturn waitList(OSArray *list, void *event, uint32_t timeoutMs, uint64_t slot);

    /* lookups */
    TbAccelBuffer *findBuffer(uint64_t handle);
    TbAccelRegion *findRegion(uint64_t id);
    TbAccelSlot   *findSlot(uint64_t id);
    void           wakeSlots();

    /* completions */
    static void ctlTxComplete(void *target, void *refcon, IOReturn status,
                              IOThunderboltTransmitCommand *cmd);
    static void dataTxComplete(void *target, void *refcon, IOReturn status,
                               IOThunderboltTransmitCommand *cmd);
    static void ctlRxComplete(void *target, void *refcon, IOReturn status,
                              IOThunderboltReceiveCommand *cmd);
    static void dataRxComplete(void *target, void *refcon, IOReturn status,
                               IOThunderboltReceiveCommand *cmd);

    /* state */
    IOThunderboltXDomainService *fService;
    IOThunderboltXDomainServiceClient *fXdc;    /* family path/queue owner */
    IOThunderboltXDomainLink    *fLink;
    IOThunderboltController     *fController;
    IOWorkLoop                  *fWorkLoop;
    IOCommandGate               *fGate;

    IOThunderboltPath *fTxPath;     /* local transmit path  (out hopid) */
    IOThunderboltXDomainPath *fXdp; /* family-style path carrying a path id */
    IOThunderboltPath *fRxPath;     /* local receive path   (in hopid)  */

    /*
     * Linux tbaccel uses exactly ONE transmit ring and ONE receive ring; both
     * control and data frames share that pair (sof == eof == TBACCEL_PDF).
     */
    IOThunderboltTransmitQueue *fTxQ;
    IOThunderboltReceiveQueue  *fRxQ;

    IOLock  *fTableLock;
    OSArray *fBuffers;
    OSArray *fRegions;
    OSArray *fSlots;
    OSArray *fBlobs;
    OSArray *fAcks;
    OSArray *fCtlRxPool;    /* TbAccelCtrlRx * */
    OSArray *fDataRxPool;   /* TbAccelXfer * (unused placeholders) */
    OSArray *fEvents;       /* TbAccelEvent * */

    uint64_t fNextId;
    uint64_t fCredit;       /* bytes the sender may still emit */
    bool     fReady;
    bool     fPathsDone;    /* peer handshake seen, paths allocated */
    bool     fPeerHopsKnown;
    bool     fPathAttempted; /* the (bounded) attempt was made for this state */
    uint16_t fPeerIn, fPeerOut;
    uint32_t fPathPolls;
    uint32_t fAttempts;
    IOTimerEventSource *fTimer;
    uint8_t  fLocalUUID[16];
    uint8_t  fRemoteUUID[16];

    /* Wire diagnostics: distinguish "the NHI never drained our TX ring" from
     * "the frame went out but the peer never answered". */
    uint32_t fTxSub, fTxDone, fTxSt, fTxLastType;
    uint32_t fRxAll, fRxOk, fRxBad, fRxLastType, fRxLastLen;
    void     wireStat();
    void     probeXdc();
};

#endif /* TBACCEL_CLIENT_H */
