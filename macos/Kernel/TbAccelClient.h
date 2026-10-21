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
#include <kern/thread_call.h>

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
    /* Queues the operation and returns its transfer fence; the worker thread
     * does the ARM, the GPU fence and the streaming. */
    IOReturn opSubmit(const tbaccel_submit *req, const void *blob, uint32_t blobLen,
                      uint64_t waitHandle, uint64_t waitValue, uint32_t waitMs,
                      uint64_t *outSeq);
    IOReturn opRecvCtrl(tbaccel_recv_ctrl *req, void *out, uint32_t outCap);
    IOReturn opSendCtrl(const tbaccel_send_ctrl *req, const void *blob);
    IOReturn opWaitSlot(uint64_t slot, uint32_t timeoutMs);
    IOReturn opMapBuffer(uint64_t handle, IOMemoryDescriptor **md);
    IOReturn opImportSurface(tbaccel_import_surface *req);
    IOReturn opEvent(tbaccel_event *req);
    /* Wait until transfer 'fence' (as returned by SUBMIT) has fully drained. */
    IOReturn opDrainWait(uint64_t fence, uint32_t timeoutMs, uint64_t *drained);
    IOReturn waitGpuValue(uint64_t handle, uint64_t value, uint32_t timeoutMs);
    /* Tear the fabric down and run bring-up again.  The peer restarts after a
     * Linux reboot with identical hop ids, so nothing else would reset us. */
    IOReturn opReset();
    uint64_t drainFence() const { return fTxSubmitted; }

    /* With a user client attached the TBX1 operations are delivered through
     * recv_ctrl instead of being executed in-kernel. */
    void setUserPeer(bool on) { fUserPeer = on; }
    bool pathsDone() const { return fPathsDone; }
    void retryPaths();
    bool userPeer() const { return fUserPeer; }

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
    static IOReturn sDrain(OSObject *, void *, void *, void *, void *);
    static IOReturn sReset(OSObject *, void *, void *, void *, void *);
    static IOReturn sStreamGate(OSObject *, void *, void *, void *, void *);


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
    static void *bufferBytes(TbAccelBuffer *b);

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
    /* ARM + wait for the peer's ACK + stream. The op rides the ARM, which is
     * what a Linux tbaccel receiver expects (tbaccel_transfer does the same). */
    IOReturn submitArm(uint64_t slotId, TbAccelRegion *region, uint64_t total,
                       const void *op, uint32_t opLen);
    IOReturn submitStream(uint64_t slotId, TbAccelRegion *region, uint64_t total,
                          const void *op, uint32_t opLen);
    /* submitStream sleeps on the gate (ACK, credits), so it must run inside a
     * gate action when called from a thread that does not hold the gate. */
    IOReturn streamViaGate(uint64_t slotId, TbAccelRegion *region, uint64_t total,
                           const void *op, uint32_t opLen);
    IOReturn submitOpToSlot(uint64_t slotId, TbAccelRegion *region, uint64_t total,
                            const void *op, uint32_t opLen,
                            uint64_t waitHandle, uint64_t waitValue, uint32_t waitMs);

    /* ---- peer (compute) side: TBX1 operations, no userspace involved ---- */
    bool     peerAllocRegion(uint64_t size, TbAccelRegion **out);
    void     peerOnBlob(const void *payload, uint32_t len);
    void     peerPrepare(const TbxMsg *m);
    void     peerDispatch(TbAccelSlot *slot);
    void     deliverOp(TbAccelSlot *slot);
    static IOReturn sPeerDispatch(OSObject *, void *, void *, void *, void *);
    void     peerMatmul(TbAccelSlot *inSlot, const TbxMsg *op);
    /* The stream opened by an ARM: payload frames land in whichever receive
     * buffer is at the head of the ring, so they are copied into the slot here. */
    bool     peerDataChunk(const uint8_t *data, uint32_t len);
    void     peerSchedule();
    static void peerThread(void *arg, thread_call_param_t param);
    void     peerStats(uint64_t t0, uint64_t t1, uint64_t computeNs);

    /* lookups */
    TbAccelBuffer *findBuffer(uint64_t handle);
    TbAccelRegion *findRegion(uint64_t id);
    TbAccelSlot   *findSlot(uint64_t id);
    TbAccelSlot   *findPeerSlot(uint64_t peerId);
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
    uint32_t fTxDataDone, fTxDataErr;   /* data-frame completions / last error */
    /*
     * Drain fence: every transmit command (control or data frame) bumps
     * fTxSubmitted when it is queued and fTxCompleted when the NHI reports it
     * done.  A submit returns fTxSubmitted as its fence, so waiting for
     * fTxCompleted >= fence means the source buffer is no longer being read by
     * the DMA engine and may be reused or freed.  This is deliberately a
     * counter rather than per-buffer bookkeeping: it makes the guarantee
     * ordered and cheap, and it is the shape a shared-memory event will take.
     */
    volatile uint64_t fTxSubmitted;
    volatile uint64_t fTxCompleted;

    /*
     * Async submit queue.  fTxSeqNext is the next transfer id, fTxSeqDone the
     * last transfer whose frames have all left.  A transfer's drain mark is the
     * fTxSubmitted value observed after its frames were queued; transfer ids are
     * streamed in order, so advancing fTxSeqDone is a simple walk.
     */
#define TBACCEL_TXQ      128
#define TBACCEL_TXMARKS  256
    OSArray          *fTxReqs;
    thread_call_t     fTxTC;
    volatile uint64_t fTxSeqNext;
    volatile uint64_t fTxSeqDone;
    struct TxMark {
        volatile bool set;
        uint64_t      mark;
    } fTxMark[TBACCEL_TXMARKS];
    static void txWorker(void *self, void *arg);
    void        txAdvanceDrain();
    void        txFlushQueue();
    uint32_t fRxAll, fRxOk, fRxBad, fRxLastType, fRxLastLen;
    uint32_t fRxData;      /* completed data transfers (peer slots + streams) */
    uint32_t fRxCtlMax;    /* largest control frame delivered intact */
    uint32_t fRxDataFrames;/* payload frames consumed into the open stream */
    TbAccelSlot *fRxStreamSlot;  /* slot the current ARM opened, or null */
    uint64_t     fRxStreamOff;
    uint64_t     fRxCreditPending;   /* consumed bytes not yet reported */
    /* Last few completions, unambiguously ordered (0 = most recent): the armed
     * length vs the length the NHI reports is what tells a split frame from a
     * rejected one. */
    struct RxEv {
        uint32_t armed, got, status, armedOff;
        uint32_t head[2];   /* first 8 bytes of the received data */
    } fRxEv[4];
    uint32_t     fRxEvSeq;
    void         rxTrace(uint32_t armed, uint32_t armedOff, uint32_t got,
                         uint32_t status, const uint8_t *data);
    void     wireStat();
    void     probeXdc();

    /* Peer-side bookkeeping and latency accounting. The operations run on a
     * thread_call rather than in the RX completion context: a MATMUL has to
     * wait for the client's ACK, and that ACK arrives on the same work loop
     * the completion runs on. */
    thread_call_t fPeerTC;
    volatile bool fPeerBusy;
    volatile bool fUserPeer;
    uint32_t fPeerRuns, fPeerSkips;   /* thread_call accounting */
    uint32_t fPeerSched, fPeerEnterFail;
    TbAccelRegion *fPeerY;          /* result buffer, grown on demand */
    uint32_t fPeerOps;
    uint32_t fPeerErrs;
    uint64_t fPeerLastUs, fPeerMinUs, fPeerMaxUs;
    uint64_t fPeerComputeUs, fPeerAckUs;
    char     fPeerErr[64];
};

#endif /* TBACCEL_CLIENT_H */
