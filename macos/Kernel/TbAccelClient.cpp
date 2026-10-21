/*
 * TbAccelClient.cpp - macOS tbaccel protocol engine (v2, credit streaming),
 * with IOSurface buffers and MTLSharedEvent-shaped fences for the ANE/MTL
 * coop path (see ggml/src/ggml-ane).
 *
 * Model (matches linux/tbaccel.c v2 and AppleThunderboltIP):
 *   - plain IOService subclass bound to IOThunderboltXDomainService
 *   - two NHI rings per direction: control and data
 *     (IOThunderboltTransmitQueue / IOThunderboltReceiveQueue)
 *   - frames are IOThunderboltTransmitCommand / ReceiveCommand with SOF/EOF
 *   - completion is a 24-byte {target, fn, refcon} value, invoked
 *     fn(target, refcon, status, cmd)
 *
 * Control protocol (identical wire format to Linux):
 *   ARM{slot,len} -> ACK{slot,armed}+total -> stream armed bytes ->
 *   CREDIT{bytes} as the receiver re-arms -> DONE{slot}+blob
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>
#include <kern/clock.h>
#include <kern/task.h>

#include "TbAccelClient.h"
#include "TbAccelUserClient.h"
#include "TbAccelLog.h"
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

#define super IOService

/* When the family's XDomain client exists it owns path setup (it handles the
 * peer's request via allocateXDomainPath/allocateXDomainQueue), so building our
 * own path would both conflict with it and consume the peer's hop ids. */
static bool sUseXdc;

/* Kill switch for the client experiment: boot-arg -tbaccel_noxdc 1 (or just
 * -tbaccel_noxdc) makes the driver behave exactly as before. */
static bool sNoXdc;
static bool sReady;          /* opt-in: call clientReady(true) */
static bool sBootArgsRead;

static void readBootArgs()
{
    if (sBootArgsRead)
        return;
    sBootArgsRead = true;
    int v = 0;
    if (PE_parse_boot_argn("tbaccel_noxdc", &v, sizeof v))
        sNoXdc = (v != 0);
    v = 0;
    if (PE_parse_boot_argn("tbaccel_ready", &v, sizeof v))
        sReady = (v != 0);
}

/* Path notifications. The family only invokes these when a refcon is present,
 * and the refcon is set behind attach() - which we do not call. They exist so
 * the two setter calls have a valid, ABI-safe target (four register arguments,
 * return value discarded - see the Callback typedef). */
static void tbAccelXdcNewPath(void *refcon, void *path, void *unused2, uint32_t unused3)
{
    (void)refcon; (void)path; (void)unused2; (void)unused3;
}

static void tbAccelXdcDeactPath(void *refcon, void *path, void *unused2, uint32_t unused3)
{
    (void)refcon; (void)path; (void)unused2; (void)unused3;
}
OSDefineMetaClassAndStructors(TbAccelClient, IOService)

OSDefineMetaClassAndStructors(TbAccelBuffer,  OSObject)

/* Buffers own their descriptor (newBuffer()/opImport() hand it over), so this
 * is where the memory is actually released. */
void TbAccelBuffer::free()
{
    if (map) { map->release(); map = nullptr; kva = nullptr; }
    if (md) {
        if (prepared) { md->complete(); prepared = false; }
        md->release();
        md = nullptr;
    }
    OSObject::free();
}

/*
 * Bytes for CPU access.  Kernel buffers answer getBytesNoCopy(); imported user
 * buffers only have the kernel mapping made in opImportSurface().
 */
void *TbAccelClient::bufferBytes(TbAccelBuffer *b)
{
    if (!b || !b->md)
        return nullptr;
    if (b->kva)
        return b->kva;
    IOBufferMemoryDescriptor *mb = OSDynamicCast(IOBufferMemoryDescriptor, b->md);
    return mb ? mb->getBytesNoCopy() : nullptr;
}

OSDefineMetaClassAndStructors(TbAccelRegion,  OSObject)
OSDefineMetaClassAndStructors(TbAccelSlot,    OSObject)
OSDefineMetaClassAndStructors(TbAccelBlob,    OSObject)
OSDefineMetaClassAndStructors(TbAccelAck,     OSObject)
OSDefineMetaClassAndStructors(TbAccelXfer,    OSObject)
OSDefineMetaClassAndStructors(TbAccelCtrlRx,  OSObject)
OSDefineMetaClassAndStructors(TbAccelEvent,   OSObject)


#define TBACCEL_TAG "TbAccel"

/* --------------------------------------------------------------- lifecycle */

bool TbAccelClient::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    TBLOG2("TbAccelClient::start enter provider=%p\n", provider);
    setProperty("TbAccelClientStarted", kOSBooleanTrue);
    {
        /* If the XDomain service flaps, start() runs repeatedly - which would
         * turn our client creation into a feedback loop. Count it. */
        static uint32_t sStartCount;
        char sv[32];
        snprintf(sv, sizeof sv, "%u", (unsigned)++sStartCount);
        setProperty("TbAccelStartCount", sv);
    }

    fService = OSDynamicCast(IOThunderboltXDomainService, provider);
    if (!fService) {
        TBLOG2("TbAccelClient provider is not IOThunderboltXDomainService\n");
        setProperty("TbAccelClientFail", "not-xdomain-service");
        return true;
    }
    setProperty("TbAccelClientStep", "cast");
    /* Identifies the loaded build in ioreg, so "did my build actually run?" is
     * answerable without comparing LC_UUIDs. */
    setProperty("TbAccelBuildTag", TBACCEL_BUILD_TAG);

    fLink = fService->getXDomainLink();
    fController = fService->getController();
    TBLOG2("TbAccelClient service=%p link=%p controller=%p\n", fService, fLink, fController);
    if (!fLink || !fController) {
        TBLOG2("TbAccelClient no XDomain link/controller\n");
        setProperty("TbAccelClientFail", "no-link-or-controller");
        return true;
    }
    setProperty("TbAccelClientStep", "link-controller");

    /* Log the HopIDs the peer published. Linux mirrors ours:
     * its in = our out, its out = our in (tbaccel_get_peer_hopids()).
     * At this instant they are usually absent (65535): the peer is only just
     * loading. Their appearance is the handshake that gates path creation -
     * readPeerHops() is re-run from the retry timer for exactly that reason. */
    {
        uint16_t in = 0, out = 0;
        (void)readPeerHops(&in, &out);
    }

    /* Settle "is the hop-id exchange a race or a different table?" by showing
     * what this dictionary actually contains. ioreg shows inhopid on this very
     * node while our lookup misses it, so empty vs. partial vs. wrong-type
     * matters. */
    {
        OSDictionary *props = fService ? fService->getProperties() : nullptr;
        char keys[256];
        size_t n = 0;
        keys[0] = 0;
        if (!props) {
            snprintf(keys, sizeof keys, "<null>");
        } else {
            OSCollectionIterator *it = OSCollectionIterator::withCollection(props);
            if (it) {
                bool first = true;
                OSObject *o;
                while ((o = it->getNextObject())) {
                    const char *s = nullptr;
                    if (OSSymbol *sym = OSDynamicCast(OSSymbol, o))
                        s = sym->getCStringNoCopy();
                    else if (OSString *str = OSDynamicCast(OSString, o))
                        s = str->getCStringNoCopy();
                    if (!s) s = "?";
                    int w = snprintf(keys + n, sizeof keys - n, "%s%s",
                                     first ? "" : ",", s);
                    if (w <= 0 || (size_t)w >= sizeof keys - n)
                        break;
                    n += (size_t)w;
                    first = false;
                }
                it->release();
            }
            if (!n)
                snprintf(keys, sizeof keys, "<empty>");
        }
        setProperty("TbAccelPropKeys", keys);
        if (OSNumber *pc = OSNumber::withNumber((unsigned long long)(props ? props->getCount() : 0xffffffffu), 32)) {
            setProperty("TbAccelPropCount", pc);
            pc->release();
        }
    }

    /* The family's per-service client is what owns XDomain path setup: an
     * incoming path request from the peer is handled by
     * processPathSetupRequest -> allocateXDomainPath() + allocateXDomainQueue().
     * Without a client that request is dropped, which is why the peer's
     * enable_paths() succeeds and installs nothing here.
     *
     * `withXDomainService()` alone is the one call in this class that has been
     * proven safe. Do NOT add attach(), clientReady() or the per-path getters:
     * all three dereference NULL inside the family and take the machine down
     * (see docs 27.0.5 / 27.0.6). */
    readBootArgs();
    /* Boot-arg form matters: the numeric convention is `name=value`, and
     * `-name value` is not guaranteed to parse. Publish the raw string so a
     * missing marker can be told apart from an unparsed argument. */
    {
        const char *ba = PE_boot_args();
        setProperty("TbAccelBootArgs", ba ? ba : "");
    }
    if (!sNoXdc) {
        fXdc = IOThunderboltXDomainServiceClient::withXDomainService(fService);
        setProperty("TbAccelXdc", fXdc ? "created" : "null");
        if (fXdc) {
            /* Initialise its path/queue tables. No path callbacks: they take a
             * 0x28-byte struct by value and are optional anyway (the family
             * checks the refcon first and returns NotReady without calling).
             * registerService's argument is unused by the implementation. */
            bool reg = fXdc->registerService(0);
            setProperty("TbAccelXdcReg", reg ? "ok" : "fail");

            /* Opt-in (-tbaccel_ready 1): clientReady touches [client+0x160]
             * without a null check, so it is only safe if initWithService set
             * that object up. Reversed body: idempotent on +0x108, sets the
             * ready byte, then dispatches through [0x160]. */
            if (sReady) {
                bool r = fXdc->clientReady(true);
                setProperty("TbAccelXdcReady", r ? "ok" : "fail");
            }
        }
    } else {
        setProperty("TbAccelXdc", "disabled");
    }

    fLink->getXDomainUUID(fRemoteUUID);
    TBLOG2("TbAccelClient remote uuid=%02x%02x%02x%02x...\n",
           fRemoteUUID[0], fRemoteUUID[1], fRemoteUUID[2], fRemoteUUID[3]);

    fWorkLoop = getWorkLoop();
    if (!fWorkLoop) {
        /* No provider work loop: create our own so the command gate and the
         * bring-up retry timer actually run. */
        fWorkLoop = IOWorkLoop::workLoop();
        setProperty("TbAccelClientOwnWl", kOSBooleanTrue);
    }
    if (fWorkLoop) {
        fWorkLoop->retain();
        fGate = IOCommandGate::commandGate(this);
        if (fGate)
            fWorkLoop->addEventSource(fGate);
    }
    TBLOG2("TbAccelClient workloop=%p gate=%p\n", fWorkLoop, fGate);
    setProperty("TbAccelWlValid", fWorkLoop != nullptr ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("TbAccelClientStep", "workloop");

    fTableLock = IOLockAlloc();
    fBuffers   = OSArray::withCapacity(8);
    fRegions   = OSArray::withCapacity(8);
    fSlots     = OSArray::withCapacity(8);
    fBlobs     = OSArray::withCapacity(8);
    fAcks      = OSArray::withCapacity(8);
    fCtlRxPool = OSArray::withCapacity(TBACCEL_RING_SIZE);
    fDataRxPool= OSArray::withCapacity(TBACCEL_RING_SIZE);
    fEvents    = OSArray::withCapacity(8);
    fNextId    = 1;
    fCredit    = 0;

    fTimer = IOTimerEventSource::timerEventSource(this, &TbAccelClient::retryTimeout);
    if (fTimer && fWorkLoop)
        fWorkLoop->addEventSource(fTimer);
    setProperty("TbAccelTimer", fTimer != nullptr ? kOSBooleanTrue : kOSBooleanFalse);

    /* Peer operations run here, not on the work loop: a MATMUL must wait for the
     * client's ACK, and that ACK is delivered by the same work loop. */
    fPeerTC = thread_call_allocate(&TbAccelClient::peerThread, this);
    setProperty("TbAccelPeerTC", fPeerTC ? kOSBooleanTrue : kOSBooleanFalse);
    fPeerBusy = false;
    fUserPeer = false;
    fPeerY = nullptr;
    fPeerOps = fPeerErrs = 0;
    fPeerRuns = fPeerSkips = 0;
    fPeerSched = fPeerEnterFail = 0;
    fRxStreamSlot = nullptr;
    fRxStreamOff = 0;
    fRxCreditPending = 0;
    fRxDataFrames = 0;
    memset(fRxEv, 0, sizeof fRxEv);
    fRxEvSeq = 0;
    fPeerLastUs = fPeerMinUs = fPeerMaxUs = 0;
    fPeerComputeUs = fPeerAckUs = 0;
    fPeerErr[0] = 0;

    fAttempts = 0;
    fPathsDone = false;
    fPeerHopsKnown = false;
    fPeerIn = fPeerOut = 0;
    fPathPolls = 0;
    fPathAttempted = false;

    /* The NHI/link is not reliably ready the instant the XDomainService
     * matches (ring allocate/start is state dependent), so the fabric bring-up
     * is retried from the timer until it succeeds. */
    attemptBringup();
    return true;
}

void TbAccelClient::attemptBringup()
{
    if (fReady) {
        /* Rings are up; only the path phase can still be outstanding. It is
         * gated on the peer's hop-id properties (the handshake), so retrying
         * here is the normal case until the peer loads. */
        if (!fPathsDone)
            (void)tryPaths();
        return;
    }
    fAttempts++;
    {
        OSNumber *n = OSNumber::withNumber(fAttempts, 32);
        if (n) { setProperty("TbAccelTry", n); n->release(); }
    }

    if (fAttempts > 1)
        teardown();   /* clear any partial state from the previous attempt */

    if (!setupPaths()) {
        TBLOG2("attempt %u: setupPaths failed\n", fAttempts);
        setProperty("TbAccelClientFailStage", "paths");
        setProperty("TbAccelClientFail", "paths");
        armRetry(1000);
        return;
    }
    if (!setupQueues()) {
        TBLOG2("attempt %u: setupQueues failed\n", fAttempts);
        setProperty("TbAccelClientFailStage", "queues");
        armRetry(1000);
        return;
    }
    fAttempts = 0;
    setProperty("TbAccelClientStep", "queues");
    if (!startControlRx()) {
        TBLOG2("attempt: startControlRx failed\n");
        setProperty("TbAccelClientFailStage", "controlrx");
        setProperty("TbAccelClientFail", "controlrx");
        armRetry(1000);
        return;
    }
    setProperty("TbAccelClientStep", "controlrx");

    fReady = true;
    fTxSub = fTxDone = fTxSt = fTxLastType = 0;
    fTxSubmitted = fTxCompleted = 0;
    fRxAll = fRxOk = fRxBad = fRxLastType = fRxLastLen = 0;
    fRxData = 0;
    fRxDataFrames = 0;
    fRxCtlMax = 0;
    wireStat();

    /* One-shot TX probe. Type 0x7f is not in Linux's rx_control switch, so the
     * peer ignores it; it exists purely to answer "does our TX ring actually
     * drain?" via the TbAccelTxStat done counter. Safe: no slot, no state. */
    sendCtl(0x7f, 0, 0, nullptr, 0);
    setProperty("TbAccelTxProbe", "sent");
    registerService(0);

    /* Path enable comes last, after the rings are up and the RX ring is primed
     * (tbnet_connected_work ordering). tryPaths() defers until the peer has
     * published its hop ids. */
    if (tryPaths())
        TBLOG2("TbAccelClient up (control/data rings, credit window %d frames)\n",
               TBACCEL_WINDOW);
    else
        TBLOG2("TbAccelClient rings up, waiting for peer handshake\n");
    setProperty("TbAccelClientStep", "up");
}

void TbAccelClient::armRetry(unsigned int ms)
{
    if (!fTimer) {
        setProperty("TbAccelClientFail", "no-timer");
        return;
    }
    /* Ring bring-up gives up after ~2 minutes. The path phase is a poll for the
     * peer's handshake, so it must keep waiting: the peer can be loaded, or
     * reloaded, long after we boot. */
    if (!fReady && fAttempts >= 120) {
        setProperty("TbAccelClientFail", "gave-up");
        return;
    }
    fTimer->setTimeout(ms, kMillisecondScale);
}

void TbAccelClient::retryTimeout(OSObject *owner, IOTimerEventSource *timer)
{
    TbAccelClient *me = (TbAccelClient *)owner;
    if (me)
        me->attemptBringup();
}

void TbAccelClient::stop(IOService *provider)
{
    fReady = false;
    if (fPeerTC) {
        thread_call_cancel(fPeerTC);
        thread_call_free(fPeerTC);
        fPeerTC = nullptr;
    }
    if (fTimer) {
        fTimer->cancelTimeout();
        if (fWorkLoop)
            fWorkLoop->removeEventSource(fTimer);
        fTimer->release();
        fTimer = nullptr;
    }
    teardown();
    if (fGate) {
        if (fWorkLoop)
            fWorkLoop->removeEventSource(fGate);
        fGate->release();
        fGate = nullptr;
    }
    if (fWorkLoop) { fWorkLoop->release(); fWorkLoop = nullptr; }
    super::stop(provider);
}

void TbAccelClient::free()
{
    if (fBuffers)   fBuffers->release();
    if (fRegions)   fRegions->release();
    if (fSlots)     fSlots->release();
    if (fBlobs)     fBlobs->release();
    if (fAcks)      fAcks->release();
    if (fCtlRxPool) fCtlRxPool->release();
    if (fDataRxPool)fDataRxPool->release();
    if (fEvents)    fEvents->release();
    if (fXdc)       fXdc->release();
    if (fTableLock) IOLockFree(fTableLock);
    super::free();
}

IOReturn TbAccelClient::newUserClient(task_t owningTask, void *securityID,
                                      UInt32 type, IOUserClient **handler)
{
    TbAccelUserClient *client = OSTypeAlloc(TbAccelUserClient);
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type) ||
        !client->attach(this) || !client->start(this)) {
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

/* -------------------------------------------------------------- link setup */

/*
 * Data/control paths. The exact fabric configuration (source/destination ports
 * and HopID ranges) follows AppleThunderboltIPTransmitter::configureTxPath and
 * is done through virtual methods on the IOThunderboltPort objects; that part
 * still needs to be pinned down against a live peer. For now we create the
 * path objects and give them initial credits, which is enough to exercise the
 * queue/command plumbing.
 */
bool TbAccelClient::setupPaths()
{
    /* Paths are created in setupQueues(): the path's source hop id must equal
     * the NHI ring hop, which we only know after the ring is allocated. */
    setProperty("TbAccelClientStep", "paths");
    return true;
}

/* Compact record of every path attempt this boot: one reboot then tells us all
 * the pairs/modes that were tried, not just the last one. */
static char   sTryLog[448];
static size_t sTryLogLen;

/* Build+allocate one path for an explicit (source,destination) hop pair.
 *
 * The port pair matters: IOThunderboltPath::allocate() allocates an *input hop
 * id on each port* from the ranges we set (its own os_log strings:
 * "dst_port->getMaxInHopID", "dst_range.start > dst_range.end", "allocated
 * input hop id %d for dst port %p ... status = 0x%x"). Binding our own NHI port
 * as the destination asks it to allocate an input hop id for a peer that does
 * not live there, which is why that form returns kIOReturnNotFound.
 *
 * Returns null on failure (the path object is unusable after a failed
 * allocate(), so the caller must make a fresh one per attempt). */
static IOThunderboltPath *allocPath(TbAccelClient *me, IOThunderboltController *ctrl,
                                    IOThunderboltPort *srcPort, IOThunderboltPort *dstPort,
                                    uint16_t srcHop, uint16_t dstHop,
                                    const char *mode, const char *tag)
{
    IOThunderboltPath *p = IOThunderboltPath::withController(ctrl);
    if (!p) {
        TBLOG2("makePath(%s): withController failed\n", tag);
        return nullptr;
    }
    /* AppleThunderboltIPTransmitter::createTxPath calls setInitialCredits(w)
     * + setSourceInitialCredits(w - 2); AppleThunderboltIPConnection::createRxPath
     * calls setInitialCredits(w) + setDestinationInitialCredits(w - 2). */
    p->setInitialCredits(TBACCEL_WINDOW);
    if (tag[0] == 't') {
        p->setSourceInitialCredits(TBACCEL_WINDOW - 2);
    } else {
        p->setDestinationInitialCredits(TBACCEL_WINDOW - 2);

        /* The rest of AppleThunderboltIPConnection::createRxPath's attribute
         * block (0x5dc94..0x5df44), in the same order and with the same
         * values. The important one is setCounterEnable(): the family's own
         * control path shows "Counter = 0" in its hop-table entry while ours
         * showed "Counter = None", i.e. the hop had no credit counter
         * allocated, and an ingress hop without a counter is the one thing our
         * receive path had that the working control path does not. */
        p->setPriority(2);
        p->setWeight(2);
        p->setCounterEnable(7);
        p->setDropPacketEnable(0);
        p->setIngressFlowControlEnable(7);
        p->setEgressFlowControlEnable(3);          /* TX uses 7 */
        p->setIngressSharedBufferingEnable(0);
        p->setEgressSharedBufferingEnable(0);
        p->setNonFlowControlledCredits(0);
        p->setFlags(0);
        p->setRoutingOptions(0x11);
        me->setProperty("TbAccelRxPathCfg", "ip-rx");
    }

    if (srcPort)
        p->setSourcePort(srcPort);
    if (dstPort)
        p->setDestinationPort(dstPort);

    IOThunderboltHopIDRange s = { srcHop, srcHop };
    IOThunderboltHopIDRange d = { dstHop, dstHop };
    p->setSourceHopIDRange(s);
    p->setDestinationHopIDRange(d);

    uint32_t prc = p->allocate();
    char v[96];
    snprintf(v, sizeof v, "%s src%u dst%u %s rc=0x%08x", tag,
             (unsigned)srcHop, (unsigned)dstHop, mode, (unsigned)prc);
    me->setProperty("TbAccelPathLastTry", v);
    me->setProperty(tag[0] == 't' ? "TbAccelTxPathRc" : "TbAccelRxPathRc", v);
    if (sTryLogLen < sizeof sTryLog) {
        int n = snprintf(sTryLog + sTryLogLen, sizeof sTryLog - sTryLogLen,
                         "%s%s%u/%u%s:0x%08x", sTryLogLen ? " " : "", tag,
                         (unsigned)srcHop, (unsigned)dstHop, mode, (unsigned)prc);
        if (n > 0)
            sTryLogLen += (size_t)n < sizeof sTryLog - sTryLogLen
                        ? (size_t)n : sizeof sTryLog - sTryLogLen;
        me->setProperty("TbAccelPathTries", sTryLog);
    }
    if (prc != 0) {
        TBLOG2("makePath(%s): allocate failed src=%u dst=%u %s rc=0x%08x\n",
               tag, (unsigned)srcHop, (unsigned)dstHop, mode, (unsigned)prc);
        p->release();
        return nullptr;
    }
    return p;
}

/* The peer's port: our XDomain service is a child of the peer's
 * IOThunderboltXDomainLink, which is itself a child of the IOThunderboltPort on
 * the remote switch. Walking up with IORegistryEntry::getParentEntry() uses only
 * base-class virtuals, so it is safe for objects we have no class definition
 * for (and needs no assumption about their exact superclass). */
static IOThunderboltPort *findPeerPort(IOThunderboltXDomainService *svc)
{
    if (!svc)
        return nullptr;
    IORegistryEntry *link = svc->getParentEntry(gIOServicePlane);
    if (!link)
        return nullptr;
    IORegistryEntry *port = link->getParentEntry(gIOServicePlane);
    if (!port)
        return nullptr;
    return OSDynamicCast(IOThunderboltPort, port);
}

/* Remember which (src,dst) pair worked, per direction, so the second attempt
 * in that direction does not re-probe - every failed attempt costs the peer
 * hop ids, and its table is tiny. */
static inline unsigned pairIdx(const char *tag) { return tag[0] == 't' ? 0u : 1u; }
static bool     sHopPairKnown[2];
static uint16_t sSrcHop[2], sDstHop[2];
/* -1 unknown, else index into the port-mode table below. */
static int      sPathPorts[2] = { -1, -1 };
/* The peer's port - a different port from ours, which is the whole point:
 * allocate() takes a hop id from the source range on the source port AND from
 * the destination range on the destination port, so using one port for both
 * makes the second allocation collide with the first and return
 * kIOReturnNotFound regardless of the hop value. */
static IOThunderboltPort *sPeerPort;
/* Our port facing the peer (where inbound frames enter). */
static IOThunderboltPort *sDownPort;
/* Set once a direction has failed every candidate pair: further attempts only
 * burn the peer's (16 entry) hop table. Cleared when the handshake is (re)done,
 * because a pre-handshake failure says nothing about the post-handshake one. */
static bool     sPathGaveUp[2];

/* Create+allocate+activate a path for our transmit hop @ringHop, sending to the
 * peer's receive hop @peerHop (RDMA's allocateAndActivatePath sequence). */
static IOThunderboltPath *makePath(TbAccelClient *me, IOThunderboltController *ctrl,
                                   uint16_t ringHop, uint16_t peerHop, const char *tag)
{
    unsigned idx = pairIdx(tag);
    IOThunderboltPath *p = nullptr;

    if (sUseXdc || sPathGaveUp[idx])
        return nullptr;

    struct pair { uint16_t s, d; };
    pair pairs[4];
    unsigned np = 0;

    if (sHopPairKnown[idx]) {
        pairs[np].s = sSrcHop[idx];
        pairs[np].d = sDstHop[idx];
        np = 1;
    } else {
        pair cand[] = {
            /* The NHI transmits on its *ring* hop, so the path's source hop
             * should be the ring hop: that installs "ring hop -> peer hop" in
             * the host hop table, which is the mapping frames need. */
            { ringHop, (uint16_t)(peerHop ? peerHop : TBACCEL_PATH_HOP) },
            /* Our advertised out hop -> the peer's published in hop. */
            { TBACCEL_PATH_HOP, (uint16_t)(peerHop ? peerHop : TBACCEL_PATH_HOP) },
            /* Same, but assuming the peer mirrored symmetrically. */
            { TBACCEL_PATH_HOP, TBACCEL_PATH_HOP },
            /* Last resort: the NHI ring hop on both sides. */
            { ringHop, ringHop },
        };
        for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
            bool dup = false;
            for (unsigned j = 0; j < np; j++)
                if (pairs[j].s == cand[i].s && pairs[j].d == cand[i].d)
                    dup = true;
            if (!dup)
                pairs[np++] = cand[i];
        }
    }

    /* Port pair. "lp" = our NHI -> the peer's port: two *different* ports, so
     * the two hop-id allocations cannot collide. "nhi" keeps the old
     * single-port form as a fallback. */
    IOThunderboltPort *nhi = OSDynamicCast(IOThunderboltPort, ctrl->getNHIPort());
    bool havePeer = (sPeerPort && sPeerPort != nhi);
    struct pmode { IOThunderboltPort *s, *d; const char *n; };
    pmode portModes[] = {
        { nhi, havePeer ? sPeerPort : nhi, havePeer ? "lp" : "nhi" },
        { nhi, nhi,                                                  "nhi" },
    };
    const unsigned nmodes = havePeer ? 2u : 1u;

    unsigned first = sPathPorts[idx] >= 0 && (unsigned)sPathPorts[idx] < nmodes
                   ? (unsigned)sPathPorts[idx] : 0u;
    unsigned count = sPathPorts[idx] >= 0 && (unsigned)sPathPorts[idx] < nmodes
                   ? 1u : nmodes;

    for (unsigned i = 0; i < np && !p; i++) {
        for (unsigned m = 0; m < count && !p; m++) {
            unsigned mi = (first + m) % nmodes;
            p = allocPath(me, ctrl, portModes[mi].s, portModes[mi].d,
                          pairs[i].s, pairs[i].d, portModes[mi].n, tag);
            if (p) {
                sSrcHop[idx] = pairs[i].s;
                sDstHop[idx] = pairs[i].d;
                sHopPairKnown[idx] = true;
                sPathPorts[idx] = (int)mi;
                me->setProperty(tag[0] == 't' ? "TbAccelTxPathMode" : "TbAccelRxPathMode",
                                portModes[mi].n);
            }
        }
    }

    if (!p) {
        sPathGaveUp[idx] = true;
        me->setProperty("TbAccelPathFail", tag);
        return nullptr;
    }
    me->setProperty("TbAccelPathFail", "none");
    me->setProperty(tag[0] == 't' ? "TbAccelTxPathAlloc" : "TbAccelRxPathAlloc", true);
    me->setProperty(tag[0] == 't' ? "TbAccelTxPathHop" : "TbAccelRxPathHop",
                    (unsigned long long)p->getAllocatedSourceHopID());
    /* IP activates both of its paths with activateSynchronous() (vt+0x3d8):
     * AppleThunderboltIPTransmitter around 0x44718 and
     * AppleThunderboltIPConnection::login at 0x65d00. activate() only kicks off
     * the non-flow-controlled part (startNFCActivation) and returns before the
     * per-hop activation writes run, so it cannot report whether the hop table
     * entry was actually programmed. */
    {
        IOReturn arc = p->activateSynchronous();
        char v[32];
        snprintf(v, sizeof v, "0x%08x", (unsigned)arc);
        me->setProperty(tag[0] == 't' ? "TbAccelTxPathActRc" : "TbAccelRxPathActRc", v);
    }
    me->setProperty(tag[0] == 't' ? "TbAccelTxPathAct" : "TbAccelRxPathAct", true);
    return p;
}

/* Our own port facing the peer. The chain in the registry is:
 *
 *   IOThunderboltXDomainService
 *     -> IOThunderboltXDomainLink        (peer's link)
 *        -> IOThunderboltPort            (peer's port)
 *           -> IOThunderboltSwitchType5  (peer's switch)
 *              -> IOThunderboltPort       (OUR port facing the peer)  <-
 *
 * so four parent hops, all through IORegistryEntry's own virtual. The trail is
 * published so a mis-shaped chain shows up as characters rather than a silent
 * null: L/P/S/P means each cast matched. */
static IOThunderboltPort *findDownstreamPort(TbAccelClient *me,
                                             IOThunderboltXDomainService *svc)
{
    if (!svc)
        return nullptr;

    IORegistryEntry *e = svc->getParentEntry(gIOServicePlane);   /* link */
    IOThunderboltPort *down = nullptr;
    char trail[8];
    size_t n = 0;
    trail[0] = 0;

    for (unsigned i = 0; i < 4 && e; i++) {
        char c = '?';
        switch (i) {
        case 0: c = OSDynamicCast(IOThunderboltXDomainLink, e) ? 'L' : 'l'; break;
        case 1: c = OSDynamicCast(IOThunderboltPort, e) ? 'P' : 'p'; break;
        case 2: c = OSDynamicCast(IOThunderboltSwitch, e) ? 'S' : 's'; break;
        case 3:
            down = OSDynamicCast(IOThunderboltPort, e);
            c = down ? 'P' : 'p';
            break;
        }
        if (n + 1 < sizeof trail) {
            trail[n++] = c;
            trail[n] = 0;
        }
        if (i == 3)
            break;
        e = e->getParentEntry(gIOServicePlane);
    }

    me->setProperty("TbAccelPortChain", trail);
    return down;
}

/* The receive half: our downstream port -> our NHI. Source hop is the hop the
 * peer labels frames with (9); destination is the receive ring's hop, because
 * inbound delivery on the Mac is to a ring hop. */
static IOThunderboltPath *makeRxPath(TbAccelClient *me, IOThunderboltController *ctrl,
                                     IOThunderboltPort *inPort, uint16_t ringHop,
                                     uint16_t wireHop, const char *tag)
{
    IOThunderboltPort *nhi = OSDynamicCast(IOThunderboltPort, ctrl->getNHIPort());
    if (!inPort || !nhi || inPort == nhi)
        return nullptr;

    uint16_t srcs[3] = { wireHop, 10, 11 };
    for (unsigned i = 0; i < 3; i++) {
        if (!srcs[i])
            continue;
        if (i && srcs[i] == srcs[0])
            continue;
        IOThunderboltPath *p = allocPath(me, ctrl, inPort, nhi, srcs[i], ringHop,
                                         "rlp", tag);
        if (p) {
            /* This path is only allocated by allocPath() - unlike the transmit
             * path, nothing else activates it. IP does exactly this in
             * AppleThunderboltIPConnection::login: configureRxPath() then
             * fRxPath->activateSynchronous() (vt+0x3d8). Without it the hop
             * entry keeps its counter but loses DstHop/DstPort, because
             * IOThunderboltPath::getHopInfo() refuses to report an entry whose
             * path state is not 5 (activated). */
            IOReturn arc = p->activateSynchronous();
            char v[32];
            snprintf(v, sizeof v, "0x%08x", (unsigned)arc);
            me->setProperty("TbAccelRxPathActRc", v);
            me->setProperty("TbAccelRxPathAct", true);
            me->setProperty("TbAccelRxPathSrcHop",
                            (unsigned long long)p->getAllocatedSourceHopID());
            me->setProperty("TbAccelRxPathDstHop",
                            (unsigned long long)p->getAllocatedDestinationHopID());
            me->setProperty("TbAccelRxPathState", (unsigned long long)p->getState());
            return p;
        }
    }
    return nullptr;
}

/* Read-only probe of the hop/path table quadlets. The peer's own debugfs dump
 * (debugfs/thunderbolt/<domain>/<switch>/port<N>/path) reports 8 bytes per hop
 * starting at config offset 0x10, e.g. hop 9 -> 0x801c0802. If we can read the
 * same values from here, the register path is addressable from a kext and the
 * write step becomes mechanical. */
static void dumpPathRegs(TbAccelClient *me, IOThunderboltPort *port, const char *tag)
{
    if (!port) {
        me->setProperty(tag, "no-port");
        return;
    }
    static const unsigned char offs[] = { 0x10, 0x11, 0x12, 0x13, 0x00, 0x01 };
    char buf[200];
    size_t n = 0;
    buf[0] = 0;
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        unsigned int v = 0;
        uint32_t rc = port->cmReadDWord(offs[i], &v);
        int w = snprintf(buf + n, sizeof buf - n, "%s%02x=%08x/rc%x",
                         i ? " " : "", offs[i], v, (unsigned)rc);
        if (w <= 0 || (size_t)w >= sizeof buf - n)
            break;
        n += (size_t)w;
    }
    me->setProperty(tag, buf);
}

/* Diagnostic: log the raw IOReturn for a spread of (queueType, flags) pairs.
 * Decoded from AppleThunderboltNHITransmitRingManager::allocateTransmitRing:
 *   0xe00002be  -> no ring in getEnabledTxRingMask() (mask = ~0xff, so bits 0-7
 *                  are never allocatable and the ring count must exceed 8)
 *   anything else -> a ring was picked but ring->configure() failed with it
 * A successful queue is returned so we never free an allocated ring. */
static IOThunderboltTransmitQueue *tbProbeTxQueue(TbAccelClient *me, IOThunderboltController *ctrl)
{
    static const struct { uint16_t type; uint32_t flags; } combos[] = {
        { 0, 4 }, { 2, 4 }, { 3, 4 }, { 0, 0 }, { 0, 1 }, { 2, 1 }, { 3, 1 },
        { 0, 2 }, { 0, 8 }, { 0, 0x10 }, { 1, 4 }, { 0, 0x100 },
    };
    IOThunderboltTransmitQueue *hit = nullptr;
    for (unsigned i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
        IOThunderboltTransmitQueue *q = IOThunderboltTransmitQueue::withController(ctrl);
        if (!q) { TBLOG2("probe%u: withController null\n", i); continue; }
        q->setFlags(combos[i].flags);
        q->setQueueType(combos[i].type);
        q->setRingSize(TBACCEL_NHI_RING_SIZE);
        q->setMaxFrameSize(TB_MAX_FRAME_SIZE);
        q->enableE2EFlowControl(false);
        uint32_t rc = q->allocate();
        uint16_t hop = 0;
        if (rc == 0) q->getHopID(&hop);
        TBLOG2("probe%u type=%u flags=0x%x ring=%u -> 0x%08x hop=%u\n",
               i, (unsigned)combos[i].type, (unsigned)combos[i].flags,
               (unsigned)TBACCEL_NHI_RING_SIZE, (unsigned)rc, (unsigned)hop);
        /* ioreg is the channel that actually survives; os_log from kexts is
         * unreliable here. */
        char key[32];
        char val[80];
        snprintf(key, sizeof key, "TbAccelProbe%u", i);
        snprintf(val, sizeof val, "t%u f0x%x r%u -> 0x%08x hop%u",
                 (unsigned)combos[i].type, (unsigned)combos[i].flags,
                 (unsigned)TBACCEL_NHI_RING_SIZE, (unsigned)rc, (unsigned)hop);
        me->setProperty(key, val);
        if (rc == 0 && !hit) { hit = q; continue; }
        q->release();
    }
    me->setProperty("TbAccelProbeHit", hit ? kOSBooleanTrue : kOSBooleanFalse);
    return hit;
}

static IOThunderboltTransmitQueue *newTxQueue(TbAccelClient *me, IOThunderboltController *ctrl,
                                               IOWorkLoop *wl, const char *name,
                                               IOThunderboltTransmitQueue *pre = nullptr)
{
    (void)wl;
    /* Exact setter sequence decoded from AppleThunderboltIPTransmitter::createTxPath:
     * setFlags(4), setQueueType(2), setRingSize(n), setMaxFrameSize(4096),
     * enableE2EFlowControl(e2e), allocate(), start(). */
    IOThunderboltTransmitQueue *q = pre;
    if (!q) {
    q = IOThunderboltTransmitQueue::withController(ctrl);
    if (!q) { me->setProperty("TxCreateFail", kOSBooleanTrue); return nullptr; }
    q->setFlags(4);          /* IP's createTxPath: flags=4, type=2 */
    q->setQueueType(2);
    q->setRingSize(TBACCEL_NHI_RING_SIZE);
    q->setMaxFrameSize(TB_MAX_FRAME_SIZE);
    q->enableE2EFlowControl(false);   /* peer has no E2E counters */
    {
        uint32_t rc = q->allocate();
        me->setProperty("TxAllocRc", (unsigned long long)rc);
        if (rc != 0) {
            TBLOG2("tx allocate -> 0x%08x\n", (unsigned)rc);
            me->setProperty("TxAlloc", kOSBooleanFalse);
            q->release();
            return nullptr;
        }
    }
    }
    me->setProperty("TxAlloc", kOSBooleanTrue);
    uint16_t hop = 0;
    q->getHopID(&hop);
    {
        char v[32];
        snprintf(v, sizeof v, "hop%u", (unsigned)hop);
        me->setProperty("TbAccelTxHop", v);
    }
    /* The path is created later, once the peer's hop ids are known. */
    {
        uint32_t src = q->start();
        char v[32];
        snprintf(v, sizeof v, "0x%08x", (unsigned)src);
        me->setProperty("TxStartRc", v);
        if (src != 0) {
            TBLOG2("tx start -> 0x%08x\n", (unsigned)src);
            me->setProperty("TxStart", kOSBooleanFalse);
            q->release();
            return nullptr;
        }
    }
    me->setProperty("TxStart", kOSBooleanTrue);
    return q;
}

static IOThunderboltReceiveQueue *newRxQueue(TbAccelClient *me, IOThunderboltController *ctrl,
                                             IOWorkLoop *wl, const char *name, uint16_t e2eHop)
{
    /* AppleThunderboltIPConnection::createRxPath uses
     * withControllerAndWorkLoop for its receive queue - a receive ring has to be
     * serviced by the NHI interrupt handler, which is what the work loop is
     * for. We were creating it with withController alone. */
    IOThunderboltReceiveQueue *q =
        IOThunderboltReceiveQueue::withControllerAndWorkLoop(ctrl, wl);
    if (!q) { me->setProperty("RxCreateFail", kOSBooleanTrue); return nullptr; }
    /* AppleThunderboltIPConnection::createRxPath uses flags 0 for the receive
     * queue (its transmit queue uses 4). We had copied the transmit value onto
     * both queues; with flags=4 small control frames are delivered but every
     * data frame comes back kIOReturnIOError with no payload written. */
    q->setFlags(0);
    q->setQueueType(2);
    /* IP asks its receive queue for tens of entries, we asked for 4096. The ring
     * manager builds a per-hop mapper sized from this, and with a huge ring every
     * frame above ~256 bytes comes back as (size - 256) + 252 with
     * kIOReturnIOError and no payload - as if the mapper granule were
     * TB_FRAME_SIZE. Try an IP-sized ring first and fall back, so a mapper that
     * refuses a small ring cannot break bring-up. */
    q->setMaxFrameSize(TB_MAX_FRAME_SIZE);
    q->setSOFPDFBitmask((uint16_t)(1u << TBACCEL_PDF_SOF));
    q->setEOFPDFBitmask((uint16_t)(1u << TBACCEL_PDF_EOF));
    /* End-to-end (hop counter) flow control, matching
     * AppleThunderboltIPConnection::createRxPath: it sets the hop that credit
     * returns go to - the transmit path's hop, which is why net->tx_ring->hop is
     * what Linux's tb_ring_alloc_rx() is given - and then enables E2E.
     *
     * This is not optional when the peer is E2E: with the receive queue out of
     * the accounting, frames arriving on the counter-enabled hop are dropped by
     * the NHI before any completion is generated (RxStat stays all0 entirely).
     * Non-E2E peers work either way, as the reverse direction shows. */
    /* IP disables raw mode explicitly (vt+0x1d0). Leaving it at the default
     * puts the ring in raw mode, where the NHI delivers 256-byte pieces
     * (TB_FRAME_SIZE) and drops 4 bytes at each split: a 512-byte frame arrives
     * as 256+252 with kIOReturnIOError and no payload, and only frames <= 64
     * bytes (which fit one piece whole) were ever received intact. */
    q->enableRawMode(false);
    q->setTxE2EHopID(e2eHop);
    q->enableE2EFlowControl(e2eHop != 0);
    uint32_t rxRingUsed = 0;
    {
        static const uint32_t ringSizes[] = { 64, 256, TBACCEL_NHI_RING_SIZE };
        uint32_t rc = kIOReturnError;
        uint32_t used = 0;

        for (unsigned i = 0; i < sizeof(ringSizes) / sizeof(ringSizes[0]); i++) {
            q->setRingSize(ringSizes[i]);
            rc = q->allocate();
            if (rc == 0) { used = ringSizes[i]; break; }
        }
        rxRingUsed = used;
        me->setProperty("RxRingSize", (unsigned long long)used);
        me->setProperty("RxAllocRc", (unsigned long long)rc);
        if (rc != 0) {
            TBLOG2("rx allocate -> 0x%08x\n", (unsigned)rc);
            me->setProperty("RxAlloc", kOSBooleanFalse);
            q->release();
            return nullptr;
        }
    }
    me->setProperty("RxAlloc", kOSBooleanTrue);
    uint16_t hop = 0;
    q->getHopID(&hop);
    {
        char v[32];
        snprintf(v, sizeof v, "hop%u", (unsigned)hop);
        me->setProperty("TbAccelRxHop", v);
    }
    /* The path is created later, once the peer's hop ids are known. */
    {
        uint32_t src = q->start();
        char v[32];
        snprintf(v, sizeof v, "0x%08x", (unsigned)src);
        me->setProperty("RxStartRc", v);
        if (src != 0) {
            TBLOG2("rx start -> 0x%08x\n", (unsigned)src);
            me->setProperty("RxStart", kOSBooleanFalse);
            q->release();
            return nullptr;
        }
    }
    me->setProperty("RxStart", kOSBooleanTrue);
    {
        char v[128];
        snprintf(v, sizeof v, "flags0 type2 ring%u max%u sof%u eof%u e2e1 hop%u",
                 (unsigned)rxRingUsed, (unsigned)TB_MAX_FRAME_SIZE,
                 (unsigned)(1u << TBACCEL_PDF_SOF), (unsigned)(1u << TBACCEL_PDF_EOF),
                 (unsigned)e2eHop);
        me->setProperty("TbAccelRxCfg", v);
    }
    return q;
}

bool TbAccelClient::setupQueues()
{
    /* The probe allocated one ring per (type,flags) combination just to learn
     * the ring-manager contract; those ring hops are a shared resource, so it
     * runs only when explicitly asked for. */
    IOThunderboltTransmitQueue *pre = nullptr;
    fTxQ = newTxQueue(this, fController, fWorkLoop, "tx", pre);
    if (!fTxQ)
        return false;

    uint16_t txHop = 0;
    fTxQ->getHopID(&txHop);
    TBLOG2("tx queue hop=%u\n", (unsigned)txHop);

    /* The family-built receive queue route (tryXDomainRx) PANICS: constructing
     * an IOThunderboltXDomainPath ourselves and activating it makes the family
     * branch through a vtable slot that does not authenticate (PAC failure at
     * family pc 0x...3afa4, x17=0x470, x8=0xaba5...). See docs 27.0.14.
     * Disabled: our own ring is the only safe receive path for now. */
    fRxQ = newRxQueue(this, fController, fWorkLoop, "rx", txHop);
    if (!fRxQ)
        return false;

    uint16_t rxHop = 0;
    fRxQ->getHopID(&rxHop);
    {
        char v[32];
        snprintf(v, sizeof v, "hop%u", (unsigned)rxHop);
        setProperty("TbAccelRxHop", v);
    }
    TBLOG2("queues tx=%p rx=%p\n", fTxQ, fRxQ);
    return true;
}

/*
 * Family-built receive queue.
 *
 * allocateXDomainQueue() in the family does exactly this: create a queue for a
 * path id and register it, and allocateRxQueueInternal() looks the path up by
 * id and derives the queue's hop from it. That association is what we cannot
 * express with a bare IOThunderboltReceiveQueue, and it is why inbound frames
 * never had a hop to land on.
 *
 * Markers are published at each step so a failure is attributable without
 * guessing: TbAccelXdpCreate / XdpAlloc / XdpAct / XdpReg / XdpRx.
 */
bool TbAccelClient::tryXDomainRx()
{
    static const unsigned int kPathID = 1;

    if (!fXdc || !fLink) {
        setProperty("TbAccelXdpCreate", "no-client");
        return false;
    }

    /* withXDomainLink() is a stub that logs and returns NULL - it never
     * allocates. Construct the object the way the family's own
     * allocateXDomainPath() does, then initialise it from the link. */
    fXdp = OSTypeAlloc(IOThunderboltXDomainPath);
    if (!fXdp) {
        setProperty("TbAccelXdpCreate", "alloc-fail");
        return false;
    }
    if (!fXdp->initWithXDomainLink(fLink)) {
        setProperty("TbAccelXdpCreate", "init-fail");
        fXdp->release();
        fXdp = nullptr;
        return false;
    }
    setProperty("TbAccelXdpCreate", "ok");

    fXdp->setServicePathID(kPathID);

    uint32_t rc = fXdp->allocate();
    if (rc != 0) {
        char v[32];
        snprintf(v, sizeof v, "0x%08x", (unsigned)rc);
        setProperty("TbAccelXdpAlloc", v);
        return false;
    }
    setProperty("TbAccelXdpAlloc", "ok");

    fXdp->activate();
    setProperty("TbAccelXdpAct", "ok");

    fXdc->registerXDomainPath(fXdp, kPathID);
    setProperty("TbAccelXdpReg", "ok");

    IOThunderboltReceiveQueue *q =
        fXdc->allocateRxQueueInternal(kPathID, TB_MAX_FRAME_SIZE, TBACCEL_PDF);
    setProperty("TbAccelXdpRx", q ? "ok" : "null");
    if (!q)
        return false;

    fRxQ = q;
    uint16_t hop = 0;
    q->getHopID(&hop);
    {
        char v[32];
        snprintf(v, sizeof v, "hop%u", (unsigned)hop);
        setProperty("TbAccelXdpRxHop", v);
    }
    return true;
}

/* Read the hop ids the peer published on its `accel` XDomain service. This is
 * our analogue of Thunderbolt-net's login exchange: it is the handshake that
 * says the peer's side exists, and therefore the first moment a path may
 * legally be created. Returns true when at least the peer's receive hop (the
 * one we transmit to) is known. */
bool TbAccelClient::readPeerHops(uint16_t *inHop, uint16_t *outHop)
{
    uint32_t in = 0xffff, out = 0xffff;

    if (fService) {
        OSDictionary *props = fService->getProperties();
        if (props) {
            /* The family stores XDomain properties with a " (Value)" suffix -
             * the real key is literally "inhopid (Value)". Looking up
             * "inhopid" therefore always missed, which is why the peer's own
             * advertisement showed up in ioreg but never here. Accept both
             * spellings so a change either way keeps working. */
            static const char *inKeys[]  = { "inhopid (Value)",  "inhopid"  };
            static const char *outKeys[] = { "outhopid (Value)", "outhopid" };
            for (unsigned i = 0; i < 2 && in == 0xffff; i++) {
                OSNumber *n = OSDynamicCast(OSNumber, props->getObject(inKeys[i]));
                if (n) in = n->unsigned32BitValue();
            }
            for (unsigned i = 0; i < 2 && out == 0xffff; i++) {
                OSNumber *n = OSDynamicCast(OSNumber, props->getObject(outKeys[i]));
                if (n) out = n->unsigned32BitValue();
            }
        }
    }

    if (inHop)  *inHop  = (uint16_t)in;
    if (outHop) *outHop = (uint16_t)out;

    {
        char v[48];
        snprintf(v, sizeof v, "peer in%u out%u", (unsigned)in, (unsigned)out);
        setProperty("TbAccelPeerHops", v);
    }
    return in != 0xffff && in != 0;
}

/* Path phase. Runs after the rings are up and stays resident: it polls for the
 * peer's handshake (its inhopid/outhopid) and makes at most ONE bounded attempt
 * per handshake, so a failure cannot burn the peer's 16-entry hop table but a
 * peer that loads later - or reloads - is still picked up. */
bool TbAccelClient::tryPaths()
{
    if (fPathsDone)
        return true;

    uint16_t in = 0, out = 0;
    bool known = readPeerHops(&in, &out);

    /* A handshake is: the peer's hop ids appearing, or changing. Both mean the
     * peer's side has (re)initialised, so a previous verdict is stale. */
    bool handshake = false;
    if (known && (!fPeerHopsKnown || in != fPeerIn || out != fPeerOut)) {
        fPeerHopsKnown = true;
        fPeerIn  = in;
        fPeerOut = out;
        fPathPolls = 0;
        fPathAttempted = false;
        /* Any failure recorded before the peer existed is meaningless now. */
        sHopPairKnown[0] = sHopPairKnown[1] = false;
        sPathGaveUp[0]   = sPathGaveUp[1]   = false;
        sPathPorts[0]    = sPathPorts[1]    = -1;
        setProperty("TbAccelPathHandshake", kOSBooleanTrue);
        handshake = true;
        TBLOG2("handshake: peer in=%u out=%u\n", (unsigned)in, (unsigned)out);
    } else {
        fPeerIn  = in;
        fPeerOut = out;
    }

    if (!known && !fPeerHopsKnown) {
        /* Hedge: the family may not refresh the service property table after
         * start(), so the handshake could never become visible to us even
         * though the peer is ready. Wait 30 s, then make one bounded attempt
         * with the agreed default instead of deadlocking. */
        if (++fPathPolls < 60) {
            setProperty("TbAccelPathPhase", "waiting-peer");
            armRetry(500);
            return false;
        }
        setProperty("TbAccelPathNoHandshake", kOSBooleanTrue);
        fPeerIn  = fPeerOut = TBACCEL_PATH_HOP;
    }

    if (!fTxQ || !fRxQ) {
        setProperty("TbAccelPathPhase", "no-rings");
        return false;
    }

    if (!sPeerPort) {
        sPeerPort = findPeerPort(fService);
        setProperty("TbAccelPeerPort", sPeerPort ? kOSBooleanTrue : kOSBooleanFalse);
        /* Read-only register probe, once per boot, as soon as both ports are
         * known. */
        if (sPeerPort) {
            sDownPort = findDownstreamPort(this, fService);
            setProperty("TbAccelDownPort", sDownPort ? kOSBooleanTrue : kOSBooleanFalse);
            /* The NHI port has no command queue (rc 0xe00002f0) and the remote
             * port answers Unsupported (0xe00002c7), so cmReadDWord cannot
             * reach the hop tables; keep one read on our own downstream port as
             * the remaining candidate. Probe the lane-adapter port as well: it
             * is the port whose path table actually decides whether inbound
             * frames tagged with the peer's hop reach our RX ring, so if any
             * port can be read back it is this one. */
            dumpPathRegs(this, sDownPort, "TbAccelDownPathRegs");
            if (sPeerPort && sPeerPort != sDownPort)
                dumpPathRegs(this, sPeerPort, "TbAccelLanePathRegs");
        }
    }

    if (fPathAttempted) {
        /* Nothing new to try. Stay resident and watch for a handshake: the
         * fallback attempt can easily run before the peer has loaded. */
        setProperty("TbAccelPathPhase",
                    sPathGaveUp[0] ? "tx-exhausted" : "waiting-peer");
        armRetry(2000);
        return false;
    }
    fPathAttempted = true;

    uint16_t txRingHop = 0, rxRingHop = 0;
    fTxQ->getHopID(&txRingHop);
    fRxQ->getHopID(&rxRingHop);

    /* Transmit: our out hop -> the peer's in hop (what it listens on). */
    if (!fTxPath)
        fTxPath = makePath(this, fController, txRingHop, fPeerIn, "tx");
    if (!fTxPath) {
        /* Exhausted or not, stay resident: a later handshake resets the verdict
         * and allows exactly one more attempt. We never re-probe without one. */
        setProperty("TbAccelPathPhase", sPathGaveUp[0] ? "tx-exhausted" : "tx-failed");
        armRetry(2000);
        return false;
    }

    /* One IOThunderboltPath covers the forward direction; the reverse needs its
     * own port/hop pair (see makeRxPath). A failure here is not fatal for TX. */
    if (!fRxPath) {
        /* Our downstream port turns out to be the NHI itself (port 7), which
         * makeRxPath rejects as "same port twice". Fall back to the peer's port
         * so the reverse path still gets attempted - and now with the *new*
         * ring hop (2) as destination, which no earlier attempt used. */
        IOThunderboltPort *nhiPort =
            OSDynamicCast(IOThunderboltPort, fController->getNHIPort());
        IOThunderboltPort *rxSrc =
            (sDownPort && sDownPort != nhiPort) ? sDownPort : sPeerPort;
        fRxPath = makeRxPath(this, fController, rxSrc, rxRingHop,
                             TBACCEL_PATH_HOP, "rx");
        setProperty("TbAccelRxPathAlloc", fRxPath ? kOSBooleanTrue : kOSBooleanFalse);
    }

    fPathsDone = true;
    setProperty("TbAccelPathPhase", fRxPath ? "up" : "up-tx-only");

    /* Probe again now that the path exists: the bring-up probe was sent before
     * any fabric state was installed and could not have crossed. */
    sendCtl(0x7f, 0, 0, nullptr, 0);
    setProperty("TbAccelTxProbe", "after-path");

    TBLOG2("path up (src%u dst%u, mode %s)\n",
           (unsigned)sSrcHop[0], (unsigned)sDstHop[0],
           sPathPorts[0] == 0 ? "lp" : "nhi");
    return true;
}

void TbAccelClient::teardown()
{
    fRxStreamSlot = nullptr;
    fRxStreamOff = 0;
    /* Disable the paths before stopping the rings, mirroring Thunderbolt-net's
     * teardown: a stopped ring zeroes its descriptor base and frees the frame
     * buffers, and the peer-side hop's 'pending' bit can never clear in that
     * state. */
    if (fXdp) { fXdp->deactivateSynchronous(); fXdp->release(); fXdp = nullptr; }
    if (fTxPath) { fTxPath->deactivateSynchronous(); fTxPath->release(); fTxPath = nullptr; }
    if (fRxPath) { fRxPath->deactivateSynchronous(); fRxPath->release(); fRxPath = nullptr; }
    if (fTxQ) { fTxQ->stop(); fTxQ->shutdown(); fTxQ->release(); fTxQ = nullptr; }
    if (fRxQ) { fRxQ->stop(); fRxQ->shutdown(); fRxQ->release(); fRxQ = nullptr; }
    fPathsDone = false;
}

/* --------------------------------------------------------- control rx path */

/* Pre-post control receive frames; each lands in its own 4K page. */
bool TbAccelClient::startControlRx()
{
    uint32_t ok = 0, fail = 0, allocFail = 0;
    TBLOG2("startControlRx: posting %u control RX commands\n", (unsigned)TBACCEL_RING_SIZE);
    for (uint32_t i = 0; i < TBACCEL_RING_SIZE; i++) {
        IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
            kernel_task, kIODirectionIn, TB_MAX_FRAME_SIZE, PAGE_SIZE);
        if (!md) {
            TBLOG2("startControlRx: md alloc failed at %u\n", i);
            allocFail++;
            break;
        }
        IOThunderboltReceiveCommand *cmd =
            OSTypeAlloc(IOThunderboltReceiveCommand);
        if (!cmd || !cmd->initWithControllerAndQueue(fController, fRxQ)) {
            TBLOG2("startControlRx: command alloc/init failed at %u\n", i);
            allocFail++;
            md->release();
            if (cmd) cmd->release();
            break;
        }
        TbAccelCtrlRx *rx = OSTypeAlloc(TbAccelCtrlRx);
        rx->md  = md;
        rx->cmd = cmd;
        fCtlRxPool->setObject(rx);
        rx->release();

        cmd->setMemoryDescriptor(md);
        cmd->setOffset(0);
        cmd->setLength(TB_MAX_FRAME_SIZE);
        cmd->setSOF(TBACCEL_PDF_SOF);
        cmd->setEOF(TBACCEL_PDF_EOF);
        cmd->setCompletion({this, &TbAccelClient::ctlRxComplete, rx});
        cmd->setRefcon(rx);
        /* submit() reports whether the command actually made it onto the NHI
         * ring; a silent failure here would leave the ring with no buffers and
         * look exactly like "nothing ever arrives". */
        {
            uint32_t src = fRxQ->submit(cmd);
            if (src == 0) ok++; else { fail++; }
        }
    }
    {
        char v[64];
        snprintf(v, sizeof v, "ok%u fail%u allocfail%u started%d",
                 (unsigned)ok, (unsigned)fail, (unsigned)allocFail,
                 fRxQ->isStarted() ? 1 : 0);
        setProperty("TbAccelRxPost", v);
    }
    return ok > 0;
}

/* Rolling record of the last completions, so a bad transfer is attributable
 * from ioreg without another build: 'd'=data, 'c'=control, '?'=rejected, then
 * the received length, then '!' when the status was not success. */
void TbAccelClient::rxTrace(uint32_t armed, uint32_t armedOff, uint32_t got,
                            uint32_t status, const uint8_t *data)
{
    for (int i = 3; i > 0; i--)
        fRxEv[i] = fRxEv[i - 1];
    fRxEv[0].armed = armed;
    fRxEv[0].armedOff = armedOff;
    fRxEv[0].got = got;
    fRxEv[0].status = status;
    fRxEv[0].head[0] = fRxEv[0].head[1] = 0;
    if (data) {
        memcpy(&fRxEv[0].head[0], data, 4);
        memcpy(&fRxEv[0].head[1], data + 4, 4);
    }
    fRxEvSeq++;

    for (int i = 0; i < 4; i++) {
        char key[16], val[96];
        snprintf(key, sizeof key, "TbAccelRx%d", i);
        snprintf(val, sizeof val, "armed%u/%u got%u st0x%08x data%08x%08x",
                 (unsigned)fRxEv[i].armed, (unsigned)fRxEv[i].armedOff,
                 (unsigned)fRxEv[i].got, (unsigned)fRxEv[i].status,
                 (unsigned)fRxEv[i].head[0], (unsigned)fRxEv[i].head[1]);
        setProperty(key, val);
    }
}

void TbAccelClient::ctlRxComplete(void *target, void *refcon, IOReturn status,
                                  IOThunderboltReceiveCommand *cmd)
{
    TbAccelClient *me = (TbAccelClient *)target;
    TbAccelCtrlRx *rx = (TbAccelCtrlRx *)refcon;
    if (!me || !rx || !me->fReady) {
        if (cmd) cmd->release();
        return;
    }

    uint64_t got = cmd->getReceivedLength();
    me->fRxAll++;
    if (status == kIOReturnSuccess && got) {
        const uint8_t *p = (const uint8_t *)rx->md->getBytesNoCopy();
        const TbAccelWire *w = (const TbAccelWire *)p;

        /* Control frames are recognised by their header, never by "am I in a
         * stream": the peer sends PREPARE while the previous transfer is still
         * draining, and treating that as payload corrupts both. */
        bool isCtl = got >= sizeof(TbAccelWire) && w->magic == TBACCEL_MAGIC &&
                     w->type >= TBACCEL_MSG_ARM && w->type <= TBACCEL_MSG_CREDIT;

        if (isCtl) {
            me->fRxOk++;
            if ((uint32_t)got > me->fRxCtlMax) me->fRxCtlMax = (uint32_t)got;
            me->fRxLastType = w->type;
            me->fRxLastLen = (uint32_t)got;
            me->rxTrace((uint32_t)cmd->getLength(), (uint32_t)cmd->getOffset(), (uint32_t)got, status, p);
            me->handleCtl(w, p + sizeof(*w), (uint32_t)(got - sizeof(*w)));
        } else if (status == kIOReturnSuccess && me->peerDataChunk(p, (uint32_t)got)) {
            me->fRxDataFrames++;
            me->rxTrace((uint32_t)cmd->getLength(), (uint32_t)cmd->getOffset(), (uint32_t)got, status, p);
        } else {
            me->fRxBad++;
            me->fRxLastLen = (uint32_t)got;
            me->rxTrace((uint32_t)cmd->getLength(), (uint32_t)cmd->getOffset(), (uint32_t)got, status, p);
        }
    } else {
        me->fRxBad++;
        me->rxTrace((uint32_t)cmd->getLength(), (uint32_t)cmd->getOffset(),
                    (uint32_t)got, status, nullptr);
    }
    me->wireStat();

    /* recycle the same command for the next frame */
    if (me->fReady) {
        cmd->setOffset(0);
        cmd->setLength(TB_MAX_FRAME_SIZE);
        cmd->setRefcon(rx);
        cmd->setCompletion({me, &TbAccelClient::ctlRxComplete, rx});
        me->fRxQ->submit(cmd);
    } else {
        cmd->release();
    }
}

IOReturn TbAccelClient::sendCtl(uint16_t type, uint64_t slot, uint32_t len,
                                const void *payload, uint32_t payloadLen)
{
    if (payloadLen > TBACCEL_CTRL_PAYLOAD)
        return kIOReturnBadArgument;

    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionOut, TB_MAX_FRAME_SIZE, PAGE_SIZE);
    if (!md)
        return kIOReturnNoMemory;

    TbAccelWire *w = (TbAccelWire *)md->getBytesNoCopy();
    w->magic = TBACCEL_MAGIC;
    w->type = type;
    w->pad = 0;
    w->slot = slot;
    w->len = len;
    w->payload_len = payloadLen;
    if (payloadLen)
        memcpy((uint8_t *)w + sizeof(*w), payload, payloadLen);
    uint32_t frameLen = sizeof(*w) + payloadLen;

    IOThunderboltTransmitCommand *cmd = OSTypeAlloc(IOThunderboltTransmitCommand);
    if (!cmd || !cmd->initWithControllerAndQueue(fController, fTxQ)) {
        md->release();
        if (cmd) cmd->release();
        return kIOReturnNoMemory;
    }
    cmd->setMemoryDescriptor(md);
    cmd->setOffset(0);
    cmd->setLength(frameLen);
    cmd->setSOF(TBACCEL_PDF_SOF);
    cmd->setEOF(TBACCEL_PDF_EOF);
    cmd->setRefcon(md);
    cmd->setCompletion({this, &TbAccelClient::ctlTxComplete, md});
    if (fTxQ->submit(cmd) != 0) {
        cmd->release();
        md->release();
        return kIOReturnError;
    }
    fTxSub++;
    fTxSubmitted++;
    fTxLastType = type;
    wireStat();
    return kIOReturnSuccess;
}

void TbAccelClient::ctlTxComplete(void *target, void *refcon, IOReturn status,
                                  IOThunderboltTransmitCommand *cmd)
{
    TbAccelClient *me = (TbAccelClient *)target;
    IOBufferMemoryDescriptor *md = (IOBufferMemoryDescriptor *)refcon;
    if (me) {
        me->fTxDone++;
        me->fTxSt = (uint32_t)status;
        me->fTxCompleted++;
        if (me->fGate)
            me->fGate->commandWakeup((void *)&me->fTxCompleted);
        me->wireStat();
    }
    if (md) md->release();
    if (cmd) cmd->release();
}

/* Publish TX/RX progress as strings (os_log from kexts is not captured here,
 * and ioreg renders small OSNumber values ambiguously). */
void TbAccelClient::wireStat()
{
    char v[96];
    snprintf(v, sizeof v, "sub%u done%u st0x%08x last%u",
             (unsigned)fTxSub, (unsigned)fTxDone, (unsigned)fTxSt, (unsigned)fTxLastType);
    setProperty("TbAccelTxStat", v);
    snprintf(v, sizeof v, "all%u ok%u bad%u type%u len%u ctlmax%u data%u dfr%u",
             (unsigned)fRxAll, (unsigned)fRxOk, (unsigned)fRxBad,
             (unsigned)fRxLastType, (unsigned)fRxLastLen, (unsigned)fRxCtlMax,
             (unsigned)fRxData, (unsigned)fRxDataFrames);
    setProperty("TbAccelRxStat", v);
}

/* SAFETY: do NOT call the family's per-path getters
 * (getXDomainPathForPathID / getTxQueueForPathID / getRxQueueForPathID) unless a
 * path is known to exist. With no path table they dereference NULL inside
 * IOThunderboltFamily and take the machine down:
 *   Kernel data abort, far: 0x0, pc in IOThunderboltFamily, panicked task
 *   "tbaccel" (build bf22ce99 called this from start(), which boot-looped).
 * This function therefore only reports client state. */
void TbAccelClient::probeXdc()
{
    setProperty("TbAccelXdc", fXdc ? "yes" : "no");
}

/* ---------------------------------------------------------------- lookups */

TbAccelBuffer *TbAccelClient::findBuffer(uint64_t handle)
{
    for (unsigned i = 0; i < fBuffers->getCount(); i++) {
        TbAccelBuffer *b = OSDynamicCast(TbAccelBuffer, fBuffers->getObject(i));
        if (b && b->handle == handle) return b;
    }
    return nullptr;
}

TbAccelRegion *TbAccelClient::findRegion(uint64_t id)
{
    for (unsigned i = 0; i < fRegions->getCount(); i++) {
        TbAccelRegion *r = OSDynamicCast(TbAccelRegion, fRegions->getObject(i));
        if (r && r->id == id) return r;
    }
    return nullptr;
}

TbAccelSlot *TbAccelClient::findSlot(uint64_t id)
{
    for (unsigned i = 0; i < fSlots->getCount(); i++) {
        TbAccelSlot *s = OSDynamicCast(TbAccelSlot, fSlots->getObject(i));
        if (s && s->id == id) return s;
    }
    return nullptr;
}

/* Peer slots are addressed by the *client's* buffer id (what PREPARED reported
 * as the handle and MATMUL carries in .a), not by our slot id. */
TbAccelSlot *TbAccelClient::findPeerSlot(uint64_t peerId)
{
    for (unsigned i = 0; i < fSlots->getCount(); i++) {
        TbAccelSlot *s = OSDynamicCast(TbAccelSlot, fSlots->getObject(i));
        if (s && s->isPeer && s->peerId == peerId) return s;
    }
    return nullptr;
}

void TbAccelClient::wakeSlots()
{
    if (fGate) fGate->commandWakeup(fSlots);
    if (fGate) fGate->commandWakeup(fAcks);
    if (fGate) fGate->commandWakeup(fBlobs);
    if (fGate) fGate->commandWakeup(&fCredit);
}

/* ------------------------------------------------------------- control rx */

void TbAccelClient::handleCtl(const TbAccelWire *w, const uint8_t *payload, uint32_t payLen)
{
    switch (w->type) {
    case TBACCEL_MSG_ARM: {
        uint64_t arm_len = w->len;
        uint32_t armed = 0;
        uint64_t total = 0;
        TbAccelSlot *armSlot = nullptr;
        IOLockLock(fTableLock);
        TbAccelSlot *slot = findSlot(w->slot);
        if (slot) {
            total = arm_len < slot->region->size ? arm_len : slot->region->size;
            slot->total = total;
            slot->armed_off = 0;
            slot->signaled = false;
            /* The operation that this payload belongs to rides the ARM, which
             * is how tbaccel_transfer() on Linux sends it (the receiver then
             * sees it as the slot's op). */
            slot->opLen = payLen < sizeof(slot->op) ? payLen : 0;
            if (slot->opLen && payload)
                memcpy(slot->op, payload, slot->opLen);
            slot->t0 = mach_absolute_time();
            /* Every slot - in-kernel peer or userspace peer - is assembled from
             * the control buffers (see ctlRxComplete): the ring FIFO hands the
             * first frames to the buffers that are already posted, so arming
             * per-slot commands would simply never see them.  The window we
             * grant is what the sender may put in flight. */
            if (total) {
                uint64_t window = (uint64_t)TBACCEL_WINDOW * TB_MAX_FRAME_SIZE;
                fRxStreamSlot = slot;
                fRxStreamOff = 0;
                memset(fRxEv, 0, sizeof fRxEv);
                armed = (uint32_t)(total < window ? total : window);
            } else {
                slot->signaled = true;
                armSlot = slot;
            }
        } else {
            /* No Mac-side slot for this id. That is the normal state while the
             * Mac userspace client is not running, and it used to make us ACK
             * armed=0 - which the peer reads as "nothing to send", so it fires
             * its fence without streaming a single frame. Grant the window
             * straight into the kext's own receive ring instead: the ring
             * already holds TBACCEL_RING_SIZE armed commands, so inbound data
             * can be accepted and accounted for without any userspace on the
             * Mac. When the real slot machinery lands this branch disappears. */
            uint64_t cap = (uint64_t)TBACCEL_RING_SIZE * TB_MAX_FRAME_SIZE;
            total = arm_len < cap ? arm_len : cap;
            armed = (uint32_t)total;
            /* Publish it: this is the marker that tells us whether the peer's
             * data should now appear in TbAccelRxStat. */
            char v[80];
            snprintf(v, sizeof v, "slot%llu len%llu armed%u",
                     (unsigned long long)w->slot,
                     (unsigned long long)arm_len, (unsigned)armed);
            setProperty("TbAccelArmNoSlot", v);
        }
        IOLockUnlock(fTableLock);
        {
            char v[96];
            snprintf(v, sizeof v, "slot%llu len%llu total%llu armed%u peer%d",
                     (unsigned long long)w->slot, (unsigned long long)arm_len,
                     (unsigned long long)total, (unsigned)armed,
                     (slot && slot->isPeer) ? 1 : 0);
            setProperty("TbAccelArm", v);
        }
        /* ACK carries armed-window bytes plus the total in the payload */
        sendCtl(TBACCEL_MSG_ACK, w->slot, armed, &total, sizeof(total));
        if (armSlot) {
            /* Zero-length payload: the op is complete as soon as it arrives. */
            wakeSlots();
            if (armSlot->isPeer)
                peerSchedule();
        }
        break;
    }
    case TBACCEL_MSG_ACK: {
        TbAccelAck *ack = OSTypeAlloc(TbAccelAck);
        if (ack) {
            ack->slot = w->slot;
            ack->len = w->len;
            ack->total = 0;
            if (payLen >= sizeof(uint64_t))
                memcpy(&ack->total, payload, sizeof(uint64_t));
            IOLockLock(fTableLock);
            fAcks->setObject(ack);
            IOLockUnlock(fTableLock);
            ack->release();
            wakeSlots();
        }
        break;
    }
    case TBACCEL_MSG_CREDIT: {
        IOLockLock(fTableLock);
        fCredit += w->len;
        IOLockUnlock(fTableLock);
        if (fGate) fGate->commandWakeup(&fCredit);
        break;
    }
    case TBACCEL_MSG_DONE: {
        uint32_t clen = w->payload_len < payLen ? w->payload_len : payLen;
        TbAccelSlot *slot = nullptr;
        IOLockLock(fTableLock);
        if (clen) {
            TbAccelBlob *blob = OSTypeAlloc(TbAccelBlob);
            blob->slot = w->slot;
            blob->len = clen;
            memcpy(blob->data, payload, clen);
            fBlobs->setObject(blob);
            blob->release();
        }
        slot = findSlot(w->slot);
        if (slot)
            slot->signaled = true;
        IOLockUnlock(fTableLock);
        if (slot && fGate) fGate->commandWakeup(slot);
        break;
    }
    case TBACCEL_MSG_BLOB: {
        uint32_t blen = w->payload_len < payLen ? w->payload_len : payLen;
        if (fUserPeer) {
            IOLockLock(fTableLock);
            TbAccelBlob *blob = OSTypeAlloc(TbAccelBlob);
            blob->slot = w->slot;
            blob->len = blen;
            memcpy(blob->data, payload, blen);
            fBlobs->setObject(blob);
            setProperty("TbAccelBlobs", (unsigned long long)fBlobs->getCount());
            blob->release();
            IOLockUnlock(fTableLock);
            if (fGate) fGate->commandWakeup(fBlobs);
        }
        /* A peer receives its requests this way: PREPARE arrives as a blob and
         * is answered with a PREPARED blob.  With a user client attached the
         * blob has already been queued above for recv_ctrl, and the user-space
         * peer owns the buffers, so the in-kernel peer must stay out of it. */
        if (!fUserPeer && blen >= sizeof(TbxMsg))
            peerOnBlob(payload, blen);
        break;
    }
    default:
        break;
    }
}

/* --------------------------------------------------------- data arm/stream */

IOReturn TbAccelClient::armOne(TbAccelSlot *slot, uint64_t off)
{
    TbAccelRegion *region = slot->region;
    if (off >= slot->total)
        return kIOReturnSuccess;

    uint64_t chunk = slot->total - off;
    if (chunk > TBACCEL_DATA_CHUNK)
        chunk = TBACCEL_DATA_CHUNK;

    IOThunderboltReceiveCommand *cmd = OSTypeAlloc(IOThunderboltReceiveCommand);
    if (!cmd || !cmd->initWithControllerAndQueue(fController, fRxQ)) {
        if (cmd) cmd->release();
        return kIOReturnNoMemory;
    }
    /* ZEROCOPY: land the frame straight in the surface's descriptor */
    cmd->setMemoryDescriptor(region->buffer->md);
    cmd->setOffset(region->offset + off);
    cmd->setLength(chunk);
    cmd->setSOF(TBACCEL_PDF_SOF);
    cmd->setEOF(TBACCEL_PDF_EOF);
    cmd->setRefcon(slot);
    cmd->setCompletion({this, &TbAccelClient::dataRxComplete, slot});
    if (fRxQ->submit(cmd) != 0) {
        cmd->release();
        return kIOReturnError;
    }
    slot->armed_off = off + chunk;
    return kIOReturnSuccess;
}

IOReturn TbAccelClient::armWindow(TbAccelSlot *slot)
{
    uint64_t window = (uint64_t)TBACCEL_WINDOW * TB_MAX_FRAME_SIZE;
    uint64_t end = slot->armed_off + window;
    if (end > slot->total)
        end = slot->total;
    while (slot->armed_off < end) {
        uint64_t off = slot->armed_off;
        if (armOne(slot, off) != kIOReturnSuccess)
            break;
    }
    return kIOReturnSuccess;
}

void TbAccelClient::dataRxComplete(void *target, void *refcon, IOReturn status,
                                   IOThunderboltReceiveCommand *cmd)
{
    TbAccelClient *me = (TbAccelClient *)target;
    TbAccelSlot *slot = (TbAccelSlot *)refcon;
    if (!me || !cmd) {
        if (cmd) cmd->release();
        return;
    }
    uint64_t off = cmd->getOffset();
    cmd->release();

    bool complete = false;
    if (slot && me->fReady) {
        TbAccelRegion *region = slot->region;
        uint64_t rel = off >= region->offset ? off - region->offset : 0;
        IOLockLock(me->fTableLock);
        if (rel + TB_MAX_FRAME_SIZE < slot->total) {
            /* re-arm at the current tail and hand the sender credit */
            uint64_t off2 = slot->armed_off;
            if (me->armOne(slot, off2) == kIOReturnSuccess)
                me->sendCtl(TBACCEL_MSG_CREDIT, slot->id, (uint32_t)TB_MAX_FRAME_SIZE, nullptr, 0);
        } else {
            /* last frame of this transfer */
            slot->signaled = true;
            slot->armed_off = slot->total;
            complete = true;
        }
        IOLockUnlock(me->fTableLock);
        if (complete) {
            me->fRxData++;
            me->wireStat();
            me->wakeSlots();
            me->deliverOp(slot);
        }
    }
}

IOReturn TbAccelClient::streamRegion(TbAccelRegion *region, uint64_t total)
{
    TbAccelXfer *xfer = OSTypeAlloc(TbAccelXfer);
    if (!xfer)
        return kIOReturnNoMemory;
    xfer->frames = (uint32_t)((total + TBACCEL_TX_CHUNK - 1) / TBACCEL_TX_CHUNK);
    xfer->status = 0;
    xfer->outSlot = nullptr;

    uint64_t sent = 0;
    while (sent < total) {
        uint64_t chunk = total - sent;
        if (chunk > TBACCEL_TX_CHUNK)
            chunk = TBACCEL_TX_CHUNK;

        /* wait until the receiver has credited enough bytes */
        AbsoluteTime deadline;
        clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
        while (fCredit < sent + chunk) {
            if (fGate->commandSleep(&fCredit, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
                xfer->release();
                return kIOReturnTimeout;
            }
        }

        IOThunderboltTransmitCommand *cmd = OSTypeAlloc(IOThunderboltTransmitCommand);
        if (!cmd || !cmd->initWithControllerAndQueue(fController, fTxQ)) {
            if (cmd) cmd->release();
            xfer->release();
            return kIOReturnNoMemory;
        }
        cmd->setMemoryDescriptor(region->buffer->md);
        cmd->setOffset(region->offset + sent);
        cmd->setLength(chunk);
        cmd->setSOF(TBACCEL_PDF_SOF);
        cmd->setEOF(TBACCEL_PDF_EOF);
        cmd->setRefcon(xfer);
        cmd->setCompletion({this, &TbAccelClient::dataTxComplete, xfer});
        if (fTxQ->submit(cmd) != 0) {
            cmd->release();
            xfer->release();
            return kIOReturnError;
        }
        fTxSubmitted++;
        sent += chunk;
    }
    return kIOReturnSuccess;
}

void TbAccelClient::dataTxComplete(void *target, void *refcon, IOReturn status,
                                   IOThunderboltTransmitCommand *cmd)
{
    TbAccelClient *me = (TbAccelClient *)target;
    TbAccelXfer *xfer = (TbAccelXfer *)refcon;
    if (me) {
        me->fTxCompleted++;
        if (me->fGate)
            me->fGate->commandWakeup((void *)&me->fTxCompleted);
    }
    if (cmd) cmd->release();
    if (!xfer) return;
    if (status != kIOReturnSuccess) xfer->status = (uint32_t)status;
    if (--xfer->frames == 0) {
        if (xfer->outSlot) {
            xfer->outSlot->signaled = true;
            if (me && me->fGate) me->fGate->commandWakeup(xfer->outSlot);
        }
        xfer->release();
    }
}

/* --------------------------------------------------------------- operations */

#define LOCK()   do { if (fTableLock) IOLockLock(fTableLock); } while (0)
#define UNLOCK() do { if (fTableLock) IOLockUnlock(fTableLock); } while (0)

IOReturn TbAccelClient::opGetInfo(tbaccel_info *info)
{
    info->abi_version = TBACCEL_ABI_VERSION;
    info->max_ctrl    = TBACCEL_CTRL_PAYLOAD;
    info->frame_size  = TB_MAX_FRAME_SIZE;
    info->pad         = 0;
    info->features    = TBACCEL_FEAT_FENCES | TBACCEL_FEAT_RECV_CTRL | TBACCEL_FEAT_ZEROCOPY;
    return kIOReturnSuccess;
}

static TbAccelBuffer *newBuffer(uint64_t handle, IOMemoryDescriptor *md, uint64_t size)
{
    TbAccelBuffer *b = OSTypeAlloc(TbAccelBuffer);
    if (!b) return nullptr;   /* caller owns md */
    b->handle = handle;
    b->md = md;
    b->map = nullptr;
    b->kva = nullptr;
    b->prepared = false;
    b->size = size;
    return b;
}

IOReturn TbAccelClient::opImport(tbaccel_import *req)
{
    if (req->size == 0)
        return kIOReturnBadArgument;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, req->size, PAGE_SIZE);
    if (!md)
        return kIOReturnNoMemory;

    LOCK(); uint64_t handle = fNextId++; UNLOCK();
    TbAccelBuffer *b = newBuffer(handle, md, req->size);
    if (!b) { md->release(); return kIOReturnNoMemory; }

    LOCK(); fBuffers->setObject(b); UNLOCK();
    b->release();
    req->handle = handle;
    req->size = md->getLength();
    return kIOReturnSuccess;
}

/* Import a task-mapped IOSurface (or any page-aligned user buffer) as a buffer. */
IOReturn TbAccelClient::opImportSurface(tbaccel_import_surface *req)
{
    if (!req->size || (req->addr & (PAGE_SIZE - 1)))
        return kIOReturnBadArgument;

    task_t task = current_task();
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
        req->addr, req->size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnNoMemory;

    /* Imported user memory is not an IOBufferMemoryDescriptor: wire it and make
     * a kernel mapping, which is the only way to touch it from here. */
    IOReturn kr = md->prepare(kIODirectionInOut);
    if (kr != kIOReturnSuccess) {
        md->release();
        return kr;
    }
    /* flags bit 0: give the kernel an uncached view.  A page the GPU writes
     * (the compute fence) must be read straight from memory, not through a
     * cache line that was populated before the GPU stored to it. */
    IOOptionBits mapOpts = kIOMapAnywhere;
    if (req->flags & 1)
        mapOpts |= kIOMapInhibitCache;
    IOMemoryMap *map = md->createMappingInTask(kernel_task, 0, mapOpts,
                                               0, req->size);
    if (!map) {
        md->complete();
        md->release();
        return kIOReturnNoMemory;
    }

    LOCK(); uint64_t handle = fNextId++; UNLOCK();
    TbAccelBuffer *b = newBuffer(handle, md, req->size);
    if (!b) { map->release(); md->complete(); md->release(); return kIOReturnNoMemory; }
    b->map = map;
    b->kva = (void *)map->getAddress();
    b->prepared = true;
    setProperty("TbAccelSurfaceKva", (unsigned long long)(uintptr_t)b->kva);

    LOCK(); fBuffers->setObject(b); UNLOCK();
    b->release();
    req->handle = handle;
    return kIOReturnSuccess;
}

IOReturn TbAccelClient::opRegion(tbaccel_region *req)
{
    TbAccelRegion *region;

    LOCK();
    TbAccelBuffer *buf = findBuffer(req->handle);
    if (!buf) { UNLOCK(); return kIOReturnNotFound; }
    if (req->offset > buf->size || req->size > buf->size - req->offset) { UNLOCK(); return kIOReturnBadArgument; }
    if (req->offset % TB_MAX_FRAME_SIZE) { UNLOCK(); return kIOReturnBadArgument; }
    UNLOCK();

    region = OSTypeAlloc(TbAccelRegion);
    if (!region) return kIOReturnNoMemory;
    region->id = fNextId++;
    region->buffer = buf;
    region->offset = req->offset;
    region->size = req->size;

    LOCK(); fRegions->setObject(region); UNLOCK();
    region->release();
    req->region = region->id;
    return kIOReturnSuccess;
}

IOReturn TbAccelClient::opFree(const tbaccel_free *req)
{
    LOCK();
    if (req->flags == TBACCEL_FREE_REGION) {
        for (unsigned i = 0; i < fRegions->getCount(); i++) {
            TbAccelRegion *r = OSDynamicCast(TbAccelRegion, fRegions->getObject(i));
            if (r && r->id == req->id) { fRegions->removeObject(i); break; }
        }
    } else if (req->flags == TBACCEL_FREE_BUFFER) {
        for (unsigned i = 0; i < fBuffers->getCount(); i++) {
            TbAccelBuffer *b = OSDynamicCast(TbAccelBuffer, fBuffers->getObject(i));
            if (b && b->handle == req->id) {
                /* TbAccelBuffer::free() unmaps and releases the descriptor. */
                fBuffers->removeObject(i);
                break;
            }
        }
    }
    UNLOCK();
    return kIOReturnSuccess;
}

IOReturn TbAccelClient::opPostRecv(tbaccel_post_recv *req)
{
    TbAccelRegion *region;
    uint64_t id;

    LOCK();
    region = findRegion(req->region);
    if (!region) { UNLOCK(); return kIOReturnNotFound; }
    id = fNextId++;
    UNLOCK();

    TbAccelSlot *slot = OSTypeAlloc(TbAccelSlot);
    if (!slot) return kIOReturnNoMemory;
    slot->id = id;
    slot->region = region;
    slot->total = 0;
    slot->armed_off = 0;
    slot->signaled = false;
    slot->isPeer = false;
    slot->peerId = 0;
    slot->opLen = 0;
    slot->t0 = 0;

    LOCK(); fSlots->setObject(slot); UNLOCK();
    slot->release();

    req->slot = slot->id;
    req->fence_fd = (int32_t)slot->id;
    return kIOReturnSuccess;
}

/* ARM + ACK wait + stream, with the operation riding the ARM. This is what a
 * Linux receiver expects (tbaccel_transfer() sends the op as the ARM payload),
 * and what a Mac peer needs in order to hand a MATMUL's result back together
 * with its RESULT message. */
/*
 * Sending one operation to a peer slot is deliberately split in two:
 *
 *   submitArm()     sends the ARM (a control frame)
 *   [GPU fence wait - MUST happen outside the command gate]
 *   submitStream()  collects the peer's ACK and streams the region
 *
 * The GPU wait must not run inside a gate action: while it spins it would hold
 * the gate, and the work loop's commandWakeup() calls (TX completions, ACKs,
 * drain fence) block on that gate - stalling the very pipeline the wait is
 * trying to overlap with.
 */
IOReturn TbAccelClient::submitArm(uint64_t slotId, TbAccelRegion *region,
                                  uint64_t total, const void *op, uint32_t opLen)
{
    return sendCtl(TBACCEL_MSG_ARM, slotId,
                   region ? (uint32_t)(total ? total : region->size) : 0, op, opLen);
}

IOReturn TbAccelClient::submitStream(uint64_t slotId, TbAccelRegion *region,
                                     uint64_t total, const void *op, uint32_t opLen)
{
    uint64_t armed = 0;

    AbsoluteTime deadline;
    clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
    for (;;) {
        TbAccelAck *found = nullptr;
        LOCK();
        for (unsigned i = 0; i < fAcks->getCount(); i++) {
            TbAccelAck *a = OSDynamicCast(TbAccelAck, fAcks->getObject(i));
            if (a && a->slot == slotId) { a->retain(); fAcks->removeObject(i); found = a; break; }
        }
        UNLOCK();
        if (found) {
            armed = found->len;
            uint64_t peerTotal = found->total;
            found->release();
            if (region && total == 0)
                total = peerTotal;
            break;
        }
        if (fGate->commandSleep(fAcks, deadline, THREAD_UNINT) == THREAD_TIMED_OUT)
            return kIOReturnTimeout;
    }

    if (region) {
        if (total == 0 || total > region->size)
            total = region->size;
        LOCK(); fCredit = armed; UNLOCK();
        if (fGate) fGate->commandWakeup(&fCredit);
        if (total) {
            IOReturn r = streamRegion(region, total);
            if (r != kIOReturnSuccess)
                return r;
        }
    }
    return kIOReturnSuccess;
}

/* Arm + stream without a GPU fence: used by the in-kernel peer, which runs on a
 * thread_call (no gate) and has no GPU work to wait for. */
IOReturn TbAccelClient::submitOpToSlot(uint64_t slotId, TbAccelRegion *region,
                                       uint64_t total, const void *op, uint32_t opLen,
                                       uint64_t waitHandle, uint64_t waitValue,
                                       uint32_t waitMs)
{
    IOReturn r = submitArm(slotId, region, total, op, opLen);
    if (r != kIOReturnSuccess)
        return r;
    if (waitHandle) {
        r = waitGpuValue(waitHandle, waitValue, waitMs);
        if (r != kIOReturnSuccess)
            return r;
    }
    return submitStream(slotId, region, total, op, opLen);
}

IOReturn TbAccelClient::opSubmit(const tbaccel_submit *req, const void *blob, uint32_t blobLen,
                                 uint64_t waitHandle, uint64_t waitValue, uint32_t waitMs,
                                 TbAccelRegion **outRegion)
{
    TbAccelRegion *region = nullptr;

    if (blobLen > TBACCEL_CTRL_PAYLOAD)
        return kIOReturnBadArgument;

    if (req->region != TBACCEL_NONE) {
        LOCK(); region = findRegion(req->region); UNLOCK();
        if (!region) return kIOReturnNotFound;
    }

    /* The GPU fence is waited on by the caller, outside the gate, between this
     * arm phase and opSubmitFinish(). */
    if (region)
        region->retain();
    IOReturn r = submitArm(req->slot, region, region ? region->size : 0, blob, blobLen);
    if (r != kIOReturnSuccess) {
        if (region)
            region->release();
        return r;
    }
    *outRegion = region;
    return kIOReturnSuccess;
}

void TbAccelClient::opSubmitAbort(TbAccelRegion *region)
{
    if (region)
        region->release();
}

IOReturn TbAccelClient::opSubmitFinish(uint64_t slot, TbAccelRegion *region,
                                       const void *blob, uint32_t blobLen)
{
    IOReturn r = submitStream(slot, region, region ? region->size : 0, blob, blobLen);
    if (region)
        region->release();
    return r;
}

IOReturn TbAccelClient::opSendCtrl(const tbaccel_send_ctrl *req, const void *blob)
{
    if (req->len > TBACCEL_CTRL_PAYLOAD)
        return kIOReturnBadArgument;
    return sendCtl(TBACCEL_MSG_BLOB, req->slot, 0, blob, req->len);
}

/* ------------------------------------------------------------- peer (compute)
 *
 * Mirrors linux/tbaccel_peer.c without userspace: PREPARE allocates a kernel
 * buffer and a slot, LOAD/MATMUL ride the ARM of the transfer that carries their
 * payload, and a MATMUL answers by ARMing the result into the client's posted
 * slot (which is what makes the client's out-fence fire).
 *
 * The point of this path is latency measurement: it is the same code that will
 * later be replaced by MTL/ANE work, so the numbers include everything except
 * the compute itself (published separately).
 */

bool TbAccelClient::peerAllocRegion(uint64_t size, TbAccelRegion **out)
{
    if (!size || size > (64ull << 20))
        return false;

    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, (vm_size_t)size, PAGE_SIZE);
    if (!md)
        return false;
    uint64_t len = md->getLength();

    uint64_t handle;
    LOCK(); handle = fNextId++; UNLOCK();
    TbAccelBuffer *b = newBuffer(handle, md, len);
    if (!b)
        return false;

    TbAccelRegion *r = OSTypeAlloc(TbAccelRegion);
    if (!r) { b->release(); return false; }
    LOCK(); r->id = fNextId++; UNLOCK();
    r->buffer = b;
    r->offset = 0;
    r->size = len;

    LOCK();
    fBuffers->setObject(b);
    fRegions->setObject(r);
    UNLOCK();
    b->release();
    *out = r;                /* fRegions keeps it alive */
    r->release();
    return true;
}

void TbAccelClient::peerOnBlob(const void *payload, uint32_t len)
{
    if (!payload || len < sizeof(TbxMsg))
        return;
    const TbxMsg *m = (const TbxMsg *)payload;
    if (m->magic != TBX_MAGIC)
        return;
    if (m->type == TBX_PREPARE)
        peerPrepare(m);
}

void TbAccelClient::peerPrepare(const TbxMsg *m)
{
    TbAccelRegion *region = nullptr;
    if (!peerAllocRegion(m->a, &region)) {
        fPeerErrs++;
        snprintf(fPeerErr, sizeof fPeerErr, "prepare-nomem size%llu",
                 (unsigned long long)m->a);
        setProperty("TbAccelPeerErr", fPeerErr);
        return;
    }

    TbAccelSlot *slot = OSTypeAlloc(TbAccelSlot);
    if (!slot) { fPeerErrs++; return; }
    slot->region = region;
    LOCK(); slot->id = fNextId++; UNLOCK();
    slot->total = 0;
    slot->armed_off = 0;
    slot->signaled = false;
    slot->isPeer = true;
    slot->peerId = m->id;
    slot->opLen = 0;
    slot->t0 = 0;

    uint64_t slotId = slot->id;
    LOCK(); fSlots->setObject(slot); UNLOCK();
    slot->release();

    TbxMsg r = {};
    r.magic = TBX_MAGIC;
    r.type  = TBX_PREPARED;
    r.id    = m->id;
    r.a     = m->id;        /* handle: we address buffers by the client's id */
    r.b     = slotId;
    sendCtl(TBACCEL_MSG_BLOB, 0, 0, &r, sizeof r);

    char v[96];
    snprintf(v, sizeof v, "id%llu size%llu slot%llu",
             (unsigned long long)m->id, (unsigned long long)m->a,
             (unsigned long long)slotId);
    setProperty("TbAccelPeerPrepared", v);
}

/* Take the next chunk of the open stream. The size is dictated by the protocol
 * - the sender fills frames of TB_MAX_FRAME_SIZE - not by what this completion
 * reports: the NHI returns a non-success status and an unusable length for
 * received data frames (Linux ignores the status entirely), so trusting it
 * would either drop the transfer or desynchronise the offsets. */
bool TbAccelClient::peerDataChunk(const uint8_t *data, uint32_t len)
{
    bool done = false;
    TbAccelSlot *slot;

    IOLockLock(fTableLock);
    slot = fRxStreamSlot;
    if (!slot || !data || !len || !slot->total || fRxStreamOff >= slot->total) {
        IOLockUnlock(fTableLock);
        return false;
    }

    /* The frame size is whatever the sender chose: on a successful completion
     * the NHI reports it exactly, so the receiver no longer needs to be built
     * with a chunk that matches the sender. (Only failed completions report
     * unusable lengths, and those are rejected before reaching here.) */
    uint64_t take = slot->total - fRxStreamOff;
    if (take > len)
        take = len;

    uint8_t *dst = (uint8_t *)bufferBytes(slot->region->buffer);
    if (!dst) {
        IOLockUnlock(fTableLock);
        return false;
    }
    memcpy(dst + slot->region->offset + fRxStreamOff, data, (size_t)take);
    fRxStreamOff += take;

    if (fRxStreamOff >= slot->total) {
        slot->signaled = true;
        slot->armed_off = slot->total;
        fRxStreamSlot = nullptr;
        fRxStreamOff = 0;
        fRxCreditPending = 0;
        done = true;
    } else {
        /* Report consumed credit in batches. Sending one CREDIT per frame is
         * what broke the reverse direction: the peer posts a single control
         * receive buffer, so a burst of hundreds of CREDIT frames starves it
         * and the next real control frame (our result ARM) is dropped, leaving
         * the peer to time out waiting for an ACK that was never sent. The
         * window granted in the ACK covers the whole transfer in any case. */
        fRxCreditPending += take;
        if (fRxCreditPending >= (64u * 1024u)) {
            sendCtl(TBACCEL_MSG_CREDIT, slot->id, (uint32_t)fRxCreditPending, nullptr, 0);
            fRxCreditPending = 0;
        }
    }
    IOLockUnlock(fTableLock);

    if (done) {
        fRxData++;
        wakeSlots();
        deliverOp(slot);
    }
    return true;
}

/* A completed transfer: run it in-kernel, or hand it to the user-space peer. */
void TbAccelClient::deliverOp(TbAccelSlot *slot)
{
    if (!slot)
        return;
    if (slot->isPeer && !fUserPeer) {
        /* Off this work loop: a MATMUL must wait for the client's ACK, which is
         * delivered here. */
        peerSchedule();
        return;
    }
    if (!fUserPeer || !slot->opLen)
        return;

    TbAccelBlob *blob = OSTypeAlloc(TbAccelBlob);

    if (!blob)
        return;
    {
        uint32_t n = slot->opLen < TBACCEL_CTRL_PAYLOAD
                   ? slot->opLen : TBACCEL_CTRL_PAYLOAD;

        blob->slot = slot->id;
        blob->len = n;
        memcpy(blob->data, slot->op, n);
    }
    LOCK();
    fBlobs->setObject(blob);
    setProperty("TbAccelBlobs", (unsigned long long)fBlobs->getCount());
    UNLOCK();
    blob->release();
    if (fGate)
        fGate->commandWakeup(fBlobs);
}

void TbAccelClient::peerSchedule(){
    if (fPeerTC)
        thread_call_enter(fPeerTC);
}

void TbAccelClient::peerThread(void *arg, thread_call_param_t param)
{
    (void)param;
    /* thread_call_allocate(fn, refcon) delivers the refcon as the *first*
     * argument; thread_call_enter() passes 0 as the second. Reading the second
     * made this thread a no-op that silently dropped every operation. */
    TbAccelClient *me = (TbAccelClient *)arg;
    if (!me)
        return;
    if (me->fPeerBusy) {
        /* A completion arrived while the previous pass was still running. The
         * slot stays marked, so re-arm the call instead of dropping the work. */
        me->fPeerSkips++;
        thread_call_enter(me->fPeerTC);
        return;
    }
    me->fPeerBusy = true;
    me->fPeerRuns++;

    /* One operation at a time; the client submits serially anyway. The actual
     * work runs through the command gate because it waits for the client's ACK
     * and for transmit credits with commandSleep, which is only valid with the
     * gate held. Sleeping there releases the gate, so the work loop stays free
     * to deliver exactly those completions. */
    for (;;) {
        TbAccelSlot *slot = nullptr;
        IOLockLock(me->fTableLock);
        for (unsigned i = 0; i < me->fSlots->getCount(); i++) {
            TbAccelSlot *s = OSDynamicCast(TbAccelSlot, me->fSlots->getObject(i));
            if (s && s->isPeer && s->signaled && s->opLen) { s->retain(); slot = s; break; }
        }
        IOLockUnlock(me->fTableLock);
        if (!slot)
            break;
        if (me->fGate)
            me->fGate->runAction(&TbAccelClient::sPeerDispatch, slot);
        slot->release();
    }

    me->fPeerBusy = false;
}

IOReturn TbAccelClient::sPeerDispatch(OSObject *owner, void *arg0, void *, void *, void *)
{
    TbAccelClient *me = (TbAccelClient *)owner;
    TbAccelSlot *slot = (TbAccelSlot *)arg0;
    if (me && slot)
        me->peerDispatch(slot);
    return kIOReturnSuccess;
}

void TbAccelClient::peerDispatch(TbAccelSlot *slot)
{
    if (!slot) {
        setProperty("TbAccelPeerOp", "no-slot");
        return;
    }
    {
        char v[96];
        snprintf(v, sizeof v, "len%u slot%llu peer%d",
                 (unsigned)slot->opLen, (unsigned long long)slot->id,
                 slot->isPeer ? 1 : 0);
        setProperty("TbAccelPeerOp", v);
    }
    if (slot->opLen < sizeof(TbxMsg))
        return;

    TbxMsg op;
    memcpy(&op, slot->op, sizeof op);
    slot->opLen = 0;                 /* consumed */

    if (fUserPeer) {
        /* Deliver the operation, and the slot it landed in, to the user-space
         * peer through recv_ctrl.  It answers by submitting the result into the
         * slot the op names, exactly as the in-kernel peer does. */
        TbAccelBlob *blob = OSTypeAlloc(TbAccelBlob);

        if (blob) {
            blob->slot = slot->id;
            blob->len = sizeof op;
            memcpy(blob->data, &op, sizeof op);
            LOCK(); fBlobs->setObject(blob); UNLOCK();
            blob->release();
            if (fGate) fGate->commandWakeup(fBlobs);
        }
        return;
    }

    {
        char v[96];
        snprintf(v, sizeof v, "type%u magic0x%08x m%u n%u k%u",
                 op.type, op.magic, op.m, op.n, op.k);
        setProperty("TbAccelPeerMsg", v);
    }
    if (op.magic != TBX_MAGIC) {
        fPeerErrs++;
        snprintf(fPeerErr, sizeof fPeerErr, "bad-magic 0x%08x", op.magic);
        setProperty("TbAccelPeerErr", fPeerErr);
        return;
    }

    switch (op.type) {
    case TBX_LOAD:
        /* The payload is already in the slot's buffer; nothing to do but say
         * so, which is what the peer does on Linux as well. */
        setProperty("TbAccelPeerLoad", "ok");
        break;
    case TBX_MATMUL:
        peerMatmul(slot, &op);
        break;
    default:
        break;
    }
}

void TbAccelClient::peerMatmul(TbAccelSlot *inSlot, const TbxMsg *op)
{
    TbAccelSlot *w = findPeerSlot(op->a);
    if (!w || !inSlot || !op->m || !op->n || !op->k) {
        fPeerErrs++;
        snprintf(fPeerErr, sizeof fPeerErr, "matmul-missing w%llu",
                 (unsigned long long)op->a);
        setProperty("TbAccelPeerErr", fPeerErr);
        return;
    }

    uint64_t need = (uint64_t)op->m * op->n * sizeof(float);
    if (!fPeerY || fPeerY->size < need) {
        TbAccelRegion *r = nullptr;
        if (!peerAllocRegion(need, &r)) {
            fPeerErrs++;
            snprintf(fPeerErr, sizeof fPeerErr, "matmul-nomem %llu",
                     (unsigned long long)need);
            setProperty("TbAccelPeerErr", fPeerErr);
            return;
        }
        fPeerY = r;
    }

    const uint8_t *xb = (const uint8_t *)bufferBytes(inSlot->region->buffer);
    const uint8_t *wb = (const uint8_t *)bufferBytes(w->region->buffer);
    uint8_t       *yb = (uint8_t *)bufferBytes(fPeerY->buffer);
    const float *x  = (const float *)(xb + inSlot->region->offset);
    const float *wm = (const float *)(wb + w->region->offset);
    float       *y  = (float *)yb;
    if (!xb || !wb || !yb) {
        fPeerErrs++;
        snprintf(fPeerErr, sizeof fPeerErr, "matmul-nomap");
        setProperty("TbAccelPeerErr", fPeerErr);
        return;
    }

    uint64_t c0 = mach_absolute_time();
    for (uint32_t m = 0; m < op->m; m++) {
        for (uint32_t n = 0; n < op->n; n++) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < op->k; k++)
                acc += x[m * op->k + k] * wm[n * op->k + k];
            y[m * op->n + n] = acc;
        }
    }
    uint64_t c1 = mach_absolute_time();

    TbxMsg res = {};
    res.magic = TBX_MAGIC;
    res.type  = TBX_RESULT;
    res.b     = op->b;            /* the client's posted result slot */

    IOReturn r = submitOpToSlot(op->b, fPeerY, need, &res, sizeof res, 0, 0, 0);
    uint64_t c2 = mach_absolute_time();

    if (r != kIOReturnSuccess) {
        fPeerErrs++;
        snprintf(fPeerErr, sizeof fPeerErr, "result 0x%08x", (unsigned)r);
        setProperty("TbAccelPeerErr", fPeerErr);
        return;
    }

    uint64_t computeNs = 0;
    absolutetime_to_nanoseconds(c1 - c0, &computeNs);
    peerStats(inSlot->t0, c2, computeNs);
}

void TbAccelClient::peerStats(uint64_t t0, uint64_t t1, uint64_t computeNs)
{
    if (!t0 || t1 <= t0)
        return;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    uint64_t us = ns / 1000;
    if (!fPeerOps || us < fPeerMinUs) fPeerMinUs = us;
    if (us > fPeerMaxUs) fPeerMaxUs = us;
    fPeerLastUs = us;
    fPeerComputeUs = computeNs / 1000;
    fPeerOps++;

    char v[160];
    snprintf(v, sizeof v, "ops%u err%u runs%u skips%u last%lluus min%lluus max%lluus compute%lluus",
             (unsigned)fPeerOps, (unsigned)fPeerErrs,
             (unsigned)fPeerRuns, (unsigned)fPeerSkips,
             (unsigned long long)fPeerLastUs, (unsigned long long)fPeerMinUs,
             (unsigned long long)fPeerMaxUs, (unsigned long long)fPeerComputeUs);
    setProperty("TbAccelPeer", v);
}


IOReturn TbAccelClient::opRecvCtrl(tbaccel_recv_ctrl *req, void *out, uint32_t outCap)
{
    for (;;) {
        TbAccelBlob *blob = nullptr;
        LOCK();
        if (fBlobs->getCount()) {
            blob = OSDynamicCast(TbAccelBlob, fBlobs->getObject(0));
            if (blob) { blob->retain(); fBlobs->removeObject(0); }
        }
        UNLOCK();
        if (blob) {
            IOReturn ret = kIOReturnSuccess;
            if (blob->len > outCap || blob->len > req->len) {
                ret = kIOReturnMessageTooLarge;
            } else {
                memcpy(out, blob->data, blob->len);
                req->len = blob->len;
                req->slot = blob->slot;
                req->flags = 0;
            }
            blob->release();
            return ret;
        }
        if (!fReady) return kIOReturnNotOpen;
        if (!fGate) return kIOReturnNotReady;
        /* Bounded wait.  The two-argument commandSleep() overload takes
         * (event, interruptible), not (event, deadline): passing 0 there means
         * "interruptible = 0", which sleeps with no deadline and is not
         * released by commandWakeup() - a userspace recv_ctrl blocked forever
         * even though the blob had been queued.  A 100 ms deadline makes the
         * loop self-healing and costs nothing when a blob is already waiting. */
        {
            AbsoluteTime deadline;

            clock_interval_to_deadline(100, kMillisecondScale, &deadline);
            fGate->commandSleep(fBlobs, deadline, THREAD_UNINT);
        }
    }
}

IOReturn TbAccelClient::opWaitSlot(uint64_t slot, uint32_t timeoutMs)
{
    AbsoluteTime deadline;
    IOReturn ret;

    LOCK(); TbAccelSlot *s = findSlot(slot); UNLOCK();
    if (!s) return kIOReturnNotFound;
    if (!fGate) return kIOReturnNotReady;

    clock_interval_to_deadline(timeoutMs ? timeoutMs : 5000, kMillisecondScale, &deadline);
    ret = fGate->commandSleep(s, deadline, THREAD_UNINT);
    if (ret == THREAD_TIMED_OUT) ret = kIOReturnTimeout;

    LOCK(); s = findSlot(slot); bool ok = s && s->signaled; UNLOCK();
    return ok ? kIOReturnSuccess : ret;
}

/*
 * Block until the transmit commands queued at 'fence' (0 = everything queued
 * right now) have completed.  The sleep is released by the TX completion, so a
 * peer can start the next compute before the previous result has left the wire
 * and only block when it actually wants to reuse the buffer.
 */
/*
 * Wait for a value the GPU writes into an imported page.  This is the kernel
 * side of the compute fence: the peer encodes "store value" after the matmul,
 * so once the value appears the result is in memory and may be transmitted.
 * There is no KPI to be interrupted by an MTLSharedEvent, so this polls the
 * mapped page, but it runs after the ARM handshake, i.e. while the wire round
 * trip and the GPU are already in flight, and it is bounded.
 */
IOReturn TbAccelClient::waitGpuValue(uint64_t handle, uint64_t value, uint32_t timeoutMs)
{
    if (!handle)
        return kIOReturnSuccess;

    LOCK(); TbAccelBuffer *b = findBuffer(handle); if (b) b->retain(); UNLOCK();
    if (!b)
        return kIOReturnNotFound;

    volatile uint32_t *p = (volatile uint32_t *)bufferBytes(b);
    if (!p) {
        b->release();
        return kIOReturnNoMemory;
    }

    uint64_t start = mach_absolute_time();
    uint64_t elapsedNs = 0;
    uint32_t want = (uint32_t)value;

    while (*p < want) {
        absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsedNs);
        if (elapsedNs > (uint64_t)(timeoutMs ? timeoutMs : 2000) * 1000000ull) {
            char v[80];
            snprintf(v, sizeof v, "timeout want%u now%u after%lluns",
                     (unsigned)want, (unsigned)*p, (unsigned long long)elapsedNs);
            setProperty("TbAccelGpuWait", v);
            b->release();
            return kIOReturnTimeout;
        }
    }
    {
        char v[96];
        snprintf(v, sizeof v, "h%llu want%u now%u waited%lluns",
                 (unsigned long long)handle, (unsigned)want, (unsigned)*p,
                 (unsigned long long)elapsedNs);
        setProperty("TbAccelGpuWait", v);
    }
    b->release();
    return kIOReturnSuccess;
}

IOReturn TbAccelClient::opDrainWait(uint64_t fence, uint32_t timeoutMs, uint64_t *drained)
{
    if (!fGate)
        return kIOReturnNotReady;

    uint64_t target = fence ? fence : fTxSubmitted;
    AbsoluteTime deadline;
    clock_interval_to_deadline(timeoutMs ? timeoutMs : 2000, kMillisecondScale, &deadline);

    while (fTxCompleted < target) {
        if (fGate->commandSleep((void *)&fTxCompleted, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
            char v[64];
            snprintf(v, sizeof v, "timeout%llu/%llu", (unsigned long long)target,
                     (unsigned long long)fTxCompleted);
            setProperty("TbAccelFence", v);
            return kIOReturnTimeout;
        }
    }

    if (drained)
        *drained = fTxCompleted;
    {
        char v[64];
        snprintf(v, sizeof v, "%llu/%llu", (unsigned long long)target,
                 (unsigned long long)fTxCompleted);
        setProperty("TbAccelFence", v);
    }
    return kIOReturnSuccess;
}

IOReturn TbAccelClient::opMapBuffer(uint64_t handle, IOMemoryDescriptor **md)
{
    LOCK(); TbAccelBuffer *b = findBuffer(handle); UNLOCK();
    if (!b) return kIOReturnNotFound;
    *md = b->md;
    return kIOReturnSuccess;
}

/* MTLSharedEvent-shaped event table (see ggml-ane's event/fence API). */
IOReturn TbAccelClient::opEvent(tbaccel_event *req)
{
    switch (req->op) {
    case TBACCEL_EVENT_CREATE: {
        TbAccelEvent *ev = OSTypeAlloc(TbAccelEvent);
        if (!ev) return kIOReturnNoMemory;
        LOCK(); ev->id = fNextId++; ev->signaled = 0; fEvents->setObject(ev); UNLOCK();
        req->id = ev->id;
        ev->release();
        return kIOReturnSuccess;
    }
    case TBACCEL_EVENT_SIGNAL: {
        TbAccelEvent *ev = nullptr;
        LOCK();
        for (unsigned i = 0; i < fEvents->getCount(); i++) {
            TbAccelEvent *e = OSDynamicCast(TbAccelEvent, fEvents->getObject(i));
            if (e && e->id == req->id) { ev = e; break; }
        }
        if (ev) ev->signaled = req->value;
        UNLOCK();
        if (ev && fGate) fGate->commandWakeup(ev);
        return ev ? kIOReturnSuccess : kIOReturnNotFound;
    }
    case TBACCEL_EVENT_WAIT: {
        AbsoluteTime deadline;
        clock_interval_to_deadline(req->timeout_ms ? req->timeout_ms : 5000,
                                   kMillisecondScale, &deadline);
        for (;;) {
            TbAccelEvent *ev = nullptr;
            LOCK();
            for (unsigned i = 0; i < fEvents->getCount(); i++) {
                TbAccelEvent *e = OSDynamicCast(TbAccelEvent, fEvents->getObject(i));
                if (e && e->id == req->id) { ev = e; break; }
            }
            bool done = ev && ev->signaled >= req->value;
            UNLOCK();
            if (done) return kIOReturnSuccess;
            if (!ev) return kIOReturnNotFound;
            if (fGate->commandSleep(ev, deadline, THREAD_UNINT) == THREAD_TIMED_OUT)
                return kIOReturnTimeout;
        }
    }
    case TBACCEL_EVENT_DESTROY: {
        LOCK();
        for (unsigned i = 0; i < fEvents->getCount(); i++) {
            TbAccelEvent *e = OSDynamicCast(TbAccelEvent, fEvents->getObject(i));
            if (e && e->id == req->id) { fEvents->removeObject(i); break; }
        }
        UNLOCK();
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnBadArgument;
    }
}

/* ---- gate trampolines ---- */

IOReturn TbAccelClient::sGetInfo(OSObject *o, void *a, void *, void *, void *)
{ return ((TbAccelClient *)o)->opGetInfo((tbaccel_info *)a); }
IOReturn TbAccelClient::sImport(OSObject *o, void *a, void *, void *, void *)
{ return ((TbAccelClient *)o)->opImport((tbaccel_import *)a); }
IOReturn TbAccelClient::sRegion(OSObject *o, void *a, void *, void *, void *)
{ return ((TbAccelClient *)o)->opRegion((tbaccel_region *)a); }
IOReturn TbAccelClient::sFree(OSObject *o, void *a, void *, void *, void *)
{ return ((TbAccelClient *)o)->opFree((tbaccel_free *)a); }
IOReturn TbAccelClient::sPostRecv(OSObject *o, void *a, void *, void *, void *)
{ return ((TbAccelClient *)o)->opPostRecv((tbaccel_post_recv *)a); }
IOReturn TbAccelClient::sSubmit(OSObject *o, void *a, void *, void *, void *)
{
    TbAccelSubmitArgs *p = (TbAccelSubmitArgs *)a;
    IOReturn r = ((TbAccelClient *)o)->opSubmit(&p->req, p->blob, p->blobLen,
                                                p->wait_handle, p->wait_value, p->wait_ms,
                                                &p->region);
    p->fence = (r == kIOReturnSuccess) ? ((TbAccelClient *)o)->drainFence() : 0;
    return r;
}
IOReturn TbAccelClient::sRecvCtrl(OSObject *o, void *a, void *, void *, void *)
{
    TbAccelRecvCtrlArgs *p = (TbAccelRecvCtrlArgs *)a;
    return ((TbAccelClient *)o)->opRecvCtrl(&p->req, p->out, p->cap);
}
IOReturn TbAccelClient::sSendCtrl(OSObject *o, void *a, void *, void *, void *)
{
    TbAccelSendCtrlArgs *p = (TbAccelSendCtrlArgs *)a;
    return ((TbAccelClient *)o)->opSendCtrl(&p->req, p->blob);
}
IOReturn TbAccelClient::sWaitSlot(OSObject *o, void *a, void *, void *, void *)
{
    TbAccelWaitSlotArgs *p = (TbAccelWaitSlotArgs *)a;
    return ((TbAccelClient *)o)->opWaitSlot(p->slot, p->timeout);
}
IOReturn TbAccelClient::sMapBuffer(OSObject *o, void *a, void *, void *, void *)
{
    TbAccelMapBufferArgs *p = (TbAccelMapBufferArgs *)a;
    return ((TbAccelClient *)o)->opMapBuffer(p->handle, p->md);
}
IOReturn TbAccelClient::sImportSurface(OSObject *o, void *a, void *, void *, void *)
{ return ((TbAccelClient *)o)->opImportSurface((tbaccel_import_surface *)a); }
IOReturn TbAccelClient::sEvent(OSObject *o, void *a, void *, void *, void *)
{ return ((TbAccelClient *)o)->opEvent((tbaccel_event *)a); }
IOReturn TbAccelClient::sSubmitAbort(OSObject *o, void *a, void *, void *, void *)
{
    TbAccelSubmitArgs *p = (TbAccelSubmitArgs *)a;
    ((TbAccelClient *)o)->opSubmitAbort(p->region);
    p->region = nullptr;
    return kIOReturnSuccess;
}
IOReturn TbAccelClient::sSubmitFinish(OSObject *o, void *a, void *, void *, void *)
{
    TbAccelSubmitArgs *p = (TbAccelSubmitArgs *)a;
    return ((TbAccelClient *)o)->opSubmitFinish(p->req.slot, p->region,
                                                p->blob, p->blobLen);
}
IOReturn TbAccelClient::sDrain(OSObject *o, void *a, void *, void *, void *)
{
    TbAccelDrainArgs *p = (TbAccelDrainArgs *)a;
    return ((TbAccelClient *)o)->opDrainWait(p->fence, p->timeout, &p->drained);
}
