# Apple Thunderbolt RDMA: the wire protocol

Reverse-engineered documentation of **`AppleThunderboltRDMA.kext`** — Apple's
RDMA-over-Thunderbolt stack — based on the shipping macOS 27 binaries.

> **How this was verified.** Apple ships no headers for any of this. Everything
> below was reconstructed by disassembling the shipping `AppleThunderboltRDMA`
> kext, its `IORDMAFamily` provider, and `IOThunderboltFamily` (KDK 27.0.1,
> arm64e slices), then reading the kext's `Info.plist`. Claims marked
> **(verified)** were read directly out of the disassembly at the cited file
> offsets. Unmarked framing/attribute claims are inferred from the surrounding
> verified code and are called out where they matter. The `names` are Apple's
> own mangled symbols, demangled with `c++filt`.
>
> Reference tooling used lives in `macos/reference/`: `rdma_dis.py` (annotating
> disassembler), `vtsyms.py` (vtable/fixup resolver), `fat4e.py` (arm64e slice
> extractor). The thinned slices are `macos/reference/thin/AppleThunderboltRDMA`
> and `macos/reference/thin/fam` (in both, vaddr == file offset).

---

## 1. What it is

`AppleThunderboltRDMA` is a **kernel Verbs (ibverbs) provider** that carries
InfiniBand semantics over a Thunderbolt/USB4 inter-domain (XDomain) link. It is
the macOS analogue of Linux's `rxe`/`siw`/or Apple's own intended use of
thunderbolt-net-style RDMA, and it is what backs Apple's "RDMA over Thunderbolt"
feature (e.g. TensorFlow/JACCL multi-Mac clusters and kernel-bypass GPU/NIC
staging) on Apple Silicon.

The kext registers an `ib_device` named **`tbt`** with the kernel `IORDMAFamily`,
publishes a single-port RDMA device, and translates the whole ibverbs object
model (device, port, PD, MR, CQ, QP, AH, GID, P_Key) onto Thunderbolt primitives:

* **Control / message plane** — a **GSI QP** (`qp_type == 1`) that carries
  connection-management SAMAD-style datagrams and simple SEND/SEND_WITH_IMM
  traffic as small framed messages over the XDomain "protocol listener" channel.
* **Data plane** — real **queue-pair** traffic (`qp_type == 3`) moved over NHI
  transmit/receive rings (`IOThunderboltTransmitQueue` /
  `IOThunderboltReceiveQueue`) attached to an `IOThunderboltPath`.

### Bundle facts (from `Info.plist`)  **(verified)**

| Key | Value |
|---|---|
| `CFBundleIdentifier` | `com.apple.driver.AppleThunderboltRDMA` |
| `CFBundleVersion` | `0.0.1` |
| `LSMinimumSystemVersion` | `27.0` |
| `IOKitPersonalities` | 3 (below) |
| `OSBundleLibraries` | `IONetworkingFamily` 3.4, `IOPCIFamily` 2.0, **`IORDMAFamily` 1.0**, `IOThunderboltFamily` 9.3.3 |

The three IOKit personalities:

| Personality | `IOClass` | `IOProviderClass` | Match |
|---|---|---|---|
| `AppleThunderboltRDMAPort` | `AppleThunderboltRDMAPort` | `IOThunderboltLocalNode` | — |
| `AppleThunderboltRDMAInterface` | `AppleThunderboltRDMAInterface` | `AppleThunderboltRDMAPort` | — |
| `AppleThunderboltRDMAPeerInterface` | `AppleThunderboltRDMAPeerInterface` | `IOThunderboltXDomainService` | `Protocol ID = 64087` (`0xFA57`), `Protocol Version = 1` |

So the driver is a **stack of three IOServices** anchored on the local
Thunderbolt node, and it binds the *peer* side to an XDomain service whose
protocol directory advertises ID `0xFA57`, version 1.

### The wire UUID

The single protocol identity used everywhere on the message plane is the 16-byte
GUID at file offset `0x7f0`  **(verified)**:

```
memory / IORegistry form : 59600081-1252-42f0-8b7f-12f2ee26bfdb
raw bytes                : 59 60 00 81 12 52 42 f0 8b 7f 12 f2 ee 26 bf db
```

It is only ever referenced from `KernelQP::start` (installed on the protocol
listener) and `sendFrame` (written into each outbound frame); a third reference
is a log-format string. On the wire each 32-bit word of the UUID is
**byte-swapped** individually — `_insertUUIDIntoPacketSwapped(dst,src)` copies
4 words with a per-word `rev` — so the transmitted bytes are:

```
on wire                  : 81 00 60 59 f0 42 52 12 f2 12 7f 8b db bf 26 ee
```

(`_insertUUIDIntoPacket` is the non-swapping variant used elsewhere.)

---

## 2. Layer stack and object graph

```
 linux / peer side                                macOS
 ────────────────                                 ─────
 tbnet / rdma                                    IORDMAFamily  (ibverbs core)
       │                                              ▲
       │                                              │ ib_device "tbt"
       │                                    AppleThunderboltRDMAInterface
       │                                              │ provider
       │                                    AppleThunderboltRDMAPort
       │                                              │ provider
       │                                    IOThunderboltLocalNode
       │                                              │
 Thunderbolt XDomain ROM  ──────────────────►  IOThunderboltXDomainService
 (protocol dir: id 0xFA57, ver 1, key)            (AppleThunderboltRDMAPeerInterface)
                                                       │
                                     ┌─────────────────┴─────────────────┐
                          control/message plane                 data plane
                    IOThunderboltProtocolListener         IOThunderboltPath
                       (GSI QP, qp_type 1)            + IOThunderboltTransmitQueue
                     AppleThunderboltRDMAKernelQP      + IOThunderboltReceiveQueue
                                                        (qp_type 3, QueuePair)
```

Class inventory (per-class symbol listing — ~20 classes; the interesting ones,
with the shorthands used below in parentheses):

* **IOKit driver side** — `AppleThunderboltRDMAPort`,
  `AppleThunderboltRDMAInterface` (also the ibverbs *device* owner; *Interface*),
  `AppleThunderboltRDMAPeerInterface`.
* **ibverbs device side** — `AppleThunderboltRDMAProtectionDomain` (*PD*),
  `AppleThunderboltRDMAMemoryRegion` (*MR*), `AppleThunderboltRDMAKernelCQ`
  (*KernelCQ*), `AppleThunderboltRDMAKernelQP` (*KernelQP*),
  `AppleThunderboltRDMAQueuePair` (*QueuePair*), `VerbsVmRegionRegistration`,
  `UData`, `SAAgent`.
* **transport side** — `AppleThunderboltRDMATransmitQueue` (*TransmitQueue*),
  `AppleThunderboltRDMAReceiveQueue` (*ReceiveQueue*), `TbtFrameReassembler`,
  `DARTErrorHandler`, `DARTMapping`, and the
  `OSValueObject<…{Transmit,Receive}QueueParams>` value wrappers.

There is **no separate IOKit `IOUserClient` subclass** in this kext: userspace
access goes through `IORDMAFamily`'s standard ibverbs `ioctl` surface, which the
kext implements as the callbacks (`allocUcontext`, `create_qp`, `reg_user_mr`,
…).

Two QP flavours exist and the driver dispatches on `qp_type`:

* **`qp_type == 1` (`IB_QPT_GSI`)** → **`AppleThunderboltRDMAKernelQP`**, the
  kernel-only GSI queue that lives entirely on the protocol-listener message
  plane.  **(verified)** `KernelQP::isKernelQP(attr)` is exactly
  `attr->qp_type == 1`; the `create_qp` wrapper (`Interface`, `$15` @ `0x2f680`)
  picks the KernelQP path for type 1 and the `QueuePair` path otherwise.
* **`qp_type == 3` (`IB_QPT_RC`)** → **`QueuePair`**, the NHI-ring data path.
  **(verified)** `QueuePair::isSupported` requires `qp_type == 3`,
  `create_flags == 0`, `max_send_sge < 2`, `max_recv_sge < 2`, and
  `1 <= max_wr < 0x1000`.

> **Only the GSI QP has an ibverbs data path.** **(verified)** The `post_send`
> and `post_recv` `ib_device_ops` wrappers (`$_0` @ `0x2e95c`, `$_1` @ `0x2ead8`)
> dispatch on `isKernelQP`, run the `KernelQP` path only for a kernel/GSI QP, and
> return **`-EOPNOTSUPP` (`-102`)** for a `QueuePair` (data plane). Likewise
> `poll_cq` (`$_2` @ `0x2ec54`) returns `0` (no completions) unless the CQ is a
> `KernelCQ`. So `QueuePair` is created and moved through `INIT/RTR/RTS` **only to
> bring up the NHI rings and publish their mminfo regions**; the actual bulk data
> is driven by user space directly against those mapped regions (kernel bypass) —
> not through ibverbs `post_send`. The kext's own ibverbs SEND/RECV engine is the
> **GSI message plane** described in §5–§6.

---

## 3. Enablement / gating

RDMA is only brought up when explicitly allowed  **(verified)**:

* `shouldEnableRDMA()` (`0x476a8`) returns true if **either**
  * `isDarwinOSBootEnvironment()` — device-tree `IODeviceTree:/chosen` contains
    `darwinos-security-environment` (i.e. a DarwinOS / compute-controller boot),
    **or**
  * `isRDMAEnabledInNVRAM()` — device-tree `IODeviceTree:/options` has key
    `rdma-enable`.
* `isSupportedThunderboltSpeed(speed)` (`0x47428`) returns true if
  `isDarwinOSBootEnvironment()` or `(speed & 2) >> 1` — i.e. it requires
  **Thunderbolt 5** link speed on ordinary macOS. `AppleThunderboltRDMAPort::start`
  reads `Supported Link Speed` and refuses otherwise.

`Interface::start` (`0x2c588`) also requires exactly **one peer per controller**
(log: *"RDMA only supports one peer per Thunderbolt controller like
AppleThunderboltIP"*). It registers for `AppleThunderboltRDMAPeerInterface`
publish/terminate notifications and only reports the port `ACTIVE` once a peer
is up (`Interface::queryPort` sets `ib_port_attr.state = IB_PORT_ACTIVE (4)` iff
`iface+0xa0 & 1`).

---

## 4. Discovery and binding (XDomain)

### 4.1 What the two sides advertise

The peer advertises an XDomain protocol **directory** in its Thunderbolt ROM.
macOS exposes it as an `IOThunderboltXDomainService` with IOKit properties. The
`AppleThunderboltRDMAPeerInterface` matches on  **(verified)**:

```
Protocol ID      = 64087  (0xFA57)
Protocol Version = 1
```

and in `start` (`0x373d4`) reads, via `copyProperty`:

```
Protocol Version    "Protocol Version"
Protocol Revision   "Protocol Revision"
Protocol ID         "Protocol ID"
Protocol Settings   "Protocol Settings"
Service Key         "Service Key"
```

The local side advertises the mirror image from `IOThunderboltLocalNode`. The
`AppleThunderboltRDMAPort` personality attaches to the local node, and the
`Interface` registers the ib device.

### 4.2 Service Key

`AppleThunderboltRDMAPeerInterface::probe` (`0x37144`) calls `shouldEnableRDMA()`
and then compares the first 8 bytes of the `Service Key` property against the
literal at file offset `0x800`  **(verified)**:

```
ff ff ff ff ff ff 41 44        ("......AD")
```

A mismatch logs *"service key failed to match at index %ld '%x' != '%x'"* and
the peer fails to bind. So Apple's RDMA service key is expected to be the
8-byte value with the "AD" suffix shown above (the leading `ff`s are a
wildcard/prefix-matching artifact of the comparison, i.e. the driver compares
against `0x41 0x44` = `"AD"` in the low bytes).

### 4.3 RID and protocol settings

`PeerInterface::start` obtains:

* `getController()` (`IOThunderboltXDomainService` vcall `+0x8a8`) and
  `getXDomainLink()` (`+0x6b0`), stored at `peerInterface+0x90` / `+0x88`;
* the controller's **RID** via `getRID()` (`IOThunderboltController` vcall
  `+0xa78`), then publishes it with `setProperty("RID", value, 8)`;
* `registerService(0)` (`+0x5e0`) to publish to user space.

The `RID` (route/domain id) is used as the RDMA **P_Key / partition** identity
and as a route tag on the message plane, and it appears again in
`Interface::queryPort` (`controller` vcall `+0xa78`). The property names
`"Service Key"`, `"Protocol Version"`, `"Protocol Revision"`, `"Protocol ID"`,
`"Protocol Settings"` and `"Supported Link Speed"` live at file offsets
`0x20f57`, `0x20f63`, `0x20f74`, `0x20f86`, `0x20f92`, `0x20ff3`.

### 4.4 The `AppleThunderboltIPService` string

The kext also contains the literal `"AppleThunderboltIPService"` (offset
`0x80c`) plus `"RID"` (`0x7c0`, `0x808`), `"ADRID"` (`0x806`), and
`"Queue Pair Number"` (`0x826`). These are the property names exchanged with
the peer and pushed into the ib device registry.

---

## 5. The TbtFrame message transport (GSI / control plane)

This is the most protocol-specific part. The GSI QP carries *messages* as
**TbtFrames** over the XDomain "protocol listener" channel.

### 5.1 Frame layout  **(verified)**

`KernelQP::sendFrame(payload, eof)` (`0x357b8`) builds the frame in a 256-byte
`IOBufferMemoryDescriptor` allocated at `QP+0x2c0`:

```
 offset  size  field
 0x00    16    protocol UUID, per-word byte-swapped (§1)
 0x10     4    header:  bit24 = EOF, else 0     (eof ? 0x01000000 : 0)
 0x14     n    payload
```

The memory descriptor length is rounded up to dwords:
`setLength(ceil((0x14 + payload_len) / 4))`. The frame is then shipped by an
`IOThunderboltConfigXDomainResponseCommand` (`QP+0x2c8`).

So the unit on the wire is **`[16-byte UUID][4-byte header][payload]`**, i.e. a
20-byte header + payload.

### 5.2 Outer XDomain packet framing  **(verified)**

`IOThunderboltConfigXDomainResponseCommand::prepareForExecution` (`fam 0x19f94c`)
wraps the descriptor (the TbtFrame) into the family's config-response packet:

```
 offset  size  field
 0x00     4    route
 0x04     4    (reserved / route)
 0x08     4    flags:  (bfi[0xac] << 27) | (2 & 0x3f)   ... SOF/EOF byte = 7
 0x0c     m    descriptor (the TbtFrame: UUID + header + payload)
 0x0c+m   4    CRC32 over [0x00, 0x0c+m)
```

Total packet length = `0x10 + m`. It also sets `SOF/EOF = 7`.

### 5.3 Receive path  **(verified)**

`KernelQP::protocolListenerCallback` (`0x340cc`) is the protocol-listener
completion handler. For an incoming command:

1. `command` vcall `+0x1f0` → the `IOMemoryDescriptor`; vcall `+0x230` → length;
   **it requires `length > 0x1f` (≥ 32 bytes)** with the log *"header+CRC is
   32"*.  vcall `+0x308` → `getBytesNoCopy()` → `buffer`.
2. It builds `OSData::withBytes(buffer + 0x1c, length - 0x20)`.
3. Dispatches (async, on `QP+0xa0`) a block that calls
   `TbtFrameReassembler::append` on that data.
4. When the reassembler reports `done`, calls `KernelQP::deliverMessage`.

Reconciling `0x1c`/`0x20` with the wire layout above: the received packet is

```
 0x00  outer route u32
 0x04  outer u32
 0x08  outer flags u32
 0x0c  UUID[16]
 0x1c  frame header u32     <-- buffer + 0x1c
 0x20  payload ...
 ...   CRC32 (last 4 bytes)
```

`length - 0x20` bytes read from `0x1c` therefore cover **`[0x1c, length-4)`**:
the 4-byte frame header plus the payload, *excluding* the trailing 4-byte CRC.
That exactly matches what the reassembler expects (see below) and the
"header+CRC = 32" (= 0x1c prefix + 4 CRC) minimum-length check.

### 5.4 Reassembly  **(verified)**

`TbtFrameReassembler` sits inline at `KernelQP+0xf8`:

* `buf` @ `+0xf8`, `len` @ `+0x2b0`, `done` @ `+0x2b8`; `buf` capacity 440 bytes.
* `append(src, n)` reads the **first dword** of `src`: bit 24 = EOF.
  * If already accumulating and the new frame's EOF is 0, the accumulator
    resets (a non-final frame mid-message is only valid at a boundary).
  * A **non-final** payload must be **exactly 220 bytes** (`0xdc`); the total
    accumulated message must be **≤ 440 bytes** (`0x1b8`).
  * `memmove` the payload (skipping the 4-byte header) into the accumulator;
    `len += payload_len`; `done = eof`.
* `data()` returns `{buf, len}` iff `len != 0 && done`.
* `reset()` zeroes 440 bytes + `len` + `done`.

So a message is **at most 440 bytes**, and it is split into frames carrying
**220 payload bytes each**; only the last frame sets the EOF bit.

### 5.5 Send path / fragmentation  **(verified)**

`KernelQP::postSend` (`0x3505c`) requires `QP+0x88` and `QP+0x90` non-zero and a
non-NULL work request; `wr->opcode` at `wr+0x1c` must be **2 (SEND)** or
**3 (SEND_WITH_IMM)**. It gathers the SGL (`wr+0x10`, count `wr+0x18`) into a
440-byte staging buffer via `copySGEs`, then:

* If the AH is loopback (`isLoopback`, §7) and `send_flags` bit 1 is set, it
  posts a send completion (opcode 0) to the send CQ (`QP+0xa8`) and calls
  `deliverMessage` directly — pure in-kernel loopback, no wire traffic.
* Otherwise it fragments: `frame1 = min(len, 220)` with
  `eof = (len < 221)`; if `len >= 221`, `frame2 = len - 220` with `eof = 1`, and
  sends each with `sendFrame`. Success status 0; failure err `0x15` (21). Then it
  advances to `wr->next`.

### 5.6 Receive delivery to user SGEs  **(verified)**

`KernelQP::deliverMessage(ref)` (`0x355a4`) requires the recv CQ (`QP+0xb0`) and
`QP+0x90`, and `len <= 0x1b8`. It pops a `RecvSlot` from the ring at `QP+0xe0`
(capacity `QP+0xe8`, head/tail `QP+0xf0`/`QP+0xf4`), zeroes a **40-byte (0x28)
prefix**, copies the message to `scratch + 0x28`, and if `len + 0x28` exceeds the
slot's total length it posts an error completion. Otherwise it scatters via
`copySGEs(slot, num_sge, scratch, len + 0x28, dir=1)` and posts an `ib_wc`:

```
 wr_id      = slot+0x88
 status     = 0
 opcode     = 0x80   (IB_WC_RECV)
 vendor_err = 0
 byte_len   = len + 0x28
 qp         = QP+0x98
 src_qp     = 1
 pkey       = *(u16*)(QP+0xc8)
```

The **40-byte zeroed prefix is the GRH-shaped header** the ibverbs ABI expects
for a received `IBV_WC_RECV` on a RoCE/IB QP: a user's posted receive buffer must
be at least `message_len + 40` bytes. `postRecv` (`0x35af0`) enforces the mirror:
SGL count 1..8, each address non-NULL, total length ≤ 480 (`0x1e0` = 440 + 40),
`RecvSlot = { ib_sge sge[8]; u32 num_sge; u32 total_len; u64 wr_id }` (0x90 B).

`RecvSlot.total_len` and the ≤ 480 cap are the reason messages are capped at
440 bytes on the wire: 440 payload + 40 GRH = 480 fits in any compliant receive
buffer.

### 5.7 QP bring-up  **(verified)**

`KernelQP::start` (`0x33bd8`):

1. `IOThunderboltProtocolListener::withController(QP+0x88)` → `QP+0xd8`;
   `uuid_copy` the protocol GUID → `setProtocolUUID` (vcall `+0x260`, log
   `"setProtocolUUID -> 0x%x"`); register the listener completion block via
   vcall `+0x148` (this is the hook that calls `protocolListenerCallback`).
2. Create the 256-byte send buffer → `QP+0x2c0`, `setLength(256)`.
3. `IOThunderboltConfigXDomainResponseCommand::withController` → `QP+0x2c8`;
   configure its data descriptor/key (vcalls `+0x478`).

`IOThunderboltProtocolListener::setProtocolUUID(u8*)` (`fam 0x2179ac`) copies the
16 bytes into `listener+0x51` under the controller gate; `getProtocolUUID`
(`fam 0x2179f4`) reads it back.

---

## 6. Subnet-manager agent (SAAgent)  **(verified)**

The driver registers a MAD agent on the GSI QP to answer subnet-manager queries
so the kernel ibverbs core can discover the fabric.

`SAAgent::initWithDevice(device, port, u32, local_gid, peer_gid)` (`0x47b5c`)
fills the agent at `+0xc` (port), `+0x10` (local GID), `+0x20` (peer GID), sets
the MAD-agent attribute word to `0x0203`, and calls:

```
ib_register_mad_agent(device, port, qp_type = 1, &attr, flags = 0,
                      send_handler = 0x47ca4, recv_handler = 0x47d14, context = self)
```

storing the handle at `+0x30`. Incoming MADs hit `handleSARecv` (`0x47dc8`) →
`processSAMad` (`0x48050`):

* method byte `1` (Get) → reply method `0x81` (GetResp).
* attribute ID `0x0001` → **ClassPortInfo** (`processSAClassPortInfo`,
  `0x483f0`; writes `[resp+0x38] = 0x14000000000201`, length 256).
* attribute ID `0x0035` → **PathRecord** (`processSAPathRec`, `0x48290`):
  verifies SGID == local GID and DGID == peer GID, packs with
  `ib_sa_pack_path`, status `0x1900`, length 256.
* anything else → `REQ_INVALID`.

This is a deliberately minimal SA: enough for `ibv_devinfo`/`rdma_cm` to bring
the device up and for the CM (connection manager) to exchange QP info.

---

## 7. The data plane (NHI queues + paths)

Connection setup is a classic **INIT → RTR → RTS** transition per QP, but each
transition also allocates and exchanges Thunderbolt **hop IDs** and **DMA
registration** descriptors, because a thunderbolt path is not a plain
loopback — it is a routed fabric path with flow-control credits.

> This section documents **setup**. For what actually executes the data plane
> *after* setup, see §8.

### 7.1 INIT — allocate TX hop ID, bring up RX  **(verified)**

`QueuePair::modifyQPToInit` (`0x3c4fc`):

1. `TransmitQueue::start()`.
2. `getNHIHopID(&u16)` (error `-14` / "TX Hop ID not allocated").
3. Build RX parameters `{ u32 qpnum = *(QP+0x178); OSSharedPtr @ +8 (= *(QP+0x1a8)); u16 hopID @ +0x10 }`.
4. `ReceiveQueue::withTBController(...)` and start the RX queue.

### 7.2 RTR — peer's TX hop ID, return RX registration IDs  **(verified)**

`QueuePair::modifyQPToRtr(resp)` (`0x3c8fc`):

1. Requires TX (`QP+0x180`) / RX (`QP+0x188`).
2. `peerTXDstHopID = *(u16*)(QP+0xb5)` (a `ldurh`, the peer's TX destination hop
   ID, learned during the CM exchange and stored just past destination QP num).
3. `IOSleep(4)`.
4. `ReceiveQueue::allocateAndActivatePath(peerTXDstHopID)`.
5. Zero a 48-byte response and fill it from the **RX queue**:
   `registerRegion(&resp[0])` (mminfo at `+0`) and
   `descriptorBuffer(&resp[0x18])` (mminfo at `+0x18`), i.e.
   `tbt_ibv_modify_qp_ex_resp` = **two 24-byte mminfo records**:
   `{ VerbsVmRegionRegistration registerRegion; descriptorBuffer }`.
6. A `descriptorBuffer` failure returns `-14`; errors log *"failed to fetch
   registerRegion ids (0x%08x, %s)"*.

### 7.3 RTS — return TX registration IDs  **(verified)**

`QueuePair::modifyQPToRts(resp)` (`0x3cb30`): identical, but the two mminfos come
from the **TX queue**. So the RTR response tells the peer where this side's
receive rings live; the RTS response tells it where this side's transmit rings
live.

### 7.4 mminfo / region registration  **(verified)**

`tbt_ibv_modify_qp_ex_resp` = 48 bytes = two 24-byte `mminfo`:

```
 struct mminfo {                 // 24 bytes, from VerbsVmRegionRegistration::mminfo() @ 0x493a0
     u32 regionId;               // from _ib_device_register_vma
     u32 reserved;               // 0
     u64 value;                  // IOMemoryDescriptor::getLength()
     u64 reserved2;              // 0
 };
```

`VerbsVmRegionRegistration` (`create` @ `0x490b8`, `initWithMemoryDescriptor` @
`0x491ec`) holds `IOMemoryDescriptor` at `+0x10`, a doubly-linked list at
`+0x18`/`+0x20`, `length` at `+0x28` (vcall `+0x1f0`), `regionId` at `+0x30`,
`ib_device` at `+0x40`, and an `OSString` name at `+0x48`.

Region IDs are assigned centrally by `_ib_device_register_vma(ib_device*,
region*)` (`IORDMAFamily 0x40988`): it locks `device+0x928`, reads `region+0x18`
(the id slot), and if zero assigns `max+1` (or starts at 1), linking the region
into a sorted list at `device+0x918`. `_ib_device_unregister_vma` (`0x40ad4`)
undoes it. This is the mechanism by which the two peers tell each other which
memory windows they can DMA into.

### 7.5 Ring/path configuration  **(verified)**

Both queues configure an `IOThunderboltPath` obtained from
`IOThunderboltPath::withController(controller)`, using the controller's NHI port
(`controller+0x970`).

**Common path setup** (`ReceiveQueue::withTBController` @ `0x3e590`,
`TransmitQueue::withTBController` @ `0x41bf8`):

```
 setPriority(4)                     (+0x150)
 setCounterEnable(7)                (+0x180)
 setDropPacketEnable(0)             (+0x170)
 setIngressSharedBufferingEnable(0) (+0x1b0)
 setEgressSharedBufferingEnable(0)  (+0x1c0)
 setNonFlowControlledCredits(0)     (+0x1d0)
 setFlags(0)                        (+0x1e0)
 setWeight(1)                       (+0x160)
 E2E credits = min( (linkSpeed==2) ? 60 : 30, NHIport->getMaxCredits() )
 setInitialCredits(credits)         (+0x140)
```

The link-speed test `kIOThunderboltLinkSpeed == 2` selects 60 vs 30 credits —
Thunderbolt 5 (speed 2) doubles the credit window; `getMaxCredits` on the NHI
port (`IOThunderboltPort` vcall `+0xb88`) clamps it.

**ReceiveQueue** additionally:

```
 path setRoutingOptions(1)                        (+0x438)
 path setSourcePort(params+0x10 -> +8)
 path setDestinationPort(NHI port)
 setSourceInitialCredits(credits-2)               (+0x140, source)
 setIngressFlowControlEnable(7)                   (+0x190)
 setEgressFlowControlEnable(3)                    (+0x1a0)
 IOThunderboltReceiveQueue::withController        (+0x... )
   enableRawMode(0)      (+0x1d0)   setFlags(8)        (+0x220)
   setQueueType(2)       (+0x230)   setRingSize(...)   (+0x170)
   setMaxFrameSize(0x1000) (+0x1f0) setSOFPDFBitmask(2) (+0x200)
   setEOFPDFBitmask(0xc) (+0x210)   setTxE2EHopID(hop) (+0x1b0)
   enableE2EFlowControl(1) (+0x1c0) setInterruptStride(32) (+0x248)
   allocate()            (+0x180)
   getHopID(&u16)        (+0x168)   -> RX hop ID
 path setDestinationHopIDRange(rxHopID | rxHopID<<16)
```

**TransmitQueue** additionally:

```
 path setRoutingOptions(7)                        (+0x438)
 path setDestinationHopIDRange(0x07ff0008)        (+0x3b8)   {lo=8, hi=0x7ff}
 path setDestinationPort(params+0x10 -> +8)
 path setSourcePort(NHI port)
 setSourceInitialCredits(credits-2)
 setIngressFlowControlEnable(7)                   (+0x190)
 setEgressFlowControlEnable(7)                    (+0x1a0)
 IOThunderboltTransmitQueue::withController
   enableRawMode(0)      (+0x1f0)   setFlags(8)        (+0x220)
   setQueueType(2)       (+0x230)   setRingSize(...)   (+0x170)
   setMaxFrameSize(0x1000) (+0x210) enableE2EFlowControl(1) (+0x1e0)
   setInterruptStride(32) (+0x240)  allocate()         (+0x180)
   getHopID(&u16)        (+0x168)   -> TX hop ID
 path setSourceHopIDRange(txHopID | txHopID<<16)  (+0x3a8)
```

So both directions use 4 KiB max frames, queue type 2, flag bit 3 (8), raw mode
off, E2E flow control on, interrupt stride 32, and a hop-ID range one hop wide
(`lo == hi`). The TX path globally routes the hop-ID space `0x008..0x7ff`.

### 7.6 Queue start / region publication  **(verified)**

`ReceiveQueue::start` (`0x3f5b0`) (early-returns if already started) does
`activatePath` (`+0x198`), creates a `DARTErrorHandler` on the IOMapper
(`+0x50`), and two `VerboseVmRegionRegistration`s on the ib device — one for the
**ring-management** registers (`+0x68`), one for the **descriptor ring** (`+0x78`).
It also builds an `OSArray` of `DARTMapping`s (`+0x58`) wrapping an
`IODMACommand` per MR. `registerRegion` (`0x40c00`) and `descriptorBuffer`
(`0x40c60`) return the 24-byte mminfo (or `0xe0002d8` if unset). These are the
two mminfos returned in RTR/RTS.

### 7.7 P_Key, GID, port/device attributes  **(verified)**

* `Interface::queryDevice` (`0x30278`) zeroes `ib_device_attr` and fills a
  minimal set; `+0xb8` sets `h` to 1; `ib_device+0x6e0 = 0`.
* `Interface::queryPort` (`0x303cc`) only works through the peer port
  (`PeerInterface+0xe0` → `getPort`, `IOThunderboltPort` vcall `+0xc48`):
  `max_msg_sz = 0x1000000` (16 MiB), `+0x18 = 0x400` (1024), `state = ACTIVE(4)`
  iff RDMA is enabled, and the RID as the port identity.
* `Interface::queryGID` (`0x30650`): port 0 only (else `-EINVAL`); returns the
  16-byte GID from the local node (`[self+0xb8]`, vcall `+0x8c8`).
* `Interface::queryPKey` (`0x2f320`): index 0 → `0xffff`, index 1 → `0`, else
  `-EINVAL`.
* `Interface::portImmutable` (`0x310b4`): 16 bytes from file offset `0x7a0` →
  `ib_port_immutable`, i.e. the static tuple
  `02 00 00 00 | 00 04 00 00 | 15 30 00 04 | 00 01 00 00`.

### 7.8 QP type and loopback  **(verified)**

* `KernelQP::isLoopback(ah)` (`0x354e8`): query the AH; if the AH attribute's
  bit 0 is set, compare the local GID (`QP+0xb8`) against the AH's DGID; else
  compare the `u16` at `QP+0xc8` with the AH attribute. Loopback short-circuits
  `postSend` (§5.5).
* `QueuePair::modifyQP` (`0x3ce40`) maps the `ib_qp_attr` bits onto QP fields and
  then runs the `INIT/RTR/RTS` handlers; it rejects attribute masks `>= 0x200000`
  and validates with `_ib_modify_qp_is_ok`.
* **`post_send`/`post_recv` on a `QueuePair` return `-EOPNOTSUPP`** (see §1): the
  data plane has no in-kernel verb engine. `QueuePair` exists to configure the
  hardware rings and hand their mminfo regions to user space; the queues are then
  driven directly by the userspace provider/library against the mapped registers
  and descriptor rings.

---

## 8. Execution model: what runs where

The most important architectural fact, and the one easiest to get wrong: after
setup there is **no dedicated IB/RDMA hardware engine** and, on the data plane,
**no in-kernel packet path**. The kext configures the Thunderbolt fabric and NHI
rings, then hands userspace direct access to those rings. This is a
*kernel-bypass* design.

```
 setup (kext)                                   userspace
 ────────────                                   ─────────
 allocate IOThunderboltTransmitQueue/ReceiveQueue
 allocate IOThunderboltPath (hop IDs, credits)
 get copyDeviceMemory()      -> register window  ──┐
 get copyDescriptorMemory()  -> descriptor ring  ──┤ register as VM regions,
 register both               -> regionId            │ return mminfo in modify_qp
                                                    ▼
                                          map window + ring (VMArea, via
                                          IORDMAFamily user client)
                                          write descriptors, ring doorbell
                                                    │
                                                    ▼
                                          NHI DMA engine moves frames along
                                          the path, within E2E credits
```

Evidence for each step:

* **No send method on the queue.** `AppleThunderboltRDMATransmitQueue` exposes
  only `start`, `stop`, `reset`, `mapMemoryRegion`, `deactivatePath`,
  `deallocateQueue`, … — it never enqueues a frame. The kext does not drive the
  ring.
* **The ring is exposed, not used.** `TransmitQueue::start` / `ReceiveQueue::start`
  call `copyDeviceMemory()` (the queue's MMIO register window) and
  `copyDescriptorMemory()` (the descriptor ring), wrap each in a
  `VerboseVmRegionRegistration` (→ `regionId`), and return the two `mminfo`
  through `modify_qp`'s `udata` (RTR from RX, RTS from TX).
* **The verbs are inert for a data QP.** `post_send`/`post_recv` return
  `-EOPNOTSUPP` and `poll_cq` returns 0 for a `QueuePair` (see §1, §7.8).
* **Memory is ordinary device DMA.** `mapMemoryRegion` builds a `DARTMapping`
  over the `IOMapper` (`IODMACommand`), i.e. pins/maps pages through the
  DART/IOMMU — and it is gated on the controller's SID-partition predicate. The
  `regionId` bookkeeping (`_ib_device_register_vma`, `_ib_device_lookup_mdesc`,
  `_vma_list_contains_region_id_locked`) is **software** in `IORDMAFamily`.

| Function | Runs in |
|---|---|
| Byte movement, framing, flow control, routing | **Hardware** — Thunderbolt NHI + fabric path |
| Descriptor consumption, doorbell, head/tail | **Hardware** — NHI rings |
| Ring setup, path/credit/hop-ID configuration | kext (setup only) |
| QP state machine, GSI SEND/RECV, completions | kext |
| SA MADs (ClassPortInfo, PathRecord) | kext |
| Data-plane protocol/ordering/retransmit, region lookup | userspace provider |
| MR address translation (`regionId` → memory) | `IORDMAFamily` (software) |

There is no BTH/PSN/ACK-NAK/atomics/RC-retransmit block and no NIC-style MTT/MPT
table: the ibverbs contract is implemented in software, and the "RDMA" is really
memory-mapped NHI queue transport over hop-routed Thunderbolt frames. The only
hardware that can be said to "handle ibverbs after setup" is the generic
Thunderbolt NHI DMA engine.

> One caveat: the 32 KiB MR cap with 16 KiB alignment and a DVA arena is
> suggestive of a small hardware address-window/translation assist tied to
> region IDs, but nothing in the kext programs such a table — it may be a
> generic DMA-mapping feature rather than an IB offload. This is not proven
> either way.

---

## 9. Memory regions and the DVA arena

### 9.1 User MR registration  **(verified)**

`ProtectionDomain::regUserMR(pd, start, len, virt, flags, udata, mr**)`
(`0x39e6c`) enforces:

* `start == virt`;
* `start % (1 << PAGE_SHIFT) == 0`  (Apple Silicon: 16 KiB pages, `PAGE_SHIFT` 14);
* `len <= 32768` (32 KiB);
* the length/address are rounded to **0x4000 (16 KiB)** granularity
  (mask `0xffffffffffffc000`).

DVA assignment:

* **SID-partitioned** systems: require `(len + VA) >> 40 == 0`; try fixed DVAs
  `0x10000000000`, `0x10100000000`, `0x10200000000` first.
* **Otherwise**: allocate from an `IORangeAllocator` (`pd+0x170`, size
  `aligned_len + 0x4000`, alignment `0x4000`).
* Failure → `-ENOMEM` (`-12`) with log *"could not be assigned to a DVA arena"*.

`MemoryRegion::initWithParams(pd, VA, len, virt, flags, tag, bool)` (`0x3687c`)
builds the `IOMemoryDescriptor` with `IOMemoryDescriptor::withAddressRange(VA,
len, 3, task)` (task from `pd->uobject->context->ufile->task`), prepares it, and
logs *"Userspace VA %p maps to DVA %p"*.

`ProtectionDomain::initWithPD` (`0x3982c`) sets up the DVA arenas:

* non-SID: `[0x10800000000, 0x11000000000)` (allocator `+0x180`, size
  `0x800000000` = 32 GiB);
* SID: a larger arena rooted at `0x10000000000` (allocator `+0x178`, size
  `0x30000000000` = 3 TiB).

`get_dma_mr` (`0x39e24`) always returns `-EOPNOTSUPP` (`-102`): the driver does
not support kernel DMA MRs; use user MRs.

### 9.2 PD limit  **(verified)**

`Interface::allocPD` (`0x30728`) hands out the first free bit of a **6-bit** PD
mask (PD indices 0..5) to a `ProtectionDomain::withPD(pd, index, dispatchQueue,
sid_partitioned)`.

### 9.3 SID partitioning  **(verified)**

`TransmitQueue` maps MRs itself only when the system is SID-partitioned;
otherwise it logs *"skipping MR mappings as system is not sid partitioned…"* and
relies on the receive queue to map MRs. This mirrors the DVA-arena split above.

---

## 10. CQ and completions  **(verified)**

`AppleThunderboltRDMAKernelCQ::postCompletion(ib_wc)` queues a work completion;
`reqNotify(ib_cq_notify_flags)` implements CQ notification (the kernel CQ path
used by the GSI QP). The `devOps` ops table (`ib_device_ops`) at `0x50b20` wires
`post_send@+0x18`, `post_recv@+0x20`, `poll_cq@+0x38`, `req_notify_cq@+0x48`,
`query_device@+0x60`, `query_port@+0x80`, PD/CQ/QP/MR create/destroy at their
standard slots (`create_qp@+0x158`, `create_cq@+0x178`, `reg_user_mr@+0x1a0`,
etc.).

---

## 11. End-to-end bring-up sequence

```
 1. IODeviceTree:rdma-enable (or DarwinOS) + TB5 speed     → RDMA allowed
 2. AppleThunderboltRDMAPort binds IOThunderboltLocalNode   → "RID" published
 3. Peer ROM has protocol dir id 0xFA57 ver 1               → XDomainService
 4. AppleThunderboltRDMAPeerInterface.probe: Service Key matches 0x…AD
 5. PeerInterface.start: getController/getXDomainLink, registerService
 6. Interface.start: __ib_alloc_device → ib_device "tbt", node_type 100,
    _ib_set_device_ops, netdev, match peer notifications
 7. IORDMAInterface::registerIBInterface → ibverbs sees "tbt"
 8. SAAgent registered on GSI QP (qp_type 1); answers ClassPortInfo/PathRecord
 9. alloc_pd (0..5) ; create_cq ; reg_user_mr (≤32 KiB, 16 KiB aligned, DVA)
10. create_qp qp_type 3:
      INIT → TransmitQueue::start, getNHIHopID, ReceiveQueue::withTBController
      RTR  → peer TX dst hop ID → allocateAndActivatePath, return RX mminfo×2
      RTS  → return TX mminfo×2
    (mminfo = {regionId, 0, length, 0})
11. On the GSI QP: post_send(SEND/SEND_WITH_IMM) → TbtFrame(s) over the XDomain
    listener; post_recv → ring of RecvSlots (message + 40-byte GRH).
    On the data-plane QueuePair, userspace drives the mapped NHI rings directly
    (post_send/post_recv are EOPNOTSUPP).
12. Teardown: modify QP to RST/SQD/SQE/ERR; deactivate paths; unregister VMAs
```

---

## 12. Constants cheat-sheet

| Constant | Value | Where |
|---|---|---|
| Protocol ID | `64087` / `0xFA57` | personality match |
| Protocol Version | `1` | personality match |
| Service Key (compared) | `0x…41 44` (`"AD"`) | `probe` @ `0x800` |
| Protocol UUID (memory) | `59600081-1252-42f0-8b7f-12f2ee26bfdb` | @ `0x7f0` |
| Protocol UUID (wire) | `81 00 60 59 f0 42 52 12 f2 12 7f 8b db bf 26 ee` | word-swapped |
| TbtFrame header | `bit24 = EOF`; `0x01000000` for last frame | `sendFrame` |
| Max message | `440` (`0x1b8`) bytes | reassembler |
| Frame payload (non-final) | `220` (`0xdc`) bytes | reassembler |
| GRH prefix on recv | `40` (`0x28`) bytes | `deliverMessage` |
| Recv buffer min | `len + 40`, total ≤ `480` | `postRecv` |
| RX length check | `> 0x1f` ("header+CRC is 32") | `protocolListenerCallback` |
| RX payload slice | `[0x1c, len-4)` = frame header + payload | `protocolListenerCallback` |
| QP type: GSI | `1` → KernelQP | `isKernelQP` |
| QP type: data | `3` → QueuePair, `create_flags==0`, `sge<2`, `1≤max_wr<4096` | `isSupported` |
| `post_send`/`post_recv` | KernelQP only; `QueuePair` → `-EOPNOTSUPP` (`-102`) | devOps wrappers |
| Max frame size (rings) | `0x1000` (4096) | queue setup |
| Queue type | `2`; flags `8`; interrupt stride `32` | queue setup |
| E2E credits | `min(speed==2 ? 60 : 30, NHI max)` | path setup |
| Path priority/weight/counter | `4` / `1` / `7` | path setup |
| TX dest hop-ID range | `{lo=8, hi=0x7ff}` | TX path |
| RX SOF/EOF PDF bitmask | `2` / `0xc` | RX queue |
| MR max size | `32768` (32 KiB), 16 KiB aligned | `regUserMR` |
| DVA arena (non-SID) | `[0x10800000000, 0x11000000000)` | `initWithPD` |
| PD limit | 6 (`0..5`) | `allocPD` |
| max_msg_sz | `0x1000000` (16 MiB) | `queryPort` |
| device node_type | `100`; name `"tbt"` | `Interface::start` |

---

## 13. What is still unknown

These are the places where the disassembly stops short of a complete wire spec:

* **The data-plane engine is in user space (§8).** The kext sets up the NHI rings,
  paths and mminfo regions, but `post_send`/`post_recv`/`poll_cq` are inert for a
  `QueuePair`, so how user space rings the doorbell, sizes/consumes the descriptor
  ring, and interprets the ring-management registers is defined by the userspace
  provider/library (matching the register IDs this kext hands out), not this kext.
* **Hop-ID semantics.** The driver allocates a TX and an RX hop ID per QP via
  `getNHIHopID`/`getHopID` and exchanges the peer's TX destination hop ID out of
  band, but which byte range of the 16-bit hop ID is "hop" vs "route" (the fabric
  uses a routed hop-ID space up to `kIOThunderboltMaxHopID`, and the TX range is
  `0x008..0x7ff`) is not pinned down here. Log *"txDstHopId %d >
  kIOThunderboltMaxHopID (%d)"* guards it.
* **CM/queue-number exchange.** `"Queue Pair Number"` and `"ADRID"` properties
  appear in the strings, but the exact CM packet that carries QP numbers, hop
  IDs and the two mminfo records end-to-end (`tbt_ibv_modify_qp_ex`) lives in the
  RDMA CM / libibverbs userspace, not this kext.
* **SID partitioning trigger.** How the system decides it is "SID partitioned"
  (which selects the larger DVA arena and the TX-side MR mapping) is outside the
  kext.
* **CRC details.** The config-XDomain-response CRC is a CRC32 over the packet
  (per `prepareForExecution`); the polynomial/seed were not extracted, but the
  field position and coverage are known.
* **Return types** are inferred (Itanium mangling does not encode them).

Everything else — discovery, the TbtFrame transport, fragmentation/reassembly,
GSI/SAMAD handling, the INIT/RTR/RTS hop-ID + mminfo exchange, ring/path
configuration, MR/DVA constraints and the full ibverbs object model — was read
directly out of the disassembly and is recorded above.
