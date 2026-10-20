/*
 * IOThunderboltFamily.h - reconstructed private API of
 * com.apple.iokit.IOThunderboltFamily (9.3.3, macOS 26.6).
 *
 * HOW THIS WAS DERIVED (and verified)
 * -----------------------------------
 * From the KDK (KDK_26.6.2_25G83.kdk) copy of IOThunderboltFamily and
 * AppleThunderboltIP, using `nm` + `objdump --macho --disassemble`. Key
 * findings that shaped this header:
 *
 *  - A protocol driver is a *direct subclass of IOService* bound to an
 *    IOThunderboltXDomainService provider. AppleThunderboltIPTransmitter's
 *    constructor calls IOService::IOService(OSMetaClass const*) and its
 *    MetaClass records sizeof = 0x168. It is NOT an
 *    IOThunderboltXDomainServiceClient subclass.
 *  - Data movement uses IOThunderboltTransmitQueue / IOThunderboltReceiveQueue
 *    plus IOThunderboltTransmitCommand / IOThunderboltReceiveCommand, each
 *    carrying SOF/EOF and an IOMemoryDescriptor.
 *  - The command completion is a 24-byte value type with layout
 *    { void *target; void (*fn)(void*,void*,IOReturn,Cmd*); void *refcon; }
 *    invoked as fn(target, refcon, status, cmd). (Verified in
 *    IOThunderboltTransmitCommand::complete().)
 *  - Paths are IOThunderboltPath (created with IOThunderboltPath::withController)
 *    whose credits are set via IOThunderboltAbstractPath::setSourceInitialCredits.
 *
 * Return types are not encoded in C++ mangling; they are best effort.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef IOTHUNDERBOLT_FAMILY_H
#define IOTHUNDERBOLT_FAMILY_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOMemoryDescriptor.h>

class IOThunderboltController;
class IOThunderboltLocalNode;
class IOThunderboltSwitch;
class IOThunderboltXDomainLink;
class IOThunderboltXDomainService;
class IOThunderboltPath;
class IOThunderboltXDomainPath;
class IOThunderboltAbstractPath;
class IOThunderboltTransmitQueue;
class IOThunderboltReceiveQueue;
class IOThunderboltTransmitCommand;
class IOThunderboltReceiveCommand;
class IOThunderboltDispatchContext;

class IOThunderboltPort : public IOService
{
    OSDeclareDefaultStructors(IOThunderboltPort)
public:
    uint16_t getPortNumber();
    uint16_t getMaxInHopID();
    /* Returns an IOReturn (0 = success), NOT a bool - return types are not
     * encoded in C++ mangling, which is the same trap that made allocate()
     * look like it was failing on success. */
    uint32_t cmReadDWord(unsigned char offset, unsigned int *value);    uint16_t getMaxOutHopID();
    bool     isUpstream();
};

/* --------------------------------------------------------------------- link */

class IOThunderboltXDomainLink : public OSObject
{
    OSDeclareDefaultStructors(IOThunderboltXDomainLink)
public:
    static IOThunderboltXDomainLink *withController(IOThunderboltController *controller);
    bool initWithController(IOThunderboltController *controller);

    IOThunderboltController *getController();
    void     getXDomainUUID(uint8_t uuid[16]);
    uint64_t getRouteString();
    bool     isInvalid();
    bool     isTerminated();
};

/* ------------------------------------------------------------------ service */

/*
 * One protocol directory advertised by the peer; the IOService that a protocol
 * driver binds to (IOProviderClass = IOThunderboltXDomainService).
 */
class IOThunderboltXDomainService : public IOService
{
    OSDeclareDefaultStructors(IOThunderboltXDomainService)
public:
    static IOThunderboltXDomainService *withXDLink(IOThunderboltXDomainLink *link);
    bool initWithLink(IOThunderboltXDomainLink *link);

    IOThunderboltXDomainLink *getXDomainLink();
    IOThunderboltController  *getController();
    uint64_t                  getRouteString();
    OSDictionary             *getProperties();
    bool                      publish();
    void                      unpublish();
};

/* The family's per-service client. This is what actually owns XDomain path
 * setup: an incoming path request from the peer is handled by
 * processPathSetupRequest -> allocateXDomainPath() + allocateXDomainQueue(),
 * so the path *and* its queues are created here rather than by us. We only
 * look them up by path id afterwards.
 * All methods are declared non-virtual so the calls bind directly to the real
 * symbols and declaration order cannot shift them. */
class IOThunderboltXDomainServiceClient : public OSObject
{
    OSDeclareDefaultStructors(IOThunderboltXDomainServiceClient)
public:
    static IOThunderboltXDomainServiceClient *withXDomainService(IOThunderboltXDomainService *svc);
    bool initWithService(IOThunderboltXDomainService *svc);

    bool attach(IOService *provider);
    void detach(IOService *provider);

    IOThunderboltController     *getController();
    IOThunderboltXDomainLink    *getXDomainLink();
    IOThunderboltPath           *getXDomainPathForPathID(unsigned int pathID);
    IOThunderboltTransmitQueue  *getTxQueueForPathID(unsigned int pathID);
    IOThunderboltReceiveQueue   *getRxQueueForPathID(unsigned int pathID);
    IOThunderboltTransmitQueue  *createTxQueueInternal(unsigned int pathID);
    IOThunderboltReceiveQueue   *createRxQueueInternal(unsigned int pathID);
    IOThunderboltTransmitQueue  *allocateTxQueueInternal(unsigned int pathID, uint64_t maxFrameSize);
    IOThunderboltReceiveQueue   *allocateRxQueueInternal(unsigned int pathID, uint64_t maxFrameSize,
                                                         uint16_t e2eHop);
    void                         registerXDomainPath(IOThunderboltXDomainPath *path, unsigned int pathID);
    void                         registerTransmitQueue(IOThunderboltTransmitQueue *q, unsigned int pathID);
    void                         registerReceiveQueue(IOThunderboltReceiveQueue *q, unsigned int pathID);
    bool                         clientReady(bool ready);

    /*
     * Initialises the client's internal path/queue tables (+0xa0, +0xa8, +0xb0,
     * +0xb8). The unsigned argument is stored but never read anywhere in the
     * function, so its value does not matter; 0 is safe.
     *
     * Note on the path callbacks: setNewXDomainPathCallback takes
     * IOThunderboltXDomainServiceClient::Callback BY VALUE, and that is not a
     * function pointer - the setter does `ldp q0, q1, [x1]` plus `ldr x8,
     * [x1, #0x20]`, i.e. a >=0x28-byte struct passed as a hidden pointer
     * (hence the nested-name mangling `NS_8CallbackE` rather than the
     * structural `PFvPvS0_S0_jE`). We do not set it: the notification path
     * checks the *refcon* first and returns kIOReturnNotReady (0xe00002ea)
     * without calling anything when it is absent, which is the behaviour we
     * want anyway.
     */
    bool registerService(unsigned int serviceId);
};

/* ----------------------------------------------------------- abstract path */

class IOThunderboltAbstractPath : public OSObject
{
    OSDeclareDefaultStructors(IOThunderboltAbstractPath)
public:
    /* vt[0x140] - the single credit value AppleThunderboltIPTransmitter
     * configures a path with (min(window, ring_entries) - 2). The
     * source/destination pair below exists but is not what IP drives. */
    void     setInitialCredits(uint32_t credits);
    void     setSourceInitialCredits(uint32_t credits);
    void     setDestinationInitialCredits(uint32_t credits);

    /* The rest of the path attribute block IP fills in before allocate().
     * These are what end up in the per-hop "path packet" written to the switch,
     * including whether the hop gets a credit counter at all. */
    void setPriority(uint32_t v);
    void setWeight(uint32_t v);
    void setCounterEnable(uint32_t v);
    void setDropPacketEnable(uint32_t v);
    void setIngressFlowControlEnable(uint32_t v);
    void setEgressFlowControlEnable(uint32_t v);
    void setIngressSharedBufferingEnable(uint32_t v);
    void setEgressSharedBufferingEnable(uint32_t v);
    void setNonFlowControlledCredits(uint32_t v);
    void setFlags(uint32_t v);
};

/* {start, end} hop-id range; passed by value in one register (matches RDMA's
 * path->setSourceHopIDRange(hop | (hop << 16))). */
struct IOThunderboltHopIDRange {
    uint16_t start;
    uint16_t end;
};

class IOThunderboltPath : public IOThunderboltAbstractPath
{
    OSDeclareDefaultStructors(IOThunderboltPath)
public:
    static IOThunderboltPath *withController(IOThunderboltController *controller);
    bool initWithController(IOThunderboltController *controller);

    void     setSourceHopIDRange(IOThunderboltHopIDRange range);
    void     setDestinationHopIDRange(IOThunderboltHopIDRange range);
    uint16_t getAllocatedSourceHopID();
    uint16_t getAllocatedDestinationHopID();

    /* Lives on IOThunderboltPath, not on IOThunderboltAbstractPath: the family
     * exports __ZN17IOThunderboltPath8getStateEv only, so declaring it on the
     * base class produces an undefined symbol that kcgen cannot resolve and the
     * whole auxiliary kernel collection build fails (silently, from kmutil's
     * point of view - it still prints "requires a reboot"). */
    uint32_t getState();

    /* Raw IOReturn (the ABI return type; bool would truncate the code). */
    uint32_t allocate();
    void deallocate();

    IOReturn activate();
    IOReturn activateSynchronous();
    IOReturn deactivate(bool synchronous);
    IOReturn deactivateSynchronous();

    IOThunderboltPort *getSourcePort();
    IOThunderboltPort *getDestinationPort();
    void setSourcePort(IOThunderboltPort *port);
    void setDestinationPort(IOThunderboltPort *port);
    void setRoutingOptions(uint32_t options);
    void setRequiredBandwidth(uint32_t bw);
    void setMaximumBandwidth(uint32_t bw);
};

/* XDomain flavour of a path: same allocation/activation, plus a service path id
 * that the family's client uses to look it up when it builds queues for it.
 * This is the object the peer's path-setup request would create, and it is what
 * ties a receive queue's hop to a path. */
class IOThunderboltXDomainPath : public IOThunderboltPath
{
    OSDeclareDefaultStructors(IOThunderboltXDomainPath)
public:
    static IOThunderboltXDomainPath *withXDomainLink(IOThunderboltXDomainLink *link);
    bool              initWithXDomainLink(IOThunderboltXDomainLink *link);
    unsigned int      getServicePathID();
    void              setServicePathID(unsigned int pathID);

    /* Overridden on this class: calling the inherited symbol instead would skip
     * the XDomain path's setup-request machinery, which is the whole point. */
    IOReturn          activate();
    IOReturn          deactivateSynchronous();
};

/* ------------------------------------------------------------------ queues */

class IOThunderboltTransmitQueue : public OSObject
{
    OSDeclareDefaultStructors(IOThunderboltTransmitQueue)
public:
    static IOThunderboltTransmitQueue *withControllerAndWorkLoop(IOThunderboltController *controller,
                                                                 IOWorkLoop *wl);
    static IOThunderboltTransmitQueue *withController(IOThunderboltController *controller);
    bool initWithControllerAndWorkLoop(IOThunderboltController *controller, IOWorkLoop *wl);

    void setRingSize(uint32_t entries);
    void setMaxFrameSize(uint64_t bytes);
    void setQueueType(uint16_t type);
    void setInterruptStride(uint32_t stride);
    void setFlags(uint32_t flags);
    bool enableE2EFlowControl(bool enable);

    /* Raw IOReturn (the ABI return type; bool would truncate the code). */
    uint32_t allocate();
    uint32_t start();
    void stop();
    void shutdown();
    bool isStarted();

    uint32_t submit(IOThunderboltTransmitCommand *command);
    void getHopID(uint16_t *hopID);
};

class IOThunderboltReceiveQueue : public OSObject
{
    OSDeclareDefaultStructors(IOThunderboltReceiveQueue)
public:
    static IOThunderboltReceiveQueue *withControllerAndWorkLoop(IOThunderboltController *controller,
                                                                IOWorkLoop *wl);
    static IOThunderboltReceiveQueue *withController(IOThunderboltController *controller);
    bool initWithControllerAndWorkLoop(IOThunderboltController *controller, IOWorkLoop *wl);

    void setRingSize(uint32_t entries);
    void setMaxFrameSize(uint64_t bytes);
    void setQueueType(uint16_t type);
    void setInterruptStride(uint32_t stride);
    void setSOFPDFBitmask(uint16_t mask);
    void setEOFPDFBitmask(uint16_t mask);
    void setTxE2EHopID(uint16_t hopID);
    void setFlags(uint32_t flags);
    bool enableRawMode(bool enable);
    bool enableE2EFlowControl(bool enable);

    /* Raw IOReturn (the ABI return type; bool would truncate the code). */
    uint32_t allocate();
    uint32_t start();
    void stop();
    void shutdown();
    bool isStarted();

    uint32_t submit(IOThunderboltReceiveCommand *command);
    void getHopID(uint16_t *hopID);
};

/* --------------------------------------------------------------- commands */

class IOThunderboltTransmitCommand : public OSObject
{
    OSDeclareDefaultStructors(IOThunderboltTransmitCommand)
public:
    /*
     * 24-byte value type; layout verified from setCompletion()/complete().
     * fn is invoked as fn(target, refcon, status, cmd).
     */
    class Completion {
    public:
        typedef void (*Fn)(void *target, void *refcon, IOReturn status,
                           IOThunderboltTransmitCommand *cmd);
        void *target;
        Fn    fn;
        void *refcon;
    };

    static IOThunderboltTransmitCommand *withController(IOThunderboltController *controller);
    bool initWithControllerAndQueue(IOThunderboltController *controller,
                                    IOThunderboltTransmitQueue *queue);

    void setMemoryDescriptor(IOMemoryDescriptor *md);
    void setLength(uint64_t length);
    void setOffset(uint64_t offset);
    void setSOF(uint8_t sof);
    void setEOF(uint8_t eof);
    void setRefcon(void *refcon);
    void setCompletion(Completion completion);
    IOReturn getCompletion();
    void *getRefcon();
    IOThunderboltTransmitQueue *getAssociatedQueue();
};

class IOThunderboltReceiveCommand : public OSObject
{
    OSDeclareDefaultStructors(IOThunderboltReceiveCommand)
public:
    class Completion {
    public:
        typedef void (*Fn)(void *target, void *refcon, IOReturn status,
                           IOThunderboltReceiveCommand *cmd);
        void *target;
        Fn    fn;
        void *refcon;
    };

    static IOThunderboltReceiveCommand *withController(IOThunderboltController *controller);
    bool initWithControllerAndQueue(IOThunderboltController *controller,
                                    IOThunderboltReceiveQueue *queue);
    void setMemoryDescriptor(IOMemoryDescriptor *md);
    void setLength(uint64_t length);
    void setOffset(uint64_t offset);
    void setSOF(uint8_t sof);
    void setEOF(uint8_t eof);
    void setRefcon(void *refcon);
    void setCompletion(Completion completion);
    uint8_t  getSOF();
    uint8_t  getEOF();
    uint64_t getLength();
    uint64_t getOffset();
    uint64_t getReceivedLength();
    IOReturn getCompletion();
    void *getRefcon();
    IOThunderboltReceiveQueue *getAssociatedQueue();
};

/* -------------------------------------------------------------- controller */

class IOThunderboltController : public IOService
{
    OSDeclareDefaultStructors(IOThunderboltController)
public:
    IOThunderboltLocalNode *getLocalNode();
    IOService              *getNHIPort();
    /* Route strings identify switches; the peer's switch can be looked up by
     * the XDomain link's route string. Return type is a pointer, which the
     * mangling does encode (unlike scalar returns). */
    IOThunderboltSwitch    *getRootSwitch();
    IOThunderboltSwitch    *switchForRouteString(uint64_t route);
};

/* --------------------------------------------------------------- switch */

class IOThunderboltSwitch : public IOService
{
    OSDeclareDefaultStructors(IOThunderboltSwitch)
public:
    uint64_t getRouteString();
};

/* -------------------------------------------------------------- local node */

/*
 * Base class for a *local* protocol driver (advertises this host's protocol
 * directory). AppleThunderboltIPService is bound to IOThunderboltLocalNode.
 */
class IOThunderboltProtocolListener : public OSObject
{
    OSDeclareDefaultStructors(IOThunderboltProtocolListener)
public:
    static IOThunderboltProtocolListener *withController(IOThunderboltController *controller);
    bool initWithController(IOThunderboltController *controller);
    void getProtocolUUID(uint8_t uuid[16]);
    void setProtocolUUID(const uint8_t uuid[16]);
};

class IOThunderboltLocalNode : public IOService
{
    OSDeclareDefaultStructors(IOThunderboltLocalNode)
public:
    IOThunderboltController *getController();
    void getDomainUUID(uint8_t uuid[16]);

    bool publishXDService(char *serviceKey,
                          uint32_t protocolID,
                          uint32_t protocolVersion,
                          uint32_t protocolRevision,
                          uint32_t protocolInstance);
    bool publishXDServiceDictionary(char *serviceKey, OSDictionary *properties);
    void unpublishXDService(char *serviceKey);
};

#endif /* IOTHUNDERBOLT_FAMILY_H */
