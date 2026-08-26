#!/usr/bin/env python3
"""
heap_budget.py - what is permanently in MAIN RAM, and how much is left.

    python tools/heap_budget.py > tools/HEAP_BUDGET.txt

WHY THIS EXISTS
---------------
tools/vram_map.py is the single source of truth for VRAM. Until August 2026 there
was no equivalent for main RAM, and there needed to be: adding the Greenhouse
pushed the permanent heap total past the end of the heap, and the console stopped
booting. The failure was not a tidy "malloc returned NULL" - see below - and it
surfaced in a function with nothing to do with the room that caused it, so there
was nothing to read off the crash that pointed anywhere useful.

>>> THE HEAP TOP IS THE STACK. THERE IS NO GUARD. <<<
PSn00bSDK's _start_inner calls InitHeap(_end, 0x801FFFF8 - _end): the heap is
every byte from the end of BSS to the top of RAM. The BIOS stack pointer starts
at 0x801FFF00 and grows DOWN into that same region. Nothing separates them. So
the failure mode when the heap fills is NOT an allocation failure the callers
handle - malloc SUCCEEDS, hands back memory the stack is already using, and the
next CdRead DMAs straight over the return address. The console then jumps into
nothing, which PCSX-Redux reports as "Unrecoverable error while running
recompiler" at whatever PC it last had.

WHAT COUNTS AS PERMANENT
------------------------
  texmgr_register()  keeps the WHOLE TIM in RAM for the life of the run so a room
                     entry can be a pure LoadImage. Never freed. This is by far
                     the biggest consumer and the one that grows per room.
  kept buffers       a model or clip read once at startup whose pointer is stashed
                     in a file-scope variable (the prop SMDs, the Rabisu's idle
                     clip). Never freed.
  NOT counted        loads that are freed again (kitchen_stream_textures' scratch,
                     fatdoor's two TIM buffers, greenhouse_upload_textures'
                     scratch), and room_arena_load, which reads into a BSS array
                     rather than the heap.

The transient allocations still have to FIT in whatever is left, so treat the
"free at rest" figure as the budget, not as slack.

THE TWO WAYS TO ADD A TEXTURE AND SPEND NOTHING HERE
----------------------------------------------------
  1. A narrow *_upload_x() on the module that already owns the texture, and
     TIM_SLOT() for the header. See hall_2f_upload_strs.
  2. Stream it on entry into a scratch buffer that is freed again, bracketed by
     cdaudio_suspend/resume. See greenhouse_upload_textures - the first room to
     own art and register nothing.
"""
import re, os, sys, glob, subprocess
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

ELF  = 'build/HORROR.exe'
NM   = r'C:/Users/virtu/Documents/PSn00bSDK/bin/mipsel-none-elf-nm.exe'
HEAP_TOP  = 0x801FFFF8   # what _start_inner passes to InitHeap
STACK_TOP = 0x801FFF00   # BIOS default SP, grows down into the same region

disc = {}
for f in ET.parse('disc.xml').getroot().iter('file'):
    disc[f.get('name').upper()] = f.get('source')

def rounded(name):
    """A CD read always allocates whole 2048-byte sectors."""
    return ((os.path.getsize(disc[name]) + 2047) // 2048) * 2048

def basename(path):
    return path.replace('\\\\', '\\').split('\\')[-1].split(';')[0].upper()

LIT = re.compile(r'"(\\\\(?:TEX\\\\)?[A-Za-z0-9_ ]+\.(?:TIM|SMD|PVA))(?:;1)?"')

# ---------------------------------------------------------------------------
# 1. texmgr registrations - permanent, and the number that grows per room
# ---------------------------------------------------------------------------
regs = []      # (disc name, module)
for c in sorted(glob.glob('src/*.c')):
    src = open(c, encoding='utf-8', errors='replace').read()
    mod = os.path.basename(c)
    for m in re.finditer(r'texmgr_register\(\s*"([^"]+)"', src):
        regs.append((basename(m.group(1)), mod))
    if re.search(r'texmgr_register\(\s*(?:new_tex|shared_tex|raf_tex)', src):
        for m in re.finditer(
                r'(?:new_tex|shared_tex|raf_tex)[a-z_]*\[[^\]]*\]\s*=\s*\{(.*?)\};',
                src, re.S):
            for lit in LIT.findall(m.group(1)):
                regs.append((basename(lit), mod))
regs = [(n, m) for n, m in regs if n in disc]

# ---------------------------------------------------------------------------
# 2. buffers whose pointer is kept - permanent
# ---------------------------------------------------------------------------
kept = []
for c in sorted(glob.glob('src/*.c')):
    src = open(c, encoding='utf-8', errors='replace').read()
    mod = os.path.basename(c)
    for m in re.finditer(r'(\w+)\s*=\s*(?:\([\w\s*]*\)\s*)?read_file\(\s*"([^"]+)"', src):
        n = basename(m.group(2))
        if n in disc:
            kept.append((n, mod, m.group(1)))
    for m in re.finditer(r'load_file\(\s*"([^"]+)"\s*,\s*&(\w+)\s*\)', src):
        n, var = basename(m.group(1)), m.group(2)
        if n in disc and ('free(' + var) not in src[m.end():m.end() + 800]:
            kept.append((n, mod, var))

# ---------------------------------------------------------------------------
print("=" * 78)
print(" MAIN RAM BUDGET   generated by tools/heap_budget.py")
print("=" * 78)
print(__doc__.split('WHY THIS EXISTS')[1].split('THE TWO WAYS')[0].rstrip())
print()

print("TEXMGR REGISTRATIONS  (resident for the whole run)")
print("  %-24s %-5s %9s" % ("module", "regs", "bytes"))
print("  " + "-" * 44)
by_mod = {}
for n, m in regs:
    by_mod.setdefault(m, []).append(n)
reg_total = 0
for m in sorted(by_mod, key=lambda k: -sum(rounded(n) for n in by_mod[k])):
    b = sum(rounded(n) for n in by_mod[m])
    reg_total += b
    print("  %-24s %-5d %9d" % (m, len(by_mod[m]), b))
print("  %-24s %-5d %9d" % ("TOTAL", len(regs), reg_total))
print()

# duplicates: the same TIM held more than once
seen = {}
for n, m in regs:
    seen.setdefault(n, []).append(m)
dupes = {n: ms for n, ms in seen.items() if len(ms) > 1}
print("DUPLICATE REGISTRATIONS  (the same TIM held in RAM more than once)")
if dupes:
    waste = 0
    for n in sorted(dupes):
        w = rounded(n) * (len(dupes[n]) - 1)
        waste += w
        print("  %-16s %7d wasted   %s" % (n, w, ", ".join(sorted(set(dupes[n])))))
    print("  ---> reclaimable with narrow uploaders: %d bytes (%.0f KB)"
          % (waste, waste / 1024.0))
    print("       Each of these is one texture at one VRAM address, so ONE copy")
    print("       serves every room that draws it; the others want TIM_SLOT() for")
    print("       the header and a call to the owner's narrow upload function.")
else:
    print("  none")
print()

print("KEPT BUFFERS  (read once, pointer stashed, never freed)")
kept_total = 0
for n, m, var in sorted(kept, key=lambda r: -rounded(r[0])):
    kept_total += rounded(n)
    print("  %-24s %-14s %8d  -> %s" % (m, n, rounded(n), var))
print("  %-24s %-14s %8d" % ("TOTAL", "", kept_total))
print()

out = subprocess.run([NM, ELF], capture_output=True, text=True).stdout
end = [int(l.split()[0], 16) & 0xFFFFFFFF for l in out.splitlines()
       if len(l.split()) == 3 and l.split()[2] == '_end']
if not end:
    sys.exit("could not find _end in " + ELF + " - build first")
end = end[0]
heap = HEAP_TOP - end
perm = reg_total + kept_total

print("THE HEAP")
print("  _end (heap start)   0x%08X" % end)
print("  heap top            0x%08X   <- InitHeap; the STACK starts at 0x%08X"
      % (HEAP_TOP, STACK_TOP))
print("  heap size           %8d bytes (%.0f KB)" % (heap, heap / 1024.0))
print()
print("  texmgr resident     %8d bytes (%.0f KB)  %4.1f%%" % (reg_total, reg_total / 1024.0, 100.0 * reg_total / heap))
print("  kept buffers        %8d bytes (%.0f KB)  %4.1f%%" % (kept_total, kept_total / 1024.0, 100.0 * kept_total / heap))
print("  PERMANENT TOTAL     %8d bytes (%.0f KB)  %4.1f%%" % (perm, perm / 1024.0, 100.0 * perm / heap))
print("  FREE AT REST        %8d bytes (%.0f KB)  %4.1f%%" % (heap - perm, (heap - perm) / 1024.0, 100.0 * (heap - perm) / heap))
print()

biggest = max((rounded(n) for n, _, _ in kept), default=0)
print("  The largest single transient allocation still has to fit in what is")
print("  free, and startup makes several: the kitchen's 32768-byte scratch, and")
print("  every read_file above (largest %d bytes). Keep FREE AT REST well clear" % biggest)
print("  of six figures - the crash in August 2026 happened at about 140 KB.")
print()
if heap - perm < 160 * 1024:
    print("  *** WARNING: under 160 KB free. Do not add a registration without")
    print("      reclaiming one first. See the two no-cost patterns in the")
    print("      docstring at the top of this file. ***")
else:
    print("  OK: comfortable margin.")
print()
print("Regenerate with:  python tools/heap_budget.py > tools/HEAP_BUDGET.txt")
