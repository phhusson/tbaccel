#!/usr/bin/env python3
"""Decode PAC-obfuscated virtual calls (blraa/braa) in a Mach-O kext function.

Recovers: vtable offset (captured at `add x16, x16, x17`), receiver expr (x0),
and prints static calls by name.

Usage: vcall.py <macho> <symbol-substring> [max-insns]
"""
import subprocess, sys, re, bisect

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def dem(n):
    return subprocess.run(["c++filt"], input=n, capture_output=True, text=True).stdout.strip()

def load(macho):
    syms = []
    for line in run("nm", "-n", macho).splitlines():
        p = line.split()
        if len(p) >= 3:
            try: syms.append((int(p[0], 16), p[2], p[1]))
            except ValueError: pass
    syms.sort()
    return syms

def main():
    macho, needle = sys.argv[1], sys.argv[2]
    maxi = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
    syms = load(macho)
    A = [a for a, _, _ in syms]

    def res(a):
        i = bisect.bisect_right(A, a) - 1
        if i < 0: return None
        b, n, t = syms[i]
        if t.lower() not in ("t", "s"): return None
        return (dem(n), a - b)

    funcs = [(a, n) for a, n, t in syms if t.lower() == "t" and (needle in n or needle in dem(n))]
    funcs = [f for f in funcs if "_os_log_fmt" not in f[1] and "cold." not in f[1]][:1]
    if not funcs:
        print("no match"); sys.exit(1)
    addr = funcs[0][0]
    i = bisect.bisect_right(A, addr)
    stop = min(syms[i][0] if i < len(syms) else addr + maxi * 4, addr + maxi * 4)

    dis = run("objdump", "-d", "--no-show-raw-insn",
              "--start-address=0x%x" % addr, "--stop-address=0x%x" % stop, macho)
    reg, imm, pend = {}, {}, [None]
    print("### %s @0x%x" % (dem(funcs[0][1]), addr))
    for line in dis.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
        if not m: continue
        ins = re.sub(r"\s+", " ", m.group(2).strip())
        rr = lambda r: reg.get(r, r)

        mm = re.match(r"mov ([wx]\d+), #(0x[0-9a-f]+|\d+)(?:\s*;.*)?$", ins)
        if mm:
            imm[mm.group(1)] = int(mm.group(2), 16) if mm.group(2).startswith("0x") else int(mm.group(2)); continue
        mm = re.match(r"movk ([wx]\d+), #(0x[0-9a-f]+), lsl #(\d+)", ins)
        if mm:
            r, v, s = mm.group(1), int(mm.group(2), 16), int(mm.group(3))
            imm[r] = imm.get(r, 0) | (v << s); continue
        mm = re.match(r"mov (x\d+), (x\d+|sp|fp|lr)$", ins)
        if mm:
            reg[mm.group(1)] = rr(mm.group(2)); continue
        mm = re.match(r"ldr (x\d+), \[(x\d+|sp)(?:, #(0x[0-9a-f]+|\d+))?\]$", ins)
        if mm:
            base = rr(mm.group(2)); off = mm.group(3)
            o = "+0x%x" % (int(off, 16) if off and off.startswith("0x") else int(off)) if off else ""
            reg[mm.group(1)] = "[%s%s]" % (base, o); continue
        mm = re.match(r"add (x\d+), (x\d+|sp), (x17)$", ins)
        if mm:
            pend[0] = imm.get("x17"); continue
        mm = re.match(r"add (x\d+), (x\d+|sp), #(0x[0-9a-f]+|\d+)$", ins)
        if mm:
            reg[mm.group(1)] = "%s+0x%x" % (rr(mm.group(2)), int(mm.group(3), 16) if mm.group(3).startswith("0x") else int(mm.group(3))); continue
        mm = re.match(r"adrp (x\d+), (0x[0-9a-f]+)$", ins)
        if mm:
            reg[mm.group(1)] = "page(0x%s)" % mm.group(2)[2:]; continue
        mm = re.match(r"(blraa|braa) (x\d+),", ins)
        if mm:
            print("  VCALL  off=%s  recv=%s" % (("0x%x" % pend[0]) if pend[0] is not None else "?", rr("x0")))
            pend[0] = None; imm.pop("x17", None); continue
        mm = re.match(r"(bl|b) (0x[0-9a-f]+)", ins)
        if mm and mm.group(1) == "bl":
            r = res(int(mm.group(2), 16))
            if r and r[0].startswith("IOThunderbolt"): print("  CALL   %s" % r[0])
            continue

main()
