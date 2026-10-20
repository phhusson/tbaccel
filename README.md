Completely vibe-coded slop that currently doesn't work

# tbaccel

A Thunderbolt/USB4 accelerator-offload protocol, with implementations for Linux
and macOS.

* `linux/` — the original kernel module (`tbaccel.ko`) and its user-space
  client/peer/test programs.
* `macos/` — a macOS IOKit kext implementing the same protocol, plus the
  reconstructed private `IOThunderboltFamily` headers it needs.
* `docs/macos-thunderbolt-protocol-driver.md` — a full write-up of how to write
  a Thunderbolt protocol ("XDomain service") driver on macOS, using the
  Thunderline network driver (`AppleThunderboltIP`) and the structure of
  `IOThunderboltFamily` as the reference.
* `docs/apple-thunderbolt-rdma-protocol.md` — reverse-engineered documentation
  of Apple's own **RDMA-over-Thunderbolt** protocol (`AppleThunderboltRDMA.kext`):
  XDomain discovery/binding, the `TbtFrame` message transport, the GSI QP and
  SAMAD agent, the NHI data plane (paths, hop IDs, mminfo exchange), and the
  MR/DVA model.

## The protocol in one paragraph

Two XDomain peers bind through the `accel` service. Each link gets two HopID
pairs: a *control* pair carrying small framed messages (`ARM`/`ACK`/`DONE`/
`BLOB`) and a *data* pair carrying raw region bytes DMA'd straight into the
peer's buffer. A transfer is: `SUBMIT → ARM{slot,len} → ACK{slot,armed} →
data → DONE{slot}`, and the receiver signals the slot fence when the data has
landed. On top of that, `tbaccel_proto.h` defines a small `TBX1` operation
protocol (`PREPARE`/`PREPARED`/`LOAD`/`MATMUL`/`RESULT`) — the reference peer
runs a matmul.

Because the wire protocol is identical on both sides, a Linux host running
`tbaccel.ko` and a macOS host running `TbAccel.kext` discover each other (both
advertise service key `accel`, protocol id/version `1`/`1`) and interoperate.

## Status

* Linux: working module + user-space tools (see `linux/`); the protocol is
  credit-based (`ARM/ACK/CREDIT/DONE/BLOB`).
* macOS: the driver builds for `arm64e`, loads from the Auxiliary Kernel
  Collection, and the fabric is working **in both directions**: transmit frames
  reach the Linux peer, and frames from the peer land in the Mac's receive ring
  (verified by counter: 3 frames per 4096-byte transfer, deterministic across
  repeated sends).
* Still open: framing/accounting for received payloads (data frames are counted
  but not yet parsed into slots), a Mac-side slot/region path that works without
  the userspace client, and the MTL/IOSurface fence integration.

The receive-path recipe and the staging pitfalls that made this look unsolvable
for a while are written up in
`docs/macos-thunderbolt-protocol-driver.md` §27.0.15-27.0.16.

See `macos/README.md` for build/load instructions and the remaining unknowns.
