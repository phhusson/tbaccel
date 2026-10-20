#!/usr/bin/env python3
"""Annotating arm64e disassembler for the AppleThunderboltRDMA kext.

Resolves:
  * bl / b targets to nm text symbols (demangled), including auth_stub thunks
    (which appear as indirect symbol table entries) and GOT slots.
  * adrp+ldr / adrp+add into __got / __auth_got as "<sym>".

Usage: rdma_dis.py <macho> <symbol-substring|0xADDR> [max-insns]
"""
import subprocess, sys, re, bisect

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def dem(n):
    if n.startswith("_Z") or n.startswith("__Z"):
        return subprocess.run(["c++filt"], input=n, capture_output=True,
                              text=True).stdout.strip()
    return n

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

def load_indirect(macho):
    """address -> imported symbol name (from the indirect symbol table)."""
    table = {}
    for line in run("otool", "-Iv", macho).splitlines():
        m = re.match(r"^(0x[0-9a-f]+)\s+\d+\s+(\S+)$", line)
        if m:
            table[int(m.group(1), 16)] = m.group(2)
    return table

def main():
    macho = sys.argv[1]
    needle = sys.argv[2]
    maxi = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    syms = load_syms(macho)
    A = [a for a, _, _ in syms]
    indirect = load_indirect(macho)
    ind_addrs = sorted(indirect)

    def nearest(a):
        i = bisect.bisect_right(A, a) - 1
        if i < 0:
            return None
        b, n, t = syms[i]
        return (dem(n), a - b, t)

    def resolve(a):
        if a in indirect:
            return dem(indirect[a])
        # auth stub / near an indirection slot?
        i = bisect.bisect_right(ind_addrs, a) - 1
        if i >= 0:
            base = ind_addrs[i]
            if a - base < 0x20:
                return dem(indirect[base]) + "+%d" % (a - base)
        r = nearest(a)
        if r and r[2].lower() in ("t", "s"):
            return r[0] + (("+%d" % r[1]) if r[1] else "")
        return None

    if needle.startswith("0x"):
        start = int(needle, 16)
        hits = [(start, needle)]
    else:
        hits = []
        for a, n, t in syms:
            if t.lower() not in ("t", "s"):
                continue
            d = dem(n)
            if "_os_log_fmt" in d or "_ktv" == d[-4:] or "cold." in d:
                continue
            if needle in n or needle in d:
                hits.append((a, d))
        hits = hits[:6]
    if not hits:
        print("no match", file=sys.stderr); sys.exit(1)

    for addr, label in hits:
        i = bisect.bisect_right(A, addr)
        nxt = syms[i][0] if i < len(syms) else addr + maxi * 4
        stop = min(nxt, addr + maxi * 4)
        print("=" * 78)
        print("%s  @ 0x%x" % (label, addr))
        print("=" * 78)
        dis = run("objdump", "-d", "--no-show-raw-insn",
                  "--start-address=0x%x" % addr, "--stop-address=0x%x" % stop, macho)
        for line in dis.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
            if not m:
                continue
            ins = re.sub(r"\s+", " ", m.group(2).strip())
            mm = re.match(r"(bl|b|b\.[a-z]+) (0x[0-9a-f]+)", ins)
            if mm:
                tgt = int(mm.group(2), 16)
                r = resolve(tgt)
                ins = "%s %s" % (mm.group(1), ("<" + r + ">") if r else mm.group(2))
            mm = re.match(r"(adrp) x(\d+), (0x[0-9a-f]+)", ins)
            if mm:
                page = int(mm.group(3), 16)
                ins = "%s  ; page 0x%x" % (ins, page)
            print("%6x: %s" % (int(m.group(1), 16), ins))
        print()

main()
