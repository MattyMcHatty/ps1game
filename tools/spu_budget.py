"""SPU RAM budget — tools/ADDING_A_SOUND.txt STEP 3, read from the source.

The hand-kept lists in that STEP go stale every time a bank is added (they
never learned SND_BANK_CATACOMBS). This reads the three things that decide the
answer instead:

    src/sound.c   the file table (enum order, NULL = alias) and sfx_bank[]
    src/sound.h   the SFX_ enum values and the SND_BANK_ bits
    disc.xml      where each .VAG on the disc comes from, for its size

Usage, from the project root:

    py tools/spu_budget.py                          # as the source stands
    py tools/spu_budget.py SFX_TNTCL_DIE:CATACOMBS  # what-if: add a tag first

Cost per clip is its audio size rounded up to 64 (file size less the 48-byte
VAG header). `spare` is the region less the LARGEST bank.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

src = open('src/sound.c').read()
i0 = src.index('"\\\\SND\\\\SWING.VAG')
tbl = src[i0:src.index('};', i0)]
tbl = re.sub(r'/\*.*?\*/', '', tbl, flags=re.S)
files = [b for a, b in re.findall(r'(NULL|"\\\\SND\\\\(\w+)\.VAG;1")', tbl)]

disc = open('disc.xml').read()
source = {n.upper(): s for n, s in
          re.findall(r'name="(\w+)\.VAG"[^>]*source="([^"]+)"', disc, re.I)}

hdr = open('src/sound.h').read()
enum = {k: int(v) for k, v in re.findall(r'(SFX_\w+)\s*=\s*(\d+)', hdr)}
banks = {k: int(v) for k, v in re.findall(r'(SND_BANK_\w+)\s*=\s*(\d+)', hdr)}

tab = re.search(r'sfx_bank\[SFX_COUNT\]\s*=\s*\{(.*?)\n\};', src, re.S).group(1)
tab = re.sub(r'/\*.*?\*/', '', tab, flags=re.S)
tags = {}
for k, v in re.findall(r'\[(SFX_\w+)\]\s*=\s*([^,]+),', tab):
    tags[k] = 0 if 'RESIDENT' in v else sum(
        banks[b] for b in re.findall(r'SND_BANK_\w+', v))
for arg in sys.argv[1:]:
    k, b = arg.split(':')
    tags[k] = tags.get(k, 0) | banks['SND_BANK_' + b]

name_of = {v: k for k, v in enum.items()}


def cost(i):
    return (os.path.getsize(source[files[i].upper()]) - 48 + 63) & ~63


base = 4112
totals = {b: 0 for b in banks}
for i, f in enumerate(files):
    if not f:
        continue                      # an alias slot, no clip of its own
    t = tags.get(name_of.get(i), 0)
    if t == 0:
        base += cost(i)               # resident
    for b, bit in banks.items():
        if t & bit:
            totals[b] += cost(i)

region = 0x80000 - base
print('bank_base 0x%05X  region %d' % (base, region))
for b, v in totals.items():
    print('  %-20s %7d  %s' % (b, v, 'OK' if v <= region else 'OVERFLOW'))
print('spare', region - max(totals.values()))
