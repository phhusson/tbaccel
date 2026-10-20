# Writing a Thunderbolt/USB4 protocol driver for macOS

How to write a Thunderbolt *protocol* (XDomain service) driver on macOS, using
the stack on this machine (macOS 26.6, Apple M4, `IOThunderboltFamily` 9.3.3) as
the reference, and finishing with the design of the `tbaccel` driver in
`macos/` for ANE/Metal coop over Thunderbolt.

> **How this was verified.** Apple ships no headers for the inter-domain API.
> Everything here was reconstructed from the KDK
> (`KDK_26.6.2_25G83.kdk`): the shipped `IOThunderboltFamily` and
> `AppleThunderboltIP` binaries were disassembled (`nm`, `objdump --macho
> --disassemble`) and the shipping kext `Info.plist`s were read. Raw symbol dumps
> live in `macos/reference/`. Claims marked **(verified)** were read directly out
> of the disassembly.

---

## 1. The stack

```
     NHI (host controller, PCIe)  ── AppleThunderboltNHI
              │  publishes
     IOThunderboltController         getLocalNode(), getNHIPort()
        │              │
        │              └── IOThunderboltXDomainLink   (one per connected peer;
        │                    ROM read, property decode, path mgmt)
        v
     IOThunderboltLocalNode          (this host's inter-domain view)
        │  advertise local protocol directories
        v
     (peer side) IOThunderboltXDomainService  (one per protocol directory in the
        │                                       peer ROM; the provider drivers
        │                                       bind to)
        v
     YOUR PROTOCOL DRIVER = an IOService subclass, IOProviderClass =
                            IOThunderboltXDomainService
        │
        ├── IOThunderboltTransmitQueue / IOThunderboltReceiveQueue  (NHI rings)
        ├── IOThunderboltTransmitCommand / IOThunderboltReceiveCommand (frames)
        └── IOThunderboltPath  (fabric path, credits)
```

### The one thing people get wrong

A protocol driver is **not** a subclass of `IOThunderboltXDomainServiceClient`.
**(verified)** `AppleThunderboltIPTransmitter`'s constructor calls
`IOService::IOService(OSMetaClass const *)`, and its `MetaClass` records
`sizeof = 0x168`. It is a **direct `IOService` subclass** that binds to the
`IOThunderboltXDomainService` provider and drives the family's queues, paths and
commands directly. (`IOThunderboltXDomainServiceClient`, `sizeof = 0x218`, is the
family's *internal* machinery; a driver only ever sees it through
`OSDynamicCast`.)

This is good news: you do not need to reproduce an opaque base class layout.

---

## 2. Discovery: property directories

Each peer advertises protocol **directories** in its ROM. macOS maps a directory
to an `IOThunderboltXDomainService` with these IOKit properties:

| IOKit property | ROM element | Linux name |
|---|---|---|
| `Service Key` | directory name | property dir name, e.g. `accel` |
| `Protocol ID` | `prtcid` | `TB_SERVICE_ID_*` |
| `Protocol Version` | `prtcvers` | `TB_SERVICE_VERS_*` |

A protocol driver matches with a personality:

```xml
<key>IOClass</key>            <string>TbAccelClient</string>
<key>IOProviderClass</key>    <string>IOThunderboltXDomainService</string>
<key>IOPropertyMatch</key>
<dict>
    <key>Service Key</key>      <string>accel</string>
    <key>Protocol Version</key> <integer>1</integer>
</dict>
```

`AppleThunderboltRDMA` uses `"Protocol ID" = 64087` instead. The local side
advertises the mirror image from `IOThunderboltLocalNode`:

```
IOThunderboltLocalNode::publishXDService(char *name, u32 id, u32 vers,
                                         u32 revs, u32 stns);
```

Because the wire elements are the same ones Linux parses, a Linux `tbaccel` host
and this macOS driver discover each other.

---

## 3. A protocol driver's lifecycle

1. `start(IOService *provider)`:
   ```cpp
   fService = OSDynamicCast(IOThunderboltXDomainService, provider);
   fLink    = fService->getXDomainLink();
   fController = fService->getController();
   ```
   Grab the work loop and create an `IOCommandGate` for your serialized state.
2. **Create paths.** `IOThunderboltPath::withController(fController)`
   **(verified from `AppleThunderboltIPTransmitter::createTxPath`)**. Set credits
   with `setSourceInitialCredits` / `setDestinationInitialCredits`
   **(verified)**. IP then configures source/destination ports and HopID ranges
   through virtual methods on the `IOThunderboltPort` objects obtained from the
   link, and finally `activateSynchronous()`. *This port/HopID step is the part
   still to be pinned down precisely (see §7).*
3. **Create queues.** `IOThunderboltTransmitQueue::withControllerAndWorkLoop` /
   `IOThunderboltReceiveQueue::...` **(verified)**; `setRingSize`,
   `setMaxFrameSize`, `setQueueType`, `allocate()`, `start()`.
4. **Move frames** with `IOThunderboltTransmitCommand` /
   `IOThunderboltReceiveCommand`. Each carries an `IOMemoryDescriptor`, an
   offset/length, SOF/EOF bytes and a completion.
5. `registerService(0)` to publish yourself to user space.

### The completion type (verified)

`setCompletion` takes a 24-byte value type whose layout is
`{ void *target; void (*fn)(void*,void*,IOReturn,Cmd*); void *refcon; }`.
`IOThunderboltTransmitCommand::complete()` invokes it as

```
fn(target, refcon, status, cmd);
```

`IOThunderboltReceiveCommand` is identical. (Apple's own
`AppleThunderboltIPTransmitter::txCommandCallback` is called this way — which is
why its source signature looks like it only has three arguments.)

---

## 4. Control path vs data path

Thunderbolt gives a service a small reliable **control path** and bulk **data
paths**. `AppleThunderboltIPTransmitter` actually carries its protocol (login,
logout, packets) as ordinary **frames on its own queues**, not through a
separate control API. `tbaccel` does the same, and it is exactly what the Linux
driver does with its four rings (control tx/rx, data tx/rx). This is why the
macOS port maps cleanly onto the Linux one.

---

## 5. Buffers and synchronization for ANE/Metal coop

The target is ANE + Metal coop over the link (`ggml/src/ggml-ane`), so the
natural primitives are **IOSurface** and **MTLSharedEvent**:

* The ANE backend already allocates every buffer as an `IOSurface` (`IOSurfaceCreate`
  with `kIOSurfaceWidth/Height/BytesPerRow/AllocSize`), wraps it in an
  `MTLBuffer` (`newBufferWithBytesNoCopy:...StorageModeShared`), and syncs with
  `MTLSharedEvent` signaled values (`encodeSignalEvent:`, `encodeWaitForEvent:`)
  plus the private `_ANESharedWaitEvent` / `_ANESharedSignalEvent` /
  `_ANESharedEvents` objects over `IOSurfaceSharedEvent`.

So the driver exposes:

| Linux tbaccel | macOS driver |
|---|---|
| `dma-buf` import | `kTbAccelMethodImportSurface` — user passes the task-mapped IOSurface base address + size; the kernel builds an `IOMemoryDescriptor` over those (already wired) pages. `kTbAccelMethodImport` still creates a kernel buffer for tests. |
| `dma-fence` / `sync_file` | a **tbaccel event** (`kTbAccelMethodEvent`) mirroring `MTLSharedEvent`: `CREATE` / `SIGNAL(value)` / `WAIT(value,timeout)` / `DESTROY`. `WAIT` blocks on a `TbAccelEvent` inside the command gate and is woken by `SIGNAL`. |
| region data → zero copy | receive frames are posted with the surface's `IOMemoryDescriptor`, so bytes land straight in the IOSurface (which is also the `MTLBuffer`), never copied. |

The intended glue in `ggml-ane` is then: `ane_buffer_alloc` already hands out
surfaces; export the surface base/size to the tbaccel user client, and replace
the local `MTLSharedEvent` signal/wait with `tbaccel_event` SIGNAL/WAIT when the
peer is remote. The existing `ggml_apple_fence` abstraction (event + value) is the
right place to swap in the tbaccel event.

---

## 6. Wire protocol (identical to Linux)

```
ARM{slot,len}          ->  arm ceil(len/4K) frames from the slot region
<-- ACK{slot,armed,total}
stream armed bytes
CREDIT{bytes}          <-  receiver re-arms as frames drain
                       ->  DONE{slot} + opaque op blob
```

Constants: magic `"TBAC"`, types `ARM 0x10 / ACK 0x11 / DONE 0x12 / BLOB 0x13 /
CREDIT 0x14`, window = `ring_size-2` frames. On top of this the `TBX1` control
protocol (`PREPARE/PREPARED/LOAD/MATMUL/RESULT`) drives the reference matmul.

---

## 7. What is still unknown (and how to close it)

* **Fabric path configuration.** `configureTxPath` sets source/destination ports
  and HopID ranges via virtual calls on `IOThunderboltPort`, then activates.
  Those methods aren't in the exported symbol table under plain names, so they
  must be recovered from the disassembly of `configureTxPath` (indirect call
  targets) or by tracing a live IP link. Until then `setupPaths()` creates the
  path and sets credits but does not activate it.
* **Command construction.** IP subclasses `IOThunderboltTransmitCommand`
  (`AppleThunderboltIPTransmitCommand::withControllerAndQueue`). The base class
  exposes only `initWithControllerAndQueue`; if it turns out to be abstract, the
  macOS driver needs an equivalent thin subclass.
* **Return types** are inferred (mangling does not encode them).

Everything else — binding, queue creation, frame submission, the completion ABI,
the control/data protocol, buffers and events — is in place and compiles.

---

## 8. Build, sign, load

```sh
cd macos
make                 # build/TbAccel.kext, arm64e
make user            # build/tbaccel (user-space exerciser)
scripts/load.sh      # sudo kextutil -l (needs boot security to allow a kext)
scripts/unload.sh
```

On Apple Silicon a third-party kext requires the boot security policy lowered to
*Reduced* or *Permissive* (`bputil`, which needs a reboot into recovery). SIP is
already disabled on this machine. Loading prints `TbAccel: ...` lines to the
kernel log (`log stream --predicate 'sender == "TbAccel"'`).

---

## 27.0.1 (build 26A434) bring-up log — status & findings

Everything below was verified on an M4 Mac mini (macOS 27.0.1, KDK 27.0.1_26A434),
SIP disabled, security mode Permissive, 3rd-party kexts enabled.

### Working
- Kext builds (Xcode target), loads from the AuxKC, both drivers `start()`.
- `IOThunderboltLocalNode::publishXDService("accel",1,1,0,0)` (verified from
  disassembly: builds an `IOThunderboltXDDirectory` with `prtcid/prtcvers/
  prtcrevs/prtcstns`) makes the LocalNode advertise `accel [uuid]`, which makes
  the Linux peer probe and bring up `tbaccel0` + `tb_xdomain_enable_paths()`.
- `TbAccelClient` binds the peer's `accel` `IOThunderboltXDomainService`
  (Service UUID `6B6A6F21-9C1E-4F2A-...`, Linux tbaccel's dir uuid).
- `IOThunderboltPath::withController` + `set{Source,Destination}{Port,HopIDRange}`
  + `allocate()` + `activate()` all exist and are callable.

### Blocked: NHI transmit ring allocation
`IOThunderboltTransmitQueue::allocate()` returns false for us, on every attempt,
while AppleThunderboltIP gets a ring on the same controller.

Decoded from `AppleThunderboltNHITransmitRingManager::allocateTransmitRing`:
- branches on `queue->getFlags()`: `bit0` -> 0x52f34 (frame path; calls
  `ring->allocate(true)`, `ring->setOwner(queue)`, `ring->configure()`),
  `bit1` -> 0x530b4 (trace path; needs EDT key `sat-dtf-trace-ring-index` —
  this path creates a ring then fails for us),
  neither -> 0x52e6c (default: `cbz [ringmgr+0x20]` = free-ring mask; scande
  slots).
- `AppleThunderboltNHITransmitRing::start()` logs `not ready` when
  `[ring+0x18]` (NHI/soft-irq) is null or its `vtable+0xb80` returns false.

Decoded from `AppleThunderboltIPTransmitter::createTxPath` (queue at `[this+0xc8]`):
`setFlags(4)`, `setQueueType(2)`, `setRingSize(n)`, `setMaxFrameSize(4096)`,
`enableE2EFlowControl(e2e)`, `allocate()`, `start()` — reproduced exactly,
still no ring.

Attempts made (all failed at `allocate`): flags {0,1,2,3,4}; factories
{withController, withControllerAndWorkLoop}; ring sizes {64,4096};
setQueueType(0/2); enableE2EFlowControl; path ports (NHI port);
1s/2s retry up to 120 attempts; disabling Thunderbolt Bridge to free IP's ring.
The NHI is entered (162x/min) but creates no ring for us (`new ring` only ever
logged once — Apple's IP). It allocated a ring successfully exactly once, early
in the session, before IP had taken its ring.

### Not yet tried
- Family-internal queue/path API: `IOThunderboltXDomainServiceClient::withXDomainService`
  + `allocateXDomainPath`/`allocateXDomainQueue`/`createTxQueueInternal`
  (`Allocated Hop ID %u, Max Frame Size %llu, Ring Size %u`), which allocates the
  hop + ring internally. (Note: not referenced by any Apple kext and not
  instantiated by the family — needs verifying it's callable.)
- Allocating the ring from `TbAccelService` (LocalNode stage, earlier than the
  XDomainService match) to win the race for the ring.

### Approval mechanics (dev box)
Each new CDHash needs approval; the prompt only appears once you're logged in.
`scripts/approve.sh` seeds `/var/db/SystemPolicyConfiguration/UAKL.plist`
(`SignedKernelExtensions[TBACCEL001] += [com.phh.tbaccel, cdhash]`, `BootSession`)
and clears stale `kext_load_history_v3` `pending-upgrade` flags (0x910 -> 0x10).
Both must be done, then reboot, then approve in System Settings if prompted.

### 27.0.2 The ring-allocation blocker was a return-type bug (solved)

For several sessions the NHI appeared to refuse our `IOThunderboltTransmitQueue`
a transmit ring: `IOThunderboltTransmitRingManager::allocateTransmitRing` was
entered on every retry, AppleThunderboltIP held a ring on the same controller,
and every configuration permutation "failed" identically.

Root cause: a **return-type mismatch in the reconstructed family header**.
`IOThunderboltTransmitQueue::allocate()`, `::start()`, `::submit()`,
`IOThunderboltReceiveQueue::{start,submit}()` and `IOThunderboltPath::allocate()`
all return **IOReturn, where 0 is success**. Declaring them `bool` and testing
`if (!q->allocate())` therefore treats *success* as failure.

Evidence from the binaries (KDK 27.0.1):
- `IOThunderboltTransmitQueue::start()` ends `mov w20, #0` ... `mov x0, x20` /
  `retab` -> returns 0 on success.
- `IOThunderboltTransmitQueue::allocate()` returns 0 on success; the caller in
  `IOThunderboltXDomainServiceClient::allocateTxQueueInternal` does `cbz w0`.
- `submit()` sets `0xe00002c2` as its error default -> IOReturn.
- `IOThunderboltPath::allocate()` builds errors as `0xe00002bc + N` and returns
  0 when `createResources()` (`vt[0x470]`) returns 0.
- Counter-example: `init*` methods really are bool - Apple branches on
  `tbz w0, #0` after `initWithControllerAndQueue*` (seen in AppleThunderboltIP).
- The setters (`setFlags`/`setQueueType`/`setRingSize`/`setMaxFrameSize`/
  `enableE2EFlowControl`) are non-virtual in our header, so they compile to
  direct calls to the real symbols (verified in the bind table) - always correct.

Fix: declare those as `uint32_t` and test `!= 0`.

Result after the fix: `TxAlloc=Yes`, `TxStart=Yes` (rc 0), `RxAlloc=Yes`,
`RxStart=Yes` (rc 0), `TbAccelTxHop=hop2`, `TbAccelRxHop=hop1`,
`TbAccelTry=1` (no retries), client reaches `TbAccelClientStep=up`.

Useful ring-manager facts confirmed while diagnosing:
- `AppleThunderboltNHITransmitRingManager::allocateTransmitRing` branches on
  `queue->getFlags()`: bit0 -> "frame" path using **ring index 0 only**;
  bit1 -> "trace" path requiring EDT key `sat-dtf-trace-ring-index`; otherwise
  a default scan over `NHI::getEnabledTxRingMask()`.
- `AppleThunderboltNHI::getEnabledTxRingMask()` (base, used by Type5) returns
  `-0x100` = `0xFFFFFFFFFFFFFF00`, i.e. **rings 0-7 are never allocatable**;
  only `AppleThunderboltNHIGenericACIO` overrides it (dynamic field at +0xe8).
- Probe results: `flags` 0, 4, 8, 0x10, 0x100 -> success (hop 2/3);
  `flags=1` and `flags=2` -> `0xe00002be`.

### 27.0.3 Remaining: IOThunderboltPath allocation and peer hop exhaustion

`makePath()` now fails with `rc=0xe00002f0` from
`IOThunderboltPath::allocate()` -> `createResources()`, which builds an
`IOThunderboltConfigReadCommand` and `IOThunderboltConfigWriteCommand` (two
256-byte buffers) and performs the real fabric path setup with the peer.

Simultaneously the Linux peer reports `failed to allocate Rx HopID`, which is
`tb_xdomain_alloc_in_hopid()` failing on `ida_alloc_range(0, local_max_hopid)`.
The advertised `maxhopid` is **15** (a 16-entry hop space), so the peer's hop-id
IDA is easy to exhaust. Earlier builds retried path setup 43-120 times, each
attempt reserving hops on the peer that were never released - a likely cause.

So: our path failure may simply be a consequence of the peer being exhausted.
Reset the peer's Thunderbolt state (reboot, or reload the modules) and let our
(single-attempt) build try once more before chasing the hop-range semantics.

### 27.0.4 Frames do not cross; IOThunderboltPath is the blocker

State after the return-type fix (build 42bf5d73):

- Rings: `TxAlloc/TxStart/RxAlloc/RxStart = Yes`, all rc 0, hops both `hop2`.
- Wire counters (added to the driver): `TbAccelTxStat = "sub0 done0 ..."`,
  `TbAccelRxStat = "all0 ok0 bad0 ..."` after both a Mac->Linux ARM and a
  Linux->Mac ARM (slot id valid on both ends). **Nothing crosses in either
  direction.**
- `IOThunderboltPath::allocate()` -> `createResources()` fails with
  `0xe00002f0` (`kIOReturnInvalid`) for *every* (source,destination) hop pair
  tried: `{2,9}`, `{2,2}`. So the hop values are not the variable.
- There are **zero** `IOThunderboltPath` instances in the registry
  (`IOThunderboltXDomainTransmitPath`/`ReceivePath` zone counts are 0 too), so
  the NHI never gets the hop->ring route installed.

Peer facts (confirmed by the user's instrumentation in linux/tbaccel.c):

```
paths rc=0, out_hopid=9 tx_ring_hop=2 in_hopid=9 rx_ring_hop=2
```

i.e. one hop pair (9) for the XDomain path in both directions, and both DMA
rings on hop 2. Linux's `tb_xdomain_enable_paths()` returns 0, which means the
Mac's family answered that request - but without creating a path object.

Wire format was verified identical (magic 0x54424143, ARM 0x10, PDF 1, same
packed struct), and Linux's ARM handler ACKs unconditionally when the slot is
found, so the failure is fabric routing, not framing or slot ids.

Also noted: our side defines `TBACCEL_MSG_DONE 0x12` and `opSubmit()` sends it,
but Linux has no DONE case (ARM/ACK/CREDIT/BLOB only). Needs reconciling once
traffic flows.

Conclusion: driving `IOThunderboltPath` by hand is a dead end without more RE
(the failure is generic), whereas Apple's own XDomain clients go through
`IOThunderboltXDomainServiceClient`, which owns path setup and binds queues to
paths (`registerTransmitQueue`/`getTxQueueForPathID`). That is the route to
take next.

### 27.0.5 PANIC: never call the client's per-path getters when no path exists

Build bf22ce99 called `probeXdc()` from `start()`, which swept
`IOThunderboltXDomainServiceClient::getXDomainPathForPathID()` /
`getTxQueueForPathID()` / `getRxQueueForPathID()` for ids 0..15. Panic log:

```
Kernel data abort. at pc 0xfffffe0057031060 (inside IOThunderboltFamily)
  far: 0x0000000000000000                      <- NULL dereference
Panicked task ... pid 862: tbaccel             <- our userspace tool
Kernel Extensions in backtrace:
  com.apple.iokit.IOThunderboltFamily(9.3.3)
  com.phh.tbaccel(1.0)[F4709150-...]
```

With no path table populated (no path exists until the peer's setup request has
been processed) these getters dereference NULL inside the family. Called from
`start()` this boot-loops the machine whenever the peer is present - that was
the "10x Thunderbolt up and down" seen on the Linux side (each panic rebooted
the Mac and re-trained the link).

Rules:
- Never call those getters unless a path is known to exist.
- Never call them from `start()`.
- `withXDomainService()` itself is safe (build b7c04bd7 has it in `start()` and
  does not boot-loop).

Safe, userland-only way to observe family state (no kext calls, no reboot):

```
ioreg -l -w0 | grep -oE '"IOThunderboltXDomain(Transmit|Receive)Path"=[0-9]+|"IOThunderboltXDomainServiceClient"=[0-9]+'
```

### 27.0.6 PANIC 2: client attach()/clientReady() are also unusable from outside

With `tbaccel_xdc=1` the driver additionally called
`IOThunderboltXDomainServiceClient::attach(fService)` then `clientReady(true)`
from `start()`. Result: boot loop again. Panic decoded from the registers:

```
panicString: Kernel data abort, pc 0xfffffe003b09b840 (inside IOThunderboltFamily)
  far: 0x0                      <- NULL dereference
  x6:  0x6863617474416364       -> ASCII "dcAttach"
  lr:  0xfffffe003e7e599c       <- in com.phh.tbaccel (us calling the family)
Panicked task: pid 0: kernel_task
Kernel Extensions in backtrace: IOThunderboltFamily, com.phh.tbaccel
```

So `attach()` is the second unsafe entry point (`clientReady()` was never
reached). Together with 27.0.5 the conclusion is:

**`IOThunderboltXDomainServiceClient` can only be driven by the family's own
`IOThunderboltXDomainServiceClientManager` - not by a third-party kext.**
Unsafe from us: `getXDomainPathForPathID`, `getTxQueueForPathID`,
`getRxQueueForPathID`, `attach`, `clientReady`.
Safe: `withXDomainService()` alone (stable for 17+ minutes, StartCount=1).

Guard rails now in place:
- all of the above removed from the driver (verified by symbol inspection);
- anything experimental is opt-in behind boot-arg `tbaccel_xdc` (default off);
- the driver never calls the userspace client itself.

Recovery if it ever loops again: unplug Thunderbolt (no XDomain service =>
`start()` never runs => clean boot), then `sudo nvram -d boot-args`.

Next safe question: does our own TX ring drain at all? A one-shot control frame
of an unrecognised type (0x7f, ignored by Linux's rx_control switch) is sent at
bring-up purely so the new `TbAccelTxStat` done-counter answers that.

### 27.0.7 Path creation is a phase, not a knob (the `0xe00002f0` saga)

`IOThunderboltPath::allocate()` returned `0xe00002f0` (`kIOReturnInvalid`) for
every hop pair tried (`{9,9}`, `{9,2}`, `{2,2}`), with no path object ever
created. Everything that looked like a missing input was ruled out by
experiment: ring setup (both queues `rc=0`, TX drains `sub1 done1`), queue
flags/type (identical to `AppleThunderboltIPTransmitter::createTxPath`),
reserved hop ids, and finally `IOThunderboltAbstractPath::setInitialCredits`
(the single credit setter IP actually drives - `min(window, entries) - 2`).

The answer was in the *ordering*, which Linux's `thunderbolt-net` makes
explicit (`drivers/net/thunderbolt/main.c`):

- rings are created at probe/init;
- a **login exchange** establishes the session, and the hop ids
  (`local_transmit_path`, `remote_transmit_path`) are a product of it, not
  something either side picks unilaterally (`tb_xdomain_release_in_hopid(xd,
  net->remote_transmit_path)` on teardown is derived from the login packet);
- `tbnet_connected_work()` enables the paths **last**, so the Rx ring is primed
  before anything can arrive. Teardown is the mirror: disable paths first, then
  stop rings.

Our driver was creating its path inside `setupQueues()`, i.e. in the phase
*before* the handshake. The symptom was already in the registry and had been
dismissed as cosmetic:

```
"TbAccelPeerHops" = "peer in65535 out65535"
```

That is the peer's `inhopid`/`outhopid` being absent at that instant - the
handshake had not happened. `createResources()` (config read/write against a
peer that has not established its side) is a plausible source of a generic
`Invalid`, and it explains why every hop/credit variation failed identically:
none of them change the phase.

The driver now mirrors the proven order:

1. `setupQueues()` allocates the TX/RX rings only.
2. `startControlRx()` primes the whole RX ring.
3. `tryPaths()` is a separate phase, re-armed by the retry timer until the peer
   publishes `inhopid`/`outhopid` on its `accel` XDomain service. That property
   arrival is our login handshake.
4. Paths are created with the mirrored values (our out hop -> peer's in hop,
   and the peer's out hop -> our in hop) and only then activated.
5. `teardown()` deactivates paths *before* stopping rings.

Hop pairs are cached per direction and each direction gives up after three
distinct candidates, so a failure cannot burn the peer's 16-entry hop table.
A failure recorded before the handshake is cleared once the handshake happens,
because it says nothing about the post-handshake attempt.

Observability (all in ioreg; kext `os_log` is not captured on this machine):

```
TbAccelPathPhase      waiting-peer | tx-failed | rx-failed | up
TbAccelPathHandshake  true once the peer's hop ids arrive
TbAccelPeerHops       "peer in<u> out<u>"
TbAccelPathLastTry    "<tx|rx> src<u> dst<u> rc=0x........"
```

Because a reboot is expensive, the same build also discriminates the other
grounded hypothesis. `AppleThunderboltIPTransmitter` never calls
`setSourcePort`/`setDestinationPort` (verified: no use of path vtable slots
`0x388`/`0x398` anywhere in the IP binary), while we were binding the NHI port
as **both** endpoints - a path from a port to itself. The path attempt now
tries each hop candidate **portless first**, then with the port binding, and
records the winner:

```
TbAccelTxPathMode / TbAccelRxPathMode   ports | noports
TbAccelPathNoHandshake                  true if the 30 s fallback was used
TbAccelPathPhase                        waiting-peer | tx-failed | rx-failed |
                                        tx-exhausted | rx-exhausted | up
```

So one boot answers both questions: does the handshake become visible, and does
the path need the port binding at all.

First run of the phase gate (build 6073855) also settled the port hypothesis and
exposed a bug in the gate itself.

**Port binding is not the blocker.** Each candidate pair is now tried portless
and then with the NHI port as both endpoints. All six combinations still return
`0xe00002f0`, so a path from a port to itself is merely odd, not fatal, and the
"IP never binds ports" observation does not explain the failure.

**The gate stopped watching.** `tryPaths()` armed the timer only while it still
intended to attempt something, so once the 30 s fallback attempt exhausted its
candidates it went quiet. The registry then showed the peer's handshake had
arrived *afterwards*:

```
IOThunderboltXDomainService (id 4294971232)  Service Key = 'accel'
    inhopid  = 9
    outhopid = 9
```

and `TbAccelClient` (id ...233) was bound to exactly that node - while its
`TbAccelPeerHops` still read `peer in65535 out65535`. Not a property-visibility
problem at all: the client simply never re-read after giving up, so the one
attempt that mattered (post-handshake) was never made.

Fix: the path phase is now a resident watcher.

- It makes at most **one** bounded attempt per handshake (`fPathAttempted`), so
  a failure still cannot burn the peer's hop table.
- A handshake is "the peer's hop ids appeared **or changed**". Hop ids
  disappearing (peer unload) followed by their return is therefore detected as
  a new handshake, which is what makes `rmmod && modprobe tbaccel` on the Linux
  side retriable without rebooting the Mac.
- While it has nothing new to try it polls every 2 s instead of stopping.

Every attempt is now recorded in one property, so a single boot shows the whole
search rather than only its last step:

```
TbAccelPathTries = "tx9/9np:0x e00002f0 tx9/9p:0x... tx9/9..." (space separated)
                   <tag><src>/<dst><np|p>:<rc>
```

### 27.0.8 Decoding `allocate()` from its own log strings

`0xe00002f0` is **`kIOReturnNotFound`**, not `kIOReturnInvalid` as previously
recorded, and the portless variant returns **`kIOReturnBadArgument`**
(`0xe00002c2`) - so the port pair is a required input, not an optional one.
Both codes were recovered from `IOReturn.h`, and both are now logged per
attempt in `TbAccelPathTries`.

The format strings of `IOThunderboltPath::allocate()` name every check it makes:

```
allocate - dst_port %p != fDestinationPort %p
allocate - dst_port->getMaxInHopID = %d
allocate - dst_range.start %d > dst_range.end %d
allocate - (1) allocated input hop id %d for dst port %p dst counter id = %d status = 0x%x
allocate - src_port->getMaxInHopID = %d
allocate - src_range.start %d > src_range.end %d
allocate - allocated input hop id %d for src port %p src counter id = %d status = 0x%x
```

So `allocate()` **allocates an input hop id on each port** from the ranges we
set, bounded by that port's `getMaxInHopID`. It is a local resource allocation
against two ports - not a request to the peer.

`configureFromDescription()` meanwhile owns the whole bring-up, again per its
strings: `description->pathType` / `startroute`, `start_switch`, "can't find
ports", `in_port`/`out_port` + port numbers, "can't allocate in hop id", "can't
allocate NFC", "can't allocate out hop id", `fDestinationPort`,
"can't find child/peer switch", `addPathToActivePaths`. It derives the port pair
from the **route string** (`getNextPortsArray - add port route = 0x%llx port = 0x%x`).

### What AppleThunderboltIP actually calls on a path

Resolving its call sites through the bind table (`refl.py` on `thin/ip`,
`createTxPath` at `0x3db14`) gives the complete list:

| call | value |
|---|---|
| `IOThunderboltPath::withController(ctrl)` | path |
| path vt `0x140` (window) | `min(desired, ring_entries)` |
| `IOThunderboltAbstractPath::setSourceInitialCredits` | `min(desired, ring_entries) - 2` |

with `desired` = 60 when a type query returns 2, else 30. That is **all**.
IP calls neither `setSourcePort`/`setDestinationPort`, nor the hop-id-range
setters, nor `configureFromDescription`, nor `allocate()`.

Conclusion: those entry points belong to the family's own XDomain path manager
(`IOThunderboltXDomainServiceClientManager`), which is exactly the component
27.0.5/27.0.6 established we cannot drive. Hand-building a path at that layer
can therefore never be complete - the hop-id-to-ring mapping that makes frames
reach a ring is established by that manager (or by the equivalent
`IOThunderboltConfigXDomainPathSetupRequestCommand` pair).

Both credit calls are now made (we had only ever made one), restoring the IP
sequence exactly. Hop pairs and port modes are unchanged in behaviour.

### 27.0.9 Where the mapping is actually missing (hop tables)

Reading the family's own `Hop Table` properties out of the registry (`ioreg -a`
+ plist walk, no kext calls) settles what the whole path saga was chasing:

```
bus with the peer:
  our NHI adapter port        HopID=1 -> DstHopID=8  DstPort=1
  peer's "Thunderbolt Port"   HopID=8 -> DstHopID=1  DstPort=7
```

That is the XDomain *control* pair and nothing else - **no hop 9** (the hop both
sides advertise for the data path) and **no hop 2** (the hop Apple's ring
manager assigned our TX/RX rings). On the Linux side
`tb_xdomain_enable_paths(out=9, tx_ring=2, in=9, rx_ring=2)` returns 0 and
installs its own local mapping, but nothing appears on the Mac.

So `allocate()`'s `kIOReturnNotFound` was never about hop numbers or credits: it
is the *absence of a hop table entry*, and only the path manager writes those.
That also explains `0 rx_callback` on Linux and `RxStat all0` here - symmetric,
because neither direction has a route.

Hop ids coinciding (both ends picked 9) is a coincidence, not agreement: Linux
logs `peer provided no HopIDs, using next available`, i.e. our
`publishXDServiceDictionary()` does not reach its cached view of the peer
properties. Harmless at 9/9, but it would silently break distinct per-direction
hops later.

### 27.0.10 Client experiment (build c99791e5)

Per 27.0.6 the component that handles an incoming path request is the family's
per-service client (`processPathSetupRequest -> allocateXDomainPath() +
allocateXDomainQueue()`). Without one, the peer's request is dropped.

This build creates it with `withXDomainService()` only - the single call in that
class proven safe (27.0.6) - and deliberately does **not** set `sUseXdc`, so our
own path attempts still run as a fallback. Killed by `-tbaccel_noxdc 1`.

What to look for after `modprobe tbaccel` on the peer (no Mac reboot needed):

```
ioreg -l -w0 | grep -oE '"IOThunderboltXDomain(Transmit|Receive)Path"=[0-9]+'
ioreg -w0 -l -c IOThunderboltPort | grep -A3 "Hop Table"    # a hop 9 entry?
```

Also new: `TbAccelPropKeys` / `TbAccelPropCount` dump the *keys* of the service
property dictionary we actually read, so the "inhopid is in ioreg but not in
`getProperties()`" anomaly becomes observable rather than inferred
(`<null>` / `<empty>` / a key list).

### 27.0.11 Reversing `IOThunderboltXDomainServiceClient::Callback`

The first attempt declared `Callback` as a function-pointer typedef. That was
wrong in a way worth recording, because it would have failed at *load* rather
than at runtime:

```
ours: __ZN33IOThunderboltXDomainServiceClient25setNewXDomainPathCallbackEPFvPvS0_S0_jE
real: __ZN33IOThunderboltXDomainServiceClient25setNewXDomainPathCallbackENS_8CallbackE
```

Clang mangles a typedef transparently, so a function-pointer typedef yields the
structural form `PFvPvS0_S0_jE`. The real symbol uses a nested-name reference
`NS_8CallbackE`, which means `Callback` is a **type with a name of its own**, not
a typedef alias. The setter confirms it:

```asm
setNewXDomainPathCallback:
    mov  x19, x1
    ...
    ldp  q0, q1, [x19]        ; 32 bytes
    ldr  x8, [x19, #0x20]     ; + one more at +0x20
```

i.e. a **>=0x28-byte struct passed by value** (large aggregates are passed by
hidden pointer, which is why the mangling has no `P`). Its layout is not known,
so fabricating one was not worth the risk.

The notification path itself (from `sendNewXDomainPathCallback`):

```
+0x110 refcon, +0x118 fn      (+0x138 / +0x140 for deactivate)
  x0 = refcon
  cbz x0 -> return 0xe00002ea (kIOReturnNotReady), call NOTHING
  x8 = fn;  call fn(refcon, IOThunderboltXDomainPath *, 0, 0)
  return value discarded (mov w0, #0 right after)
```

So unset callbacks are handled gracefully by the family itself, and we simply
don't set them.

### 27.0.12 `registerService(unsigned)` ignores its argument

Verified: the parameter is copied to `x20` and never read again anywhere in the
function. The body initialises four internal tables (`+0xa0`, `+0xa8`, `+0xb0`,
`+0xb8`). Any value is therefore safe; we pass 0.

Build 4a19d15e creates the client with `withXDomainService()` and calls
`registerService(0)` - no callbacks, no `attach`, no `clientReady`, no getters.
Marker: `TbAccelXdcReg = ok|fail`.

### 27.0.13 `kIOReturnNotFound` was a hop-set collision, not a hop value

The family keeps a per-port `IOThunderboltHopSet` with:

```
allocateHopIDInRangeForPath(IOThunderboltHopIDRange range,
                            IOThunderboltPath *path, unsigned short *hopID)
allocateHopIDInRangeForPathWithCounter(... , unsigned short *hop,
                                       unsigned short *counter, unsigned short)
getPathForHopID(unsigned short, IOThunderboltPath **)
deallocateHopIDForPath(IOThunderboltPath *)
serialize(OSSerialize*)          <- this is what ioreg shows as "Hop Table"
```

So `allocate()` takes a hop id **out of the requested range on the source port**
and another out of the destination range **on the destination port**. Every
attempt so far bound *both* endpoints to `getNHIPort()` and both ranges to
`{9,9}`: the first allocation claims hop 9 on that port, the second asks for hop
9 on the same port and finds it taken, so `allocateHopIDInRangeForPath()` returns
`kIOReturnNotFound` (`0xe00002f0`). That is why `{9,9}`, `{9,2}` and `{2,2}` all
failed identically - the value never mattered, the port pair did. The portless
form fails earlier with `kIOReturnBadArgument`.

The peer's port is reachable without any family accessor:

```
IOThunderboltXDomainService  (an IOService, we already bind to it)
  -> getParentEntry(gIOServicePlane)   = IOThunderboltXDomainLink
  -> getParentEntry(gIOServicePlane)   = IOThunderboltPort (remote switch)
```

Both calls go through `IORegistryEntry`'s own virtual, so they are safe for
objects we have no class definition for, and no assumption about the link's exact
superclass is needed.

Build 08d80de1 therefore tries the port pair `(NHI, peer port)` first, with the
old `(NHI, NHI)` as a fallback. New marker: `TbAccelPeerPort = true|false`.

### 27.0.14 PANIC 3: constructing IOThunderboltXDomainPath ourselves

Attempt: build the receive queue the way the family does -
`OSTypeAlloc(IOThunderboltXDomainPath)` + `initWithXDomainLink(fLink)` +
`setServicePathID` + `allocate()` + `activate()` + `registerXDomainPath()` +
`allocateRxQueueInternal()`.

Result: panic.

```
panicString: PAC failure from kernel with IA key while branching to x8
  x8:  0xaba5fe00417da74c      <- pointer that will not authenticate
  x17: 0x0000000000000470      <- vtable offset (0x470 = createResources)
  x21: 0x00000000e00002bc      <- kIOReturnError in flight
  pc:  inside IOThunderboltFamily
Kernel Extensions in backtrace: IOThunderboltFamily, com.phh.tbaccel
```

Two facts worth keeping:

1. `IOThunderboltXDomainPath::withXDomainLink()` is a **stub** - its whole body
   is logging plus `mov x0, #0; ret`. It never allocates. So a NULL from it says
   nothing about state or permission (27.0.10's "the family refuses" reading was
   wrong).
2. Constructing the object directly is not equivalent to the family constructing
   it. The vtable dispatch at `+0x470` (`createResources`) does not authenticate
   for an object built this way, i.e. the family's own creation path leaves state
   that `initWithXDomainLink` alone does not establish.

Conclusion: the family-built receive queue is closed to a third-party kext. The
receive side needs either the family's own request flow (unreachable) or direct
hop-table writes (not attempted). `tryXDomainRx()` is kept in the source but is
not called; build f0383694 is back to the last known-good configuration
(hand-built rings, TX working).

### 27.0.15 The receive path, solved

The receive half turned out not to need the family's request flow, direct
hop-table writes, or an `IOThunderboltXDomainPath` at all. It needed the same
`IOThunderboltPath` the transmit side uses, configured the way
`AppleThunderboltIPConnection` configures *its* receive path, and activated
**synchronously**.

#### Hop sets are per direction, not per port

`IOThunderboltPort` carries two independent hop sets:

```
getInHopSet()   +0xd8   -> allocation for a path's *destination* port
getOutHopSet()  +0xe8   -> allocation for a path's *source* port, and the set
                           published in ioreg as "Hop Table"
```

so a port can legitimately have the same hop number in both. That is what makes
the symmetric layout work, and it retires the "one port cannot be both source
and destination" reading of 27.0.13 - that failure was two hops colliding within
one direction's set, not the port being reused.

Working layout on the Mac (all four entries present):

```
'Thunderbolt Native Host Interface Adapter' portnum=7      (OUT set = TX rings)
   Hop 1 -> DstHop 8 DstPort 1     control TX   -> wire hop 8
   Hop 2 -> DstHop 9 DstPort 1     data TX      -> wire hop 9
'Thunderbolt Port' portnum=1                                (OUT set = RX sources)
   Hop 8 -> DstHop 2 DstPort 7     control RX: wire hop 8 -> NHI hop 2
   Hop 9 -> DstHop 1 DstPort 7     data RX:    wire hop 9 -> NHI hop 1
```

The control entries are the family's own and were always there; the data entries
are ours. Note that "Hop Table" in ioreg is the **out** hop set, and that each
entry's `DstHop`/`DstPort` come from the path that installed it.

#### What AppleThunderboltIP does for RX

`AppleThunderboltIPConnection::processLoginPacket` (`0x636c8`) extracts the
peer's `transmit_path` from its `TBIP_LOGIN` and stores it at `+0x108`.
`configureRxPath` (`0x5efe0`, body `0x5f3dc`) then does:

```
src_port = fXDLink->getProvider()          // our lane adapter, NOT the peer's port
dst_port = fController->getNHIPort()
fRxPath->setSourceHopIDRange({ peer_tx_hop, peer_tx_hop })        // vt+0x3a8
fRxPath->setDestinationHopIDRange({ rx_queue_hop, rx_queue_hop }) // vt+0x3b8
fRxPath->setSourcePort(src_port)                                  // vt+0x388
fRxPath->setDestinationPort(dst_port)                             // vt+0x398
fRxPath->setInitialCredits(w); fRxPath->setDestinationInitialCredits(w - 2)
```

and `AppleThunderboltIPConnection::createRxPath` (`0x5cb4c`) sets the rest of the
path attributes before `allocate()` (`0x5dc94`-`0x5df44`):

```
setPriority(2)  setWeight(2)  setCounterEnable(7)  setDropPacketEnable(0)
setIngressFlowControlEnable(7)  setEgressFlowControlEnable(3)
setIngressSharedBufferingEnable(0)  setEgressSharedBufferingEnable(0)
setNonFlowControlledCredits(0)  setFlags(0)  setRoutingOptions(0x11)
```

(the transmit path uses the same block with `setEgressFlowControlEnable(7)`).
`setCounterEnable` is not cosmetic: without it the hop is allocated with
`Counter = None`, with it the entry gets a counter id like the control path.

#### `activate()` is asynchronous - use `activateSynchronous()`

`IOThunderboltPath::activate()` (`0x142798`) only calls `startNFCActivation()`
(`0x143e64`), which sets the state and tail-calls `beginNextNFCActivation(0)`.
The per-hop activation writes happen later and `activate()` reports nothing about
them. `activateSynchronous()` (`0x141208`) sets the sync flag at `+0x120`, drives
the same chain and blocks on the semaphore at `+0x124`, returning the real
status. IP uses it for both directions (`AppleThunderboltIPTransmitter` at
`0x44718`, and `login` at `0x65d00` right after `configureRxPath()`).

Symptom of getting this wrong is subtle: the hop entry keeps its allocated
counter but loses `DstHop`/`DstPort`, because `IOThunderboltPath::getHopInfo()`
(`0x136ba8`) refuses to report an entry whose path state is not 5:

```
Hop 9 -> DstHop None DstPort None counter=1     // allocated, never activated
Hop 9 -> DstHop 1    DstPort 7    counter=0     // activated: route installed
```

#### Result

With those three things in place (correct port/hop pair, IP's attribute block,
synchronous activation) frames from a Linux `tbaccel.ko` peer land in the Mac's
receive ring, and the ARM/ACK handshake completes over the data rings in both
directions.

### 27.0.16 Pitfalls that cost the most time (staging, not fabric)

These are macOS-side and unrelated to Thunderbolt, but they look exactly like
fabric bugs: the kext keeps running an old build, so every fix appears to be
ignored.

1. **A single undefined symbol silently kills the whole auxiliary collection
   build.** `kmutil load` prints
   `Loading extension(s): com.phh.tbaccel requires a reboot` whether or not the
   collection was rebuilt. The failure is only visible in the log:

   ```
   kmutil log show | grep -E "failed to link|building auxiliary kext collection failed"
   [CollectionBuild] extension com.phh.tbaccel failed to link
   ```

   Our case: `IOThunderboltAbstractPath::getState()` was declared on the abstract
   base, but the family only exports `IOThunderboltPath::getState()`. The
   exception is easy to make, because vtable slots (`vt+0x200`) do not tell you
   which class owns the symbol. When adding a method, take the declaring class
   from `nm ... | c++filt`, never from the vtable slot index.

2. **Approval is per CDHash, so every rebuild needs a fresh approval.**
   `Extension with identifiers ... not approved to load` comes from the *build*
   step, not the load. Bump `CFBundleVersion` per build so the running build is
   identifiable (`kmutil dumpstate`, `ioreg -l -c TbAccelClient`), rather than
   guessing from behaviour.

3. **Verify staging against the pending collection, not the loaded one.**
   `kmutil dumpstate` reports the *booted* collection. What matters for the next
   boot is:

   ```
   /var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC/com.apple.kcgen.instructions.plist
   /var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC/StashedExtensions/<uuid>/TbAccel.kext
   /System/Volumes/Preboot/<uuid>/Library/KCGen/StashedExtensions/<uuid>/TbAccel.kext
   ```

   A successful stage logs `copying stashed build data to preboot`; a link
   failure logs `pre-flight failed, building only existing auxiliary kext
   collection contents` and leaves the previous collection in place.

4. **Do not trust `[ -z "$(ioreg ...)" ]`-style "did the new build load"
   checks.** Compare `TbAccelBuildTag` (compiled from `TBACCEL_BUILD_TAG`) and
   the version from `kmutil dumpstate`.

### 27.0.17 SOLVED: the frame-size cliff was SOF == EOF

Every frame above ~192 bytes failed with a signature that looked like a hardware
size limit: the NHI delivered `frame_size - 256` bytes, then 252, with
`kIOReturnIOError` and no payload at all.  It was not a size limit.  It was the
SOF/EOF codepoints.

A Thunderbolt packet carries at most 256 bytes, so a larger frame is a *packet
sequence* delimited by the start-of-frame and end-of-frame codepoints in the NHI
header.  tbaccel stamped both with codepoint 1 (`TBACCEL_PDF` used for
`frame.sof` and `frame.eof`), so every 256-byte packet was simultaneously the
start and the end of a frame: the hardware could never join packets, and a frame
of N > ~192 bytes could only arrive as fragments.

`AppleThunderboltIP` uses distinct codepoints (`TBIP_PDF_FRAME_START = 1`,
`TBIP_PDF_FRAME_END = 2`), which is why its receive queue is configured with
masks 2 and 12 rather than 2 and 2.

The fix, both ends:

```
SOF codepoint 1 -> mask 1 << 1 = 2        (queue SOF bitmask, command setSOF)
EOF codepoint 2 -> mask 1 << 2 = 4        (queue EOF bitmask, command setEOF)
```

Also fixed by the same change: a 512-byte *control* frame (a blob) now arrives
intact (`TbAccelCtlMax = 536`), where before it was cut like data frames - the
"data frames fail, control frames work" asymmetry only ever held because control
frames are small.

Measured effect (64x64x64 f32, 16 KB per tensor):

| configuration | round trip | compute |
|---|---|---|
| 64-byte frames (single packet) | 12.6 ms | 0.37 ms |
| 192-byte frames | 2.1 ms | 0.37 ms |
| 1024/2048-byte requests, 3840-byte results | **0.96 ms** | 0.37 ms |

Two related details worth keeping:

* The sender must transmit frames no larger than the receiver arms for: Linux's
  `tbaccel_arm_one()` arms descriptors of `rx_chunk` bytes and advances
  `armed_off` by that amount regardless of the frame it just received, so a
  sender chunk that differs from the receiver's `rx_chunk` desynchronises the
  offsets even when the frames themselves are accepted.  `rx_chunk` is therefore
  a separate module parameter from `chunk`.
* 4096-byte frames still split, because the wire limit is 4092: `3844 + 248`.
  Use 3840 for full-size frames.

### 27.0.18 UserClient: input + output must stay within a page

`recv_ctrl` always failed with `kIOReturnBadArgument` from its size guard, even
though userspace passed a 24-byte `tbaccel_recv_ctrl` and a 4096-byte blob
buffer.  `TbAccelUcArgs` (recorded in `TbAccelUserClient::externalMethod`)
showed the reason:

```
sel6 in24 out0          <- what the kernel received
```

The kernel reports `structureOutputSize` as **0** when the structure input and
output together exceed a page (24 + 4096 = 4120).  Every other selector was
under that: `get_info` is output-only (24), `import`/`region`/`submit` are
small, so only `recv_ctrl` - the one call with a large output - was affected,
which is why it looked like a marshalling problem rather than a size limit.

Fix: keep the receive buffer modest (1024 bytes is far more than the 48-byte
TBX1 messages need).  With that the same call reports `in24 out1048` and blocks
in `opRecvCtrl` as intended.

Worth remembering for the event/IOSurface work: anything that returns a large
structure through `externalMethod` will hit this, so large payloads belong in
mapped memory (`clientMemoryForType`) rather than in the structure output.

### 27.0.19 A user-space peer over the same ABI

The kext can act as the compute peer in-kernel, or hand the work to a
user-space process over `TbAccelUserClient` (the macOS analogue of
`/dev/tbaccelN`).  `macos/User/tbaccel_peer.c` is the reference: it imports its
own buffers, maps them back, posts receive slots, and answers TBX1 requests.

```
peer: PREPARE id=1 size=16384 → PREPARED slot=3
peer: LOAD id=1 (ok)
peer: MATMUL w=1 y_slot=7 64x64x64 → RESULT -> slot 7
```

Three things had to be right, each of which failed first:

1. **Mode switch.**  `TbAccelUserClient::start()` sets `fUserPeer`; while it is
   set the in-kernel peer stays out of PREPARE (`peerOnBlob`) and completion
   dispatch is routed to `recv_ctrl` instead.  Otherwise both peers answer the
   same request.
2. **One receive mechanism for every slot.**  Data frames land in the
   pre-posted control buffers, not in per-slot commands, so a user-owned slot
   using `armWindow`/`dataRxComplete` never saw a frame (`bad10` and no op).
   Every slot is now assembled by copying out of whichever buffer received it
   (`peerDataChunk`) and a single `deliverOp()` decides in-kernel vs blob.
3. **`recv_ctrl` needs a small buffer.**  See 27.0.18: input + output above a
   page makes the kernel report `structureOutputSize = 0`.

Known rough edges, deliberately left for the event work: blobs are queued even
when the in-kernel peer handles the op, so switching modes can leave stale
PREPAREs for the next user client; and the peer reuses its result buffer as soon
as the next op arrives because there is no outbound-drain fence yet.  Both
disappear once fences become real events rather than slot ids.

### 27.0.20 Imported user memory is not an IOBufferMemoryDescriptor

`ktbAccelMethodImport` allocates kernel memory (an `IOBufferMemoryDescriptor`),
so `getBytesNoCopy()` is valid on it.  `kTbAccelMethodImportSurface` instead
builds `IOMemoryDescriptor::withAddressRange()` over the caller's pages.  The
receive path stored both in the same field and called `getBytesNoCopy()` on it:

```
slot->region->buffer->md->getBytesNoCopy()    // md may be an IOMemoryDescriptor
```

That is a virtual call past the end of the real vtable.  With the kernel-memory
peer it happened to land somewhere harmless; with the Metal peer, whose buffers
are imported by address, the first inbound data frame panicked:

```
PAC failure from kernel with IA key while branching to x8 at pc ...b26c
  peerDataChunk+0x84  <- ctlRxComplete+0x1e8
```

`TbAccelBuffer` now keeps the descriptor as `IOMemoryDescriptor *`, and imported
memory is `prepare(kIODirectionInOut)`ed and mapped into the kernel
(`createMappingInTask`); the mapping is cached as `kva` and released, together
with `complete()`, in `TbAccelBuffer::free()`.  `bufferBytes()` selects `kva`
when present and otherwise `OSDynamicCast`s to `IOBufferMemoryDescriptor` — a
dynamic cast, not a reinterpret cast.  `opFree` also stopped releasing `md`
itself: the buffer object owns it, and releasing twice freed it while `free()`
still used it.

With that, the Metal peer runs the reference workload:

```
peer: PREPARE id=1 -> prepared slot=3
peer: MATMUL w=1 y_slot=7 64x64x64 -> matmul 64x64x64 done (metal)
... 5 iterations ...
TbAccelRxStat = "all50 ok15 bad0 type17 len32 ctlmax72 data7 dfr35"
```
