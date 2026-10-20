#!/usr/bin/env python3
"""Disassemble a named arm64e symbol from a (thinned) Mach-O kext, resolved
against the full nm symbol table.

Usage:
  disas.py <macho> <substring> [max-insns]
  disas.py <macho> <hexaddr> [max-insns]      (addr form: 0x...)
"""
import subprocess, sys, re, bisect

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def demangle_one(name):
    if not name.startswith("_Z") and not name.startswith("__Z"):
        return name
    return subprocess.run(["c++filt"], input=name, capture_output=True, text=True).stdout.strip()

def load_syms(macho):
    syms = []
    for line in run("nm", "-n", macho).splitlines():
        p = line.split()
        if len(p) >= 3:
            try:
                syms.append((int(p[0], 16), p[2], p[1]))
            except ValueError:
                pass
    syms.sort()
    return syms

def main():
    macho = sys.argv[1]
    needle = sys.argv[2]
    maxi = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    syms = load_syms(macho)
    addrs = [a for a, _, _ in syms]

    def resolve(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0:
            return None
        base, name, typ = syms[i]
        if typ.lower() not in ("t", "s"):
            return None
        d = demangle_one(name)
        return (d, a - base)

    if needle.startswith("0x"):
        start = int(needle, 16)
        names = [n for a, n, _ in syms if a == start]
        label = next((demangle_one(n) for a, n, _ in syms if a == start), needle)
        hits = [(start, label)]
    else:
        hits = []
        for a, name, typ in syms:
            if typ.lower() != "t":
                continue
            d = demangle_one(name)
            if needle in name or needle in d:
                hits.append((a, d))
        hits = [h for h in hits if "_os_log_fmt" not in h[1] and "_ktv" not in h[1]
                and "cold." not in h[1]][:5]

    if not hits:
        print("no match", file=sys.stderr); sys.exit(1)

    for addr, label in hits:
        i = bisect.bisect_right(addrs, addr)
        nxt = syms[i][0] if i < len(syms) else addr + maxi * 4
        stop = min(nxt, addr + maxi * 4)
        print("=" * 80)
        print("%s  @ 0x%x - 0x%x" % (label, addr, stop))
        print("=" * 80)
        dis = run("objdump", "-d", "--no-show-raw-insn",
                  "--start-address=0x%x" % addr, "--stop-address=0x%x" % stop, macho)
        for line in dis.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
            if not m:
                continue
            ins = m.group(2)
            def rep(mm):
                t = int(mm.group(1), 16)
                r = resolve(t)
                if r and r[0]:
                    return "<%s%s>" % (r[0], ("+%d" % r[1]) if r[1] else "")
                return "<0x%x>" % t
            ins = re.sub(r"<(0x[0-9a-f]+)>", rep, ins)
            ins = re.sub(r"(bl|b|b\.[a-z]+)\t(0x[0-9a-f]+)",
                         lambda m: m.group(1) + "\t" + (lambda r: "<%s%s>" % (r[0], ("+%d" % r[1]) if r[1] else "") if r else m.group(2))(resolve(int(m.group(2), 16))),
                         ins)
            print("%6x: %s" % (int(m.group(1), 16), ins))
        print()

main()
