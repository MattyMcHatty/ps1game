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
                     in a file-scope variable (the prop SMDs). Never freed.
  NOT counted        loads that are freed again - listed separately as ROOM-SCOPED
                     so nothing vanishes from the report. That covers the scratch
                     buffers (kitchen_stream_textures, fatdoor's two TIMs, the
                     Greenhouse's and the Chain Room's texture streams) AND the
                     boss models, which are read on entry to the room the boss
                     fights in and freed on the way out. Also not counted:
                     room_arena_load, which reads into a BSS array rather than
                     the heap.

>>> A BOSS MODEL IS ROOM-SCOPED, NOT RESIDENT, AND THAT IS LOAD-BEARING. <<<
The Rabisu's RABISU.SMD + RBSIDLE.PVA are 104,448 bytes and WERE read at startup
and never freed, for a boss that exists in exactly one room. That was the single
largest avoidable item in this budget and it was blocking the second boss from
having a model at all. They are now loaded from main.c's STATE_LOADING when the
destination is the Garden Courtyard and freed on every other transition - the
same move room_arena.c made for room meshes, on the heap rather than in BSS
because unlike a room, NO boss is loaded almost all of the time.
Do not give a new boss a startup load. See src/rabisu.c and
tools/ADDING_THE_ASAG_FIGHT.txt PART 6.

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
kept   = []    # never released: permanent
scoped = []    # released again: transient, and only the PEAK has to fit

# >>> THE TEST IS "DOES THIS MODULE EVER free() THIS POINTER". <<< It used to be
# "...within the next 800 characters", which only recognised a load and a free
# inside one function. That misses the shape that actually matters now: a buffer
# read on ROOM ENTRY and freed on the way out, whose free lives in a separate
# function (rabisus_load_model / rabisus_free_model). Those are not permanent and
# must not be counted as though they were.
#
# The heuristic is deliberately generous - any free of that variable anywhere in
# the file counts - so a module that frees only on an ERROR path would be
# under-reported. Nothing in the tree does that today; if you add one, either
# free it honestly on the success path too or it will flatter this report.
# Nothing DISAPPEARS from the output either way: everything excluded from the
# permanent total is still listed, under ROOM-SCOPED below.
#
# >>> FIXING THIS ALSO CORRECTED A 16,384-BYTE OVER-COUNT THAT HAD ALWAYS BEEN
# HERE. <<< chainlink_door.c's CHNLNK.TIM (10,240) and grinder.c's GRINDER.TIM
# (6,144) are plain local scratch - read, GetTimInfo, LoadImage, free, all inside
# one function - and were never permanent. The old read_file branch checked for
# no free at all, so it counted both. FREE AT REST therefore rises by 16 KB on
# this commit for reasons that have nothing to do with the boss model; if you are
# comparing against an older HEAP_BUDGET.txt, that is where the difference is.
def _frees(src, var):
    return re.search(r'\bfree\s*\(\s*' + re.escape(var) + r'\s*\)', src) is not None

for c in sorted(glob.glob('src/*.c')):
    src = open(c, encoding='utf-8', errors='replace').read()
    mod = os.path.basename(c)
    for m in re.finditer(r'(\w+)\s*=\s*(?:\([\w\s*]*\)\s*)?read_file\(\s*"([^"]+)"', src):
        n, var = basename(m.group(2)), m.group(1)
        if n in disc:
            (scoped if _frees(src, var) else kept).append((n, mod, var))
    for m in re.finditer(r'load_file\(\s*"([^"]+)"\s*,\s*&(\w+)\s*\)', src):
        n, var = basename(m.group(1)), m.group(2)
        if n in disc:
            (scoped if _frees(src, var) else kept).append((n, mod, var))

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

print("ROOM-SCOPED LOADS  (read on entry, freed again - NOT permanent)")
scoped_total = 0
for n, m, var in sorted(scoped, key=lambda r: -rounded(r[0])):
    scoped_total += rounded(n)
    print("  %-24s %-14s %8d  -> %s" % (m, n, rounded(n), var))
if not scoped:
    print("  none")
else:
    print("  %-24s %-14s %8d   <- PEAK, not a resting cost" % ("TOTAL", "", scoped_total))
    print()
    print("  These cost nothing at rest and everything at the moment they are in.")
    print("  The transition that loads them is the peak this heap has to survive,")
    print("  so FREE AT REST must stay comfortably above the largest of them.")
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

biggest = max((rounded(n) for n, _, _ in kept + scoped), default=0)
print("  >>> THIS IS A RESTING TOTAL AND IT DOES NOT MODEL A TRANSITION'S PEAK.")
print("      THAT IS WHAT KILLS YOU, AND IT IS PER-ROOM. <<<")
print("      The GARDEN COURTYARD is the tightest door in the game: it is the")
print("      only transition that loads SND_BANK_BOSS, whose EMERGE clip is a")
print("      71,680-byte malloc — the largest single transient anywhere here —")
print("      and the Rabisu's 104,448-byte model is loaded at the same door.")
print("      Held together that is 176,128 bytes at one instant; main.c now")
print("      SEQUENCES them (free before the bank swap, load after) so the peak")
print("      is 104,448. Work the peak out by hand for any room you touch.")
print("      See tools/DIAGNOSING_A_BOOT_CRASH.txt section 8.")
print()
print("  >>> AND THE ~234 KB CLIFF IN THAT DOCUMENT IS IN OLDER UNITS. <<<")
print("      It was measured when this script still counted chainlink_door.c's")
print("      and grinder.c's TIM scratch as permanent. It no longer does, so the")
print("      figure above is ~16 KB higher than the one that cliff was measured")
print("      against: in today's units it is about 218 KB. Re-measure (section 3")
print("      of that file) before trusting either number.")
print()
print("  The largest single transient allocation still has to fit in what is")
print("  free, and startup makes several: the kitchen's 32768-byte scratch, and")
print("  every read_file above (largest %d bytes). Keep FREE AT REST well clear" % biggest)
print("  of six figures. >>> THIS FIGURE IS A RESTING TOTAL AND FLATTERS YOU. <<<")
print("  The startup PEAK is what collides with the stack, and it is far above")
print("  this: in August 2026 the Chain Room would not boot with 226 KB free at")
print("  rest. The cliff was MEASURED by shrinking BSS a sector at a time until")
print("  it booted - between 233 KB and 235 KB free at rest. Treat anything")
print("  under 256 KB as already in trouble, whatever this line says.")
print()
if heap - perm < 256 * 1024:
    print("  *** WARNING: under 256 KB free. The measured boot cliff is ~234 KB")
    print("      of FREE AT REST. Do not add a registration without")
    print("      reclaiming one first. See the two no-cost patterns in the")
    print("      docstring at the top of this file. ***")
else:
    print("  OK: comfortable margin.")
print()
print("Regenerate with:  python tools/heap_budget.py > tools/HEAP_BUDGET.txt")
