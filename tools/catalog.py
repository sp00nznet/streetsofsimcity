#!/usr/bin/env python3
"""Function catalog for Streets.exe (Streets of SimCity): pcrecomp disasm32.

    py -3 tools/catalog.py            # -> work/functions.json

The entry point is the seed; the rest is recursive descent, prologues and a
data scan, plus two kinds of extra seed:

* **Jump tables in .data.** The software rasterizer's span routines draw
  through unrolled loops, entered part-way with `jmp [reg*4 + table]`, the
  table 32 addresses inside the loop in initialised .data (0x00688F4C for the
  routine at 0x004C3890). Nothing branches to those addresses directly, so the
  loop was never reached, never lifted, and the jump went nowhere
  (docs/bringup.md, "the rasterizer"). Every run of 8 or more consecutive
  .data dwords pointing into .text is read from the exe here and seeded.
* **work/seeds.json**, addresses earlier runs found (indirect calls into code
  the scans missed), kept by pcrecomp seed_from_log.py:

      py -3 ../pcrecomp/tools/disasm/seed_from_log.py scratch/run.log game/Streets.exe --seeds work/seeds.json

Both are derived from the game binary, so they live in work/ and are never
committed.
"""
import json
import os
import struct
import subprocess
import sys

_HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'pcrecomp')), 'tools')
GAME = os.path.join(_HERE, 'game')
WORK = os.path.join(_HERE, 'work')
EXE = os.path.join(GAME, 'Streets.exe')
SEEDS = os.path.join(WORK, 'seeds.json')
ALL_SEEDS = os.path.join(WORK, 'seeds_all.json')
MIN_RUN = 8


def table_seeds(path):
    """Targets of every run of MIN_RUN+ .data dwords that all point into .text."""
    sys.path.insert(0, os.path.join(TOOLS, 'pe'))
    from pe_analyze import analyze_pe
    info = analyze_pe(path)
    raw = open(path, 'rb').read()
    sec = {s.name: s for s in info.sections}
    text, data = sec['.text'], sec['.data']
    lo = info.image_base + text.virtual_address
    hi = lo + text.virtual_size
    d = raw[data.raw_offset:data.raw_offset + data.raw_size]
    seeds, i = set(), 0
    while i + 4 <= len(d):
        j = i
        while j + 4 <= len(d) and lo <= struct.unpack_from('<I', d, j)[0] < hi:
            j += 4
        if (j - i) // 4 >= MIN_RUN:
            seeds.update(struct.unpack_from('<%dI' % ((j - i) // 4), d, i))
            i = j
        else:
            i += 4
    return seeds


def main():
    os.makedirs(WORK, exist_ok=True)        # gitignored, so absent in a fresh copy
    seeds = table_seeds(EXE)
    print('[catalog] %d addresses from code-pointer tables in .data' % len(seeds))
    if os.path.exists(SEEDS):
        seeds.update(int(a, 0) if isinstance(a, str) else a for a in json.load(open(SEEDS)))
    json.dump(['0x%08X' % a for a in sorted(seeds)], open(ALL_SEEDS, 'w'), indent=0)
    subprocess.run([sys.executable, os.path.join(TOOLS, 'disasm', 'disasm32.py'), EXE,
                    '-o', os.path.join(WORK, 'functions.json'), '--seed-functions', ALL_SEEDS], check=True)


if __name__ == '__main__':
    main()
