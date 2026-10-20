#!/usr/bin/env python3
"""Resolve a vtable's entries to symbols by taking the low 32 bits (chained
fixup encoding) as the target address.

Usage: vt2.py <macho> <vtable-symbol> [max]
"""
import subprocess, sys, re, bisect, struct

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def segments(path):
    segs, cur = [], None
    for line in run("otool", "-l", path).splitlines():
        m = re.search(r"sectname (\S+)", line)
        if m:
            cur = {"name": m.group(1)}; segs.append(cur); continue
        for key, pat in (("addr", r"addr (0x[0-9a-f]+)"), ("size", r"size (0x[0-9a-f]+)"),
                         ("offset", r"offset (\d+)")):
            m = re.search(pat, line)
            if m and cur is not None and key not in cur:
                cur[key] = int(m.group(1), 0); break
    return [s for s in segs if "addr" in s and "offset" in s and s.get("size")]

def main():
    path, sym = sys.argv[1], sys.argv[2]
    maxn = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    syms = []
    name = None
    for line in run("nm", path).splitlines():
        p = line.split()
        if len(p) == 3:
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    addrs = [a for a, _ in syms]
    def resolve(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i >= 0:
            return syms[i][1]
        return "?"
    vt = None
    for line in run("nm", path).splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == sym:
            vt = int(p[0], 16); break
    if vt is None:
        print("vtable not found"); return
    seg = None
    for s in segments(path):
        if s["addr"] <= vt < s["addr"] + s["size"]:
            seg = s; break
    if not seg:
        print("no file offset"); return
    data = open(path, "rb").read()
    off = seg["offset"] + (vt - seg["addr"])
    for i in range(maxn):
        raw = struct.unpack_from("<Q", data, off + i * 8)[0]
        low = raw & 0xffffffff
        if raw == 0:
            break
        nm = resolve(low)
        nm = re.sub(r"^__Z", "_Z", nm)
        p = subprocess.run(["c++filt", nm], capture_output=True, text=True).stdout.strip() or nm
        # strip leading (anonymous namespace) noise from c++filt
        p = re.sub(r"^\(anonymous namespace\)::", "", p)
        tag = ""
        if low < 0x1000:
            tag = "  <stub>"
        print(f"idx {i:4d}  off {i*8:#06x}  {low:#010x}  {p}{tag}")

main()
