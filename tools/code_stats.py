#!/usr/bin/env python3
"""Report what Korg's compiler generated for Horizon's per-sample loop.

Reads the objdump listing the Korg Makefile writes (microkorg2/build/Horizon.list),
finds the largest loop in the unit (the per-sample loop; it may or may not be
inlined into unit_render) and prints:
static instruction count, inner loops, NEON ops, divides and calls in that loop.
Used by CI; handy locally after ./build.sh too.

    python3 tools/code_stats.py microkorg2/build/Horizon.list
"""
import re
import sys

INSN = re.compile(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{8}|[0-9a-f]{4} [0-9a-f]{4})\s+(\S+)\s*(.*)$')
BRANCH = re.compile(r'^b(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.w|\.n)?$')


def main(path):
    insns = []
    for line in open(path, errors='replace'):
        m = INSN.match(line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
    if not insns:
        print('no code found in listing')
        return 1

    # Loops = backward branches (GNU: "bne 1234 <f+0x10>", LLVM: "bne 0x1234 <...>").
    loops = []
    for addr, op, args in insns:
        if BRANCH.match(op):
            t = re.match(r'(?:0x)?([0-9a-f]+)\b', args)
            if t:
                tgt = int(t.group(1), 16)
                if tgt < addr and addr - tgt < 0x4000:
                    loops.append((tgt, addr))
    if not loops:
        print('no loop found')
        return 1
    start, end = max(loops, key=lambda l: l[1] - l[0])
    body = [i for i in insns if start <= i[0] <= end]
    inner = [l for l in loops if l != (start, end) and start <= l[0] and l[1] <= end]
    neon = sum(1 for _, op, a in body if op.startswith('v') and re.search(r'\b[qd]\d+', a))
    vdiv = sum(1 for _, op, _a in body if op.startswith(('vdiv', 'vsqrt')))
    calls = sorted({re.sub(r'^(?:0x)?[0-9a-f]+\s*', '', a).split(' @')[0] or a for _, op, a in body if op in ('bl', 'blx')})
    print(f'sample loop: {len(body)} instructions (static), {len(inner)} inner loops, '
          f'{neon} NEON ops, {vdiv} divides, calls: {", ".join(calls) if calls else "none"}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'microkorg2/build/Horizon.list'))
