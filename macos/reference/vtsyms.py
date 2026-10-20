#!/usr/bin/env python3
"""Resolve Mach-O vtable / const-array entries using a dyld_info -fixups dump
and a symbol table. Demangles in bulk.

Usage: vtsyms.py <macho> <fixups-file> <mangled-or-substring> [count] [stride]
"""
import subprocess, sys, re, bisect

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def load_syms(macho):
    syms = []
    for line in run("nm", "-n", macho).splitlines():
        p = line.split()
        if len(p) >= 3:
            try: syms.append((int(p[0], 16), p[2]))
            except ValueError: pass
    syms.sort()
    return syms

def demangle_all(names):
    inp = "\n".join(names)
    out = run("c++filt", *[])  # placeholder
    proc = subprocess.run(["c++filt"], input=inp, capture_output=True, text=True)
    return proc.stdout.splitlines()

def load_fixups(path):
    fix = {}
    for line in open(path, errors="replace"):
        m = re.match(r"\s*(\S+)\s+(\S+)\s+(0x[0-9a-fA-F]+)\s+(\S+)\s+(.*)$", line)
        if not m: continue
        addr = int(m.group(3), 16); typ = m.group(4); tgt = m.group(5).strip()
        if typ.endswith("bind"):
            fix[addr] = "<import> " + re.sub(r"\s*\(.*\)$", "", tgt)
        else:
            m2 = re.match(r"(0x[0-9a-fA-F]+)", tgt)
            if m2: fix[addr] = int(m2.group(1), 16)
    return fix

def main():
    macho, fixpath, needle = sys.argv[1], sys.argv[2], sys.argv[3]
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 80
    stride = int(sys.argv[5]) if len(sys.argv) > 5 else 8
    syms = load_syms(macho)
    A = [a for a, _ in syms]
    dm = demangle_all([n for _, n in syms])
    def name_of(a):
        i = bisect.bisect_right(A, a) - 1
        if i < 0: return "?"
        b = A[i]
        return dm[i] + (("+0x%x" % (a - b)) if a - b else "")
    fix = load_fixups(fixpath)
    if needle.startswith("0x"):
        vt = int(needle, 16); label = needle
    else:
        vt = None; label = None
        for i, (a, n) in enumerate(syms):
            if n == needle or dm[i] == needle:
                vt = a; label = dm[i]; break
        if vt is None:
            for i, (a, n) in enumerate(syms):
                if needle in n or needle in dm[i]:
                    vt = a; label = dm[i]; break
        if vt is None:
            print("not found"); sys.exit(1)
    print("%s @ 0x%x" % (label, vt))
    for i in range(count):
        addr = vt + 16 + i * stride
        tgt = fix.get(addr)
        if tgt is None:
            s = "(no fixup)"
        elif isinstance(tgt, str):
            s = tgt
        else:
            s = "0x%x  %s" % (tgt, name_of(tgt))
        print("off 0x%03x  %s" % (i * stride, s))

main()
