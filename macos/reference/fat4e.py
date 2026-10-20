#!/usr/bin/env python3
"""Extract a slice from an Apple fat Mach-O (handles arm64e/arm64e.x1 naming).

Usage: fat4e.py <in> <out>
"""
import struct, sys

inp, outp = sys.argv[1], sys.argv[2]
data = open(inp, "rb").read()
magic = struct.unpack(">I", data[:4])[0]
assert magic in (0xcafebabe, 0xcafebabf), hex(magic)
be = magic == 0xcafebabe
fmt = ">" if be else "<"
nfat = struct.unpack(fmt + "I", data[4:8])[0]
slices = []
off = 8
for _ in range(nfat):
    cputype, cpusubtype, offset, size, align = struct.unpack(fmt + "IIIII", data[off:off + 20])
    slices.append((cputype, cpusubtype, offset, size, align))
    off += 20

# CPU_TYPE_ARM64 = 0x0100000c ; we want the arm64e family (subtype base 2)
cands = [s for s in slices if s[0] == 0x0100000C and (s[1] & 0xFF) == 2]
if not cands:
    cands = [s for s in slices if s[0] == 0x0100000C]
s = cands[0]
print("picked cputype=0x%x cpusubtype=0x%x offset=0x%x size=0x%x" % s[:4])
open(outp, "wb").write(data[s[2]:s[2] + s[3]])
