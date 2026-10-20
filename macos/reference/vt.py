#!/usr/bin/env python3
"""Dump a C++ vtable from a (thinned) Mach-O: resolve each entry to a symbol.

Usage: vt.py <macho> <vtable-symbol-substring> [start_index] [count]
"""
import subprocess, sys, re, bisect, struct

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def segments(path):
    segs = []
    out = run("otool", "-l", path)
    name = None
    for line in out.splitlines():
        m = re.search(r"segname (\S+)", line)
        if m:
            name = m.group(1)
            seg = {"name": name}
            segs.append(seg)
            continue
        m = re.search(r"vmaddr (0x[0-9a-f]+)", line)
        if m and segs:
            segs[-1]["vmaddr"] = int(m.group(1), 16); continue
        m = re.search(r"fileoff (\d+)", line)
        if m and segs:
            segs[-1]["fileoff"] = int(m.group(1)); continue
        m = re.search(r"vmsize (0x[0-9a-f]+)", line)
        if m and segs:
            segs[-1]["vmsize"] = int(m.group(1), 16); continue
    return [s for s in segs if "vmaddr" in s and "fileoff" in s and s.get("vmsize")]

def main():
    macho = sys.argv[1]
    needle = sys.argv[2]
    start = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 200

    syms = []
    for line in run("nm", "-n", macho).splitlines():
        p = line.split()
        if len(p) >= 3:
            try:
                syms.append((int(p[0], 16), p[2]))
            except ValueError:
                pass
    syms.sort()
    addrs = [a for a, _ in syms]

    def resolve(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0:
            return None
        base, name = syms[i]
        d = subprocess.run(["c++filt"], input=name, capture_output=True, text=True).stdout.strip()
        return (d, a - base)

    vt = [(a, n) for a, n in syms if needle in n]
    if not vt:
        # try demangled
        for a, n in syms:
            d = subprocess.run(["c++filt"], input=n, capture_output=True, text=True).stdout
            if needle in d:
                vt.append((a, n))
    if not vt:
        print("vtable not found", file=sys.stderr); sys.exit(1)
    vtaddr = vt[0][0]
    print("vtable %s @ 0x%x" % (vt[0][1], vtaddr))

    segs = segments(macho)
    def read(va):
        for s in segs:
            if s["vmaddr"] <= va < s["vmaddr"] + s["vmsize"]:
                off = s["fileoff"] + (va - s["vmaddr"])
                return struct.unpack("<Q", open(macho, "rb").read()[off:off + 8])[0]
        return None

    base = vtaddr + 16  # skip offset-to-top + typeinfo
    for i in range(start, start + count):
        raw = read(base + i * 8)
        if raw is None:
            break
        ptr = raw & 0x0000FFFFFFFFFFFF
        r = resolve(ptr)
        tag = ""
        if r and r[0]:
            tag = r[0] + (("+%d" % r[1]) if r[1] else "")
        print("idx %3d  off 0x%03x  ->  0x%x  %s" % (i, i * 8, ptr, tag))

main()
