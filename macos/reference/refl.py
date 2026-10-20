#!/usr/bin/env python3
"""Find call sites of external symbols in a kext by resolving __auth_stubs
through the dyld bind table.

External calls in a kext go: bl -> __auth_stubs entry -> GOT slot -> symbol.
The stub's `adrp x17, page` + `add x17, x17, #off` gives the GOT slot, and the
bind info maps that slot to a symbol name.

Usage: refl.py <macho> <symbol-substring> [context]
"""
import subprocess, sys, re

OD = "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-objdump"


def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout


def got_to_symbol(path):
    """Map GOT slot address -> symbol name from the bind table."""
    out = {}
    for line in run("otool", "-dyld_info", path).splitlines():
        if " bind " not in line:
            continue
        m = re.search(r"0x([0-9a-fA-F]+)\s", line.split("bind")[0])
        if not m:
            continue
        slot = int(m.group(1), 16)
        sym = line.split()[-1]
        if sym.startswith("__Z") or sym.startswith("_") or "::" in sym:
            out[slot] = sym
        elif "flat-namespace" in line:
            out[slot] = sym
    # ALSO handle c++filt'd renderings by keeping raw names; caller filters.
    return out


def stubs(path):
    """Map stub address -> GOT slot address."""
    txt = run(OD, "-d", "--arch=arm64e", path)
    res = {}
    pending = {}          # stub addr -> page
    for line in txt.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]+\s+adrp\s+x17, 0x([0-9a-f]+)", line)
        if m:
            pending[int(m.group(1), 16)] = int(m.group(2), 16)
            continue
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]+\s+add\s+x17, x17, #0x([0-9a-f]+)", line)
        if m:
            a = int(m.group(1), 16)
            if a - 4 in pending:
                res[a - 4] = pending[a - 4] + int(m.group(2), 16)
    return res, txt


def main():
    path, needle = sys.argv[1], sys.argv[2]
    ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    got2sym = got_to_symbol(path)
    stub2got, txt = stubs(path)

    # stub addr -> symbol
    stub2sym = {}
    for a, g in stub2got.items():
        sym = got2sym.get(g)
        if sym:
            stub2sym[a] = sym

    lines = txt.splitlines()
    # build index of labels
    labels = []           # (line_idx, addr, name)
    for i, ln in enumerate(lines):
        m = re.match(r"^([0-9a-f]{16})\s+<([^>]+)>:", ln)
        if m:
            labels.append((i, int(m.group(1), 16), m.group(2)))

    def enclosing(idx):
        best = None
        for li, addr, name in labels:
            if li <= idx:
                best = (addr, name)
            else:
                break
        return best

    hits = 0
    for i, ln in enumerate(lines):
        m = re.search(r"bl\s+0x([0-9a-f]+)", ln)
        if not m:
            continue
        tgt = int(m.group(1), 16)
        sym = stub2sym.get(tgt)
        if not sym:
            continue
        if needle.lower() not in sym.lower():
            continue
        fn = enclosing(i)
        print(f"\n=== call in {fn[1] if fn else '?'} @ {hex(fn[0]) if fn else '?'}  ->  {sym}")
        for j in range(max(0, i - ctx), min(len(lines), i + 2)):
            print("   ", lines[j].rstrip())
        hits += 1
    print(f"\n[{hits} call site(s) for {needle}]")


main()
