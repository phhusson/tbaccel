# macOS tbaccel driver

A macOS IOKit (KEXT) implementation of the **tbaccel** Thunderbolt/USB4
accelerator-offload protocol, ported from `../linux/`, targeting **ANE/Metal
coop over Thunderbolt** (see `ggml/src/ggml-ane`).

Wire-compatible with Linux `tbaccel.ko`: both advertise the `accel` XDomain
service and speak the credit-based `ARM/ACK/CREDIT/DONE/BLOB` handshake and the
`TBX1` control protocol.

See `../docs/macos-thunderbolt-protocol-driver.md` for the (disassembly-backed)
write-up, and `../docs/apple-thunderbolt-rdma-protocol.md` for the
reverse-engineered protocol of Apple's own `AppleThunderboltRDMA.kext` (the
kernel-verbs provider this driver's transport mechanics are modelled on).

---

## Design (v2, corrected against the KDK)

Earlier revisions assumed a protocol driver subclasses
`IOThunderboltXDomainServiceClient` and sends control requests. **That is
wrong:** disassembly of `AppleThunderboltIPTransmitter` shows protocol drivers
are *plain `IOService` subclasses* bound to `IOThunderboltXDomainService`, and
they move data as **frames** over `IOThunderboltTransmitQueue` /
`IOThunderboltReceiveQueue`, exactly like the Linux driver's rings.

```
TbAccelClient : IOService          (provider: IOThunderboltXDomainService)
  ├── fCtlPath,  fDataPath         IOThunderboltPath
  ├── fCtlTxQ/fCtlRxQ              IOThunderboltTransmit/ReceiveQueue
  ├── fDataTxQ/fDataRxQ            IOThunderboltTransmit/ReceiveQueue
  └── TbAccelUserClient            external methods (the /dev/tbaccelN analogue)
```

* **Buffers are IOSurfaces.** `kTbAccelMethodImportSurface` takes the task base
  address + size of the surface (already page-aligned and wired) and builds an
  `IOMemoryDescriptor`; receive frames are posted against it, so data lands
  straight in the surface with no copy.
* **Fences are MTLSharedEvent-shaped.** `kTbAccelMethodEvent` implements
  `CREATE/SIGNAL/WAIT/DESTROY` on a 64-bit signaled value, the same primitive
  `ggml-ane` uses locally with `MTLSharedEvent`.

## Layout

```
Private/IOThunderboltFamily.h    reconstructed private API (KDK-verified)
Shared/TbAccelUAPI.h             ABI (mirrors linux/thunderbolt_accel.h + macOS
                                 ImportSurface/Event extensions)
Shared/TbAccelProtocol.h         txaccel control protocol (mirrors Linux)
Kernel/TbAccelClient.{h,cpp}     protocol engine
Kernel/TbAccelUserClient.{h,cpp} external methods
Kernel/TbAccelService.{h,cpp}    local side: advertises "accel"
Kernel/TbAccelDriver.cpp         kmod entry points
User/tbaccel.c                   user-space exerciser
scripts/load.sh, unload.sh       developer load/unload
reference/*.symbols.txt          KDK/kernel-collection symbol dumps
reference/disas.py, vcall.py, vt.py   annotating disassembler + vtable walkers
reference/rdma_dis.py, vtsyms.py      RDMA-kext disassembler and fixup-based
                                      vtable/const resolver
reference/fat4e.py               arm64e slice extractor (fat/kext -> thin Mach-O)
```

## Build / load

```sh
make            # build/TbAccel.kext (arm64e)
make user       # build/tbaccel
scripts/load.sh # sudo kextutil -l; needs boot security Reduced/Permissive
scripts/unload.sh
```

On Apple Silicon a third-party kext needs both a boot policy that trusts a
locally-signed Auxiliary Kernel Collection **and** per-kext approval:

```sh
# 1. Boot into macOS Recovery (1TR): shut down, hold power, Options > Continue,
#    Utilities > Terminal, then:
bputil -k          # enable trust in locally-signed AuxKC; auto-downgrades to
                   # Reduced Security. Prompts for username/password (or use
                   # -u <user> -p <pass>). Reboot into macOS afterwards.
# 2. Try the load, then approve it:
sudo kmutil load -p build/TbAccel.kext
#    -> if it says "not approved to load", approve it in
#       System Settings > Privacy & Security, then retry / reboot.
```

### Signing / policy escalation

`kmutil` authenticates kexts against the **Apple anchor**, so an ad-hoc or
self-signed kext fails with `Authenticating extension failed: Bad code
signature` even after the ownership is fixed. Options, cheapest first:

```sh
sudo kmutil load -z -p build/TbAccel.kext      # skip the authorization gate
```

```sh
# SIP carries the CSR_ALLOW_UNTRUSTED_KEXTS bit that AMFI consults; this is
# usually the actual fix (Reduced/Permissive mode alone is not enough).
# From Recovery / 1TR:
csrutil disable
# reboot, then retry kmutil load
```

```sh
# Permissive Security (no enforcement) - from Recovery / 1TR:
sudo bputil -n
# reboot, then retry kmutil load
```

```sh
# still "Bad code signature": disable AMFI's kext check - from Recovery / 1TR:
sudo bputil -a
sudo nvram boot-args="amfi_get_out_of_my_way=1"
# reboot, then retry kmutil load
```

The only way to satisfy the requirement under Reduced Security is a paid
**Developer ID Application** certificate + notarization. `bputil -n`/`-a`
recreate the LocalPolicy, so re-check `csrutil status` after each change.

`kmutil load -p` stages the kext into the Auxiliary Kernel Collection; a reboot
may be required before it binds. Watch it come up with:

```sh
log stream --predicate 'process == "kernel"' | grep -iE 'tbaccel|accel|xdomain'
```

## Status

* Builds and links for `arm64e`; every `IOThunderbolt*` symbol it references
  resolves with an exactly matching mangled name in the shipping family.
* Wire protocol, buffers (IOSurface), events and the completion ABI are done.
* **Fabric: working in both directions.** Transmit frames reach the Linux peer;
  frames from the peer land in the receive ring (3 frames per 4096-byte
  transfer, deterministic across repeated sends).
* The receive path needs three things, all documented in
  `../docs/macos-thunderbolt-protocol-driver.md` §27.0.15:

  ```
  src port = the lane adapter (IOThunderboltXDomainLink's provider), src hop = the
             peer's transmit hop from its login/ARM
  dst port = the NHI port, dst hop = the receive queue's hop
  + IP's path attribute block (setCounterEnable(7) ... setRoutingOptions(0x11))
  + activateSynchronous(), not activate()   <- activate() returns before the
                                               per-hop writes are done
  ```
* **Still open:** received payloads are counted but not yet framed into slots;
  a Mac-side slot/region path that does not require the userspace client; and
  the MTL/IOSurface fence wiring.
* When changing this kext, read §27.0.16 first: a single undefined family symbol
  makes the auxiliary collection build fail while `kmutil load` still reports
  success, so the old build keeps running and every fix appears ignored.
  Bump `CFBundleVersion` per build and check `TbAccelBuildTag`.
