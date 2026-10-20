#!/usr/bin/env python3
"""Dump a vtable at an exact address and resolve entries to symbols.
Usage: vtdump.py <macho> <0xVTADDR> [count]
"""
import subprocess, sys, re, bisect, struct

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def segments(path):
    segs = []
    out = run("otool", "-l", path)
    name = None
    for line in out.splitlines():
        m = re.search(r"sectname (\S+)", line)
        if m:
            seg = {"name": m.group(1)}; segs.append(seg); continue
        m = re.search(r"addr (0x[0-9a-f]+)", line)
        if m and segs:
            segs[-1]["addr"] = int(m.group(1), 16); continue
        m = re.search(r"size (0x[0-9a-f]+)", line)
        if m and segs:
            segs[-1]["size"] = int(m.group(1), 16); continue
        m = re.search(r"offset (\d+)", line)
        if m and segs:
            segs[-1]["offset"] = int(m.group(1)); continue
    return [s for s in segs if "addr" in s and "offset" in s and s.get("size")]

def main():
    macho = sys.argv[1]
    vtaddr = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    data = open(macho, "rb").read()
    syms = []
    for line in run("nm", "-n", macho).splitlines():
        p = line.split()
        if len(p) >= 3:
            try: syms.append((int(p[0], 16), p[2]))
            except ValueError: pass
    syms.sort()
    A = [a for a, _ in syms]
    def resolve(a):
        i = bisect.bisect_right(A, a) - 1
        if i < 0: return None
        base, name = syms[i]
        d = subprocess.run(["c++filt"], input=name, capture_output=True, text=True).stdout.strip()
        return (d, a - base)
    segs = segments(macho)
    def read(va):
        for s in segs:
            if s["addr"] <= va < s["addr"] + s["size"]:
                off = s["offset"] + (va - s["addr"])
                return struct.unpack("<Q", data[off:off+8])[0]
        return None
    base = vtaddr + 16
    for i in range(count):
        raw = read(base + i * 8)
        if raw is None: break
        ptr = raw & 0x0000FFFFFFFFFFFF
        r = resolve(ptr)
        tag = r[0] + (("+%d" % r[1]) if r and r[1] else "") if r else ""
        print("idx %3d  off 0x%03x  -> 0x%x  %s" % (i, i * 8, ptr, tag))

main()
