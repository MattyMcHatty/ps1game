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
The Rabisu's RABISU.SMD + RBSIDLE.PVA were 104,448 bytes and WERE read at startup
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

# >>> AND THE STACK IS NOT A FEW KILOBYTES. IT IS 148, BECAUSE main()'s
# RenderContext IS A LOCAL. <<<
#
# src/main.c declares `RenderContext ctx;` on the stack, and a RenderContext is
# two RenderBuffers, each an OT_LENGTH(2048)-entry uint32 table plus a
# BUFFER_LENGTH(65536) packet buffer plus two envs:
#
#     2 * (2048*4 + 65536 + sizeof(DISPENV) + sizeof(DRAWENV))  =  ~147,600
#
# That frame lives at the TOP of RAM for the whole run, so the heap can only
# ever grow to just underneath it. Every figure this script printed before
# September 2026 ignored it and OVERSTATED FREE AT REST BY ~145 KB.
#
# THAT IS NOT A ROUNDING ERROR, IT IS THE REASON THE ~234 KB "CLIFF" IN
# tools/DIAGNOSING_A_BOOT_CRASH.txt EXISTS AT ALL: 234 KB reported was about
# 86 KB real. Asag's arena walked straight into it - 229 KB of boss meshes and
# clips against a reported 361 KB of free heap, which was really far less. The
# console crashed inside CdReadSync with the unaligned-JR signature, because
# malloc handed out the address the stack pointer was already sitting at
# (measured: sp = 0x801DBD40, buffer 0x801D46D0..0x801DE6D0).
#
# MEASURED, not modelled: the number below is $sp read inside read_file() on
# the arena transition, which is as deep as this game's call chain gets. Re-read
# it the same way if main()'s frame ever changes.
STACK_FLOOR = 0x801DBD40   # measured $sp, September 2026 - see above

disc = {}
for f in ET.parse('disc.xml').getroot().iter('file'):
    disc[f.get('name').upper()] = f.get('source')

# >>> ONE MODULE DOES NOT ROUND, AND THE DIFFERENCE IS A WHOLE CLIP. <<<
# A CD read moves whole 2048-byte sectors, so the ordinary read_file() allocates
# whole sectors too. src/asag.c does not any more: it allocates the FILE's size
# and reads the last, partial sector through a shared one-sector scratch, which
# is what bought back the 6,308 bytes its six files were wasting at the top of
# the heap - the faint clip was being refused by 936 of them. See read_file()
# there. Anything else added to this set must do the same thing in the source.
EXACT_SIZE_MODULES = {'asag.c'}

def rounded(name, mod=None):
    """What the allocation actually costs: whole sectors, unless the module
    that reads it sizes its buffer to the file (see EXACT_SIZE_MODULES)."""
    size = os.path.getsize(disc[name])
    if mod in EXACT_SIZE_MODULES:
        return (size + 3) & ~3
    return ((size + 2047) // 2048) * 2048

def basename(path):
    return path.replace('\\\\', '\\').split('\\')[-1].split(';')[0].upper()

# The leading directory is optional and is matched LOOSELY, because there are
# three of them now (\TEX\, \TEXASAG\, \TEXCTCMB\) and a scan that knows only
# the first silently reports nothing the day a chapter gets its own - the same
# quiet under-report the TABLE_DRIVEN_SCOPED note below already had to fix once.
LIT = re.compile(r'"(\\\\(?:[A-Za-z0-9_]+\\\\)?[A-Za-z0-9_ ]+\.(?:TIM|SMD|PVA))(?:;1)?"')

# ---------------------------------------------------------------------------
# 1. texmgr registrations - permanent, and the number that grows per room
# ---------------------------------------------------------------------------
regs     = []  # (disc name, module) - read at startup, resident
deferred = []  # (disc name, module) - registered but NOT read until a chapter door

# >>> DEFERRED REGISTRATIONS COST NOTHING AT REST AND MUST NOT BE COUNTED AS
# THOUGH THEY DID. <<< texmgr_register_deferred() records a name and a group and
# reads no bytes (src/texmgr.h). Chapter 3's art is registered that way, so
# during Chapters 1 and 2 it is worth one array slot apiece and nothing else.
# They are LISTED separately rather than left out, because invisible is how a
# budget tool starts lying: the moment the player walks through the catacomb
# mouth these ARE resident and the 750 KB above them is not.
#
# NOTE the deferred scan runs BEFORE the plain-register one and the two are
# mutually exclusive by construction: 'texmgr_register(' does not match
# 'texmgr_register_deferred(' because of the open paren.
for c in sorted(glob.glob('src/*.c')):
    src = open(c, encoding='utf-8', errors='replace').read()
    mod = os.path.basename(c)
    for m in re.finditer(r'texmgr_register_deferred\(\s*"([^"]+)"', src):
        deferred.append((basename(m.group(1)), mod))
    if re.search(r'texmgr_register_deferred\(\s*(?:new_tex|shared_tex|raf_tex)', src):
        for m in re.finditer(
                r'(?:new_tex|shared_tex|raf_tex)[a-z_]*\[[^\]]*\]\s*=\s*\{(.*?)\};',
                src, re.S):
            for lit in LIT.findall(m.group(1)):
                deferred.append((basename(lit), mod))
        continue
    for m in re.finditer(r'texmgr_register\(\s*"([^"]+)"', src):
        regs.append((basename(m.group(1)), mod))
    if re.search(r'texmgr_register\(\s*(?:new_tex|shared_tex|raf_tex)', src):
        for m in re.finditer(
                r'(?:new_tex|shared_tex|raf_tex)[a-z_]*\[[^\]]*\]\s*=\s*\{(.*?)\};',
                src, re.S):
            for lit in LIT.findall(m.group(1)):
                regs.append((basename(lit), mod))
regs     = [(n, m) for n, m in regs     if n in disc]
deferred = [(n, m) for n, m in deferred if n in disc]

# ---------------------------------------------------------------------------
# 2. buffers whose pointer is kept - permanent
# ---------------------------------------------------------------------------
kept    = []   # never released: permanent
scoped  = []   # released again on a room change: transient, only the PEAK counts
chapter = []   # held through Chapters 1-2, freed at the catacomb mouth

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

# >>> AND A CHAPTER-SCOPED MODULE STILL HAS ORDINARY SCRATCH IN IT. <<<
# fatdoor.c holds its door model for the whole of Chapters 1 and 2 AND reads a
# 34 KB TIM that it frees three lines later inside the same function; so do
# chainlink_door.c and grinder.c. Tagging the whole MODULE chapter-scoped
# counted 61 KB of that scratch as permanent, which is over-reporting in the one
# direction this file must never over-report - it is the number a transition's
# headroom gets sized against.
#
# So the test is per-BUFFER, not per-module: a load is chapter-scoped only if
# the module's own *_free_assets() is what releases it. Everything else in the
# file falls through to the usual kept / room-scoped split.
def _free_assets_body(src):
    """The text of this module's *_free_assets(), or '' if it has none."""
    m = re.search(r'^void\s+\w*_free_assets\(void\)\s*\{(.*?)^\}',
                  src, re.S | re.M)
    return m.group(1) if m else ''


# >>> AND A MODULE THAT LOADS FROM A TABLE IS INVISIBLE TO BOTH PATTERNS. <<<
# Both regexes below want the FILENAME to appear as a literal argument at the
# call site. src/asag.c does not work that way: it reads twenty-two files
# through two const tables (part_def[] and clip_def[]) and one read_file(
# def->file ) call, so a scan for read_file("...") finds nothing and 204 KB of
# room-scoped loads would VANISH from this report - which is exactly the thing
# the note above promises never happens.
#
# Rather than teach the regex to follow a table, this is an explicit opt-in:
# name the module and the function that releases everything it read, and every
# disc filename that appears as a literal anywhere in it is counted as
# ROOM-SCOPED. That is honest for a module whose ONLY reads are the table's -
# check that before adding a row here, and check the named free really does
# release all of them.
#
# The scan is by BASENAME against disc.xml rather than by matching a path shape.
# An earlier version matched the path, and it silently reported nothing the day
# Asag's files moved from \TEX\ to \TEXASAG\ - which is the same class of quiet
# under-report this whole block exists to fix.
TABLE_DRIVEN_SCOPED = {
    'asag.c': 'asags_free_model',   # 8 part meshes + 14 .pva clips
}

# >>> AND A THIRD LIFETIME EXISTS NOW: CHAPTER-SCOPED. <<< The prop models
# below are read at STARTUP and held for the whole of Chapters 1 and 2, and
# then freed in one go at the catacomb mouth by area_bank_sync() (src/area_bank.h).
# The _frees() heuristic sees the free and would file them under ROOM-SCOPED,
# which is the wrong answer in the budget that binds: for ~everything the
# player does they are as permanent as a texmgr registration, and the door
# they are freed at is the one door where the heap has room to spare.
#
# So they are counted in the PERMANENT total and printed under a heading of
# their own, with what Chapter 3 gets back stated next to it. A module listed
# here must free the buffer ONLY from its *_free_assets(), never on a room
# transition - otherwise this over-reports, which is the safe direction but
# still a lie.
CHAPTER_SCOPED_MODULES = {
    'concrete_props.c', 'dining_table.c', 'piano_props.c', 'trick_drawers.c',
    'valve_handle.c', 'vines.c', 'chainlink_door.c', 'dresser.c',
    'fatdoor.c', 'grinder.c', 'lever.c',
}

for c in sorted(glob.glob('src/*.c')):
    src = open(c, encoding='utf-8', errors='replace').read()
    mod = os.path.basename(c)
    if mod in TABLE_DRIVEN_SCOPED:
        freer = TABLE_DRIVEN_SCOPED[mod]
        assert ('void ' + freer) in src, \
            '%s: %s() is gone - re-check what frees its table loads' % (mod, freer)
        for m in re.finditer(r'"([^"]*);1"', src):
            n = basename(m.group(1))
            if n in disc:
                scoped.append((n, mod, freer + '()'))
        continue
    chap = mod in CHAPTER_SCOPED_MODULES
    chap_body = _free_assets_body(src) if chap else ''
    seen = set()
    for m in re.finditer(r'(\w+)\s*=\s*(?:\([\w\s*]*\)\s*)?read_file\(\s*"([^"]+)"', src):
        n, var = basename(m.group(2)), m.group(1)
        if n in disc and (n, var) not in seen:
            seen.add((n, var))
            bucket = (chapter if _frees(chap_body, var)
                      else (scoped if _frees(src, var) else kept))
            bucket.append((n, mod, var))
    for m in re.finditer(r'load_file\(\s*"([^"]+)"\s*,\s*&(\w+)\s*\)', src):
        n, var = basename(m.group(1)), m.group(2)
        if n in disc and (n, var) not in seen:
            seen.add((n, var))
            bucket = (chapter if _frees(chap_body, var)
                      else (scoped if _frees(src, var) else kept))
            bucket.append((n, mod, var))

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

print("DEFERRED REGISTRATIONS  (registered at startup, NOT read until a chapter door)")
def_total = 0
for n, m in sorted(deferred, key=lambda r: (r[1], r[0])):
    def_total += rounded(n, m)
    print("  %-24s %-14s %8d" % (m, n, rounded(n, m)))
if not deferred:
    print("  none")
else:
    print("  %-24s %-14s %8d   <- ZERO at rest; this much once loaded"
          % ("TOTAL", "", def_total))
    print()
    print("  These are Chapter 3's (src/area_bank.h). They cost one array slot each")
    print("  and no bytes while the player is anywhere in the mansion or the garden.")
    print("  area_bank_sync() reads them at the catacomb mouth, in the same breath as")
    print("  it frees the registrations and the chapter-scoped buffers above - so")
    print("  the two totals are ALTERNATIVES and are never both resident.")
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
for n, m, var in sorted(kept, key=lambda r: -rounded(r[0], r[1])):
    kept_total += rounded(n, m)
    print("  %-24s %-14s %8d  -> %s" % (m, n, rounded(n, m), var))
print("  %-24s %-14s %8d" % ("TOTAL", "", kept_total))
print()

print("CHAPTER-SCOPED BUFFERS  (held through Chapters 1-2, freed at the catacomb mouth)")
chapter_total = 0
for n, m, var in sorted(chapter, key=lambda r: -rounded(r[0], r[1])):
    chapter_total += rounded(n, m)
    print("  %-24s %-14s %8d  -> %s" % (m, n, rounded(n, m), var))
if not chapter:
    print("  none")
else:
    print("  %-24s %-14s %8d   <- COUNTED AS PERMANENT" % ("TOTAL", "", chapter_total))
    print()
    print("  Prop models, resident from startup for all of Chapters 1 and 2 and")
    print("  freed in one go by area_bank_sync() (src/area_bank.h). They are counted in")
    print("  the permanent total because that is what they are for ~everything the")
    print("  player does; Chapter 3 gets them back on top of the registrations.")
print()

print("ROOM-SCOPED LOADS  (read on entry, freed again - NOT permanent)")
scoped_total = 0
for n, m, var in sorted(scoped, key=lambda r: -rounded(r[0], r[1])):
    scoped_total += rounded(n, m)
    print("  %-24s %-14s %8d  -> %s" % (m, n, rounded(n, m), var))
if not scoped:
    print("  none")
else:
    print("  %-24s %-14s %8d   <- PEAK, not a resting cost" % ("TOTAL", "", scoped_total))
    print()
    print("  These cost nothing at rest and everything at the moment they are in.")
    print("  The transition that loads them is the peak this heap has to survive,")
    print("  so FREE AT REST must stay comfortably above the largest of them.")
print()

# THE PEAK BANK, from tools/check_tex_banks.py, which is the single source of
# truth for which module's textures are resident where. Registrations stopped
# being permanent in September 2026: they are read when their AREA is entered
# and freed when it is left (src/texmgr.h), so what this budget charges is the
# LARGEST bank, not the sum of all of them. Imported rather than re-derived so
# the two cannot drift - if the checker is unhappy, this report is wrong too.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_tex_banks as CTB
_fm, _fc, _fu, _decl = CTB.scan()
_msz = CTB.module_bytes()
bank_sizes = {}
for _area, _fns in CTB.AREAS.items():
    _mods = CTB.closure(_fns, _fm, _fc, _fu)
    bank_sizes[_area] = sum(_msz.get(m, 0) for m in _mods)
peak_bank_area = max(bank_sizes, key=lambda a: bank_sizes[a])
peak_bank = bank_sizes[peak_bank_area]

out = subprocess.run([NM, ELF], capture_output=True, text=True).stdout
end = [int(l.split()[0], 16) & 0xFFFFFFFF for l in out.splitlines()
       if len(l.split()) == 3 and l.split()[2] == '_end']
if not end:
    sys.exit("could not find _end in " + ELF + " - build first")
end = end[0]
heap = STACK_FLOOR - end   # NOT HEAP_TOP: the top 145 KB is main()'s stack frame
perm = peak_bank + kept_total + chapter_total

print("THE HEAP")
print("  _end (heap start)   0x%08X" % end)
print("  heap top            0x%08X   <- InitHeap; the STACK starts at 0x%08X"
      % (HEAP_TOP, STACK_TOP))
print("  heap size           %8d bytes (%.0f KB)" % (heap, heap / 1024.0))
print("  stack floor         0x%08X   <- MEASURED $sp; main()'s RenderContext"
      % STACK_FLOOR)
print("  stack reserve       %8d bytes (%.0f KB)  <- NOT usable heap"
      % (HEAP_TOP - STACK_FLOOR, (HEAP_TOP - STACK_FLOOR) / 1024.0))
print()
print("  texmgr peak bank    %8d bytes (%.0f KB)  %4.1f%%   <- %s" % (peak_bank, peak_bank / 1024.0, 100.0 * peak_bank / heap, peak_bank_area))
print("  kept buffers        %8d bytes (%.0f KB)  %4.1f%%" % (kept_total, kept_total / 1024.0, 100.0 * kept_total / heap))
print("  chapter-scoped      %8d bytes (%.0f KB)  %4.1f%%"
      % (chapter_total, chapter_total / 1024.0, 100.0 * chapter_total / heap))
print("  PERMANENT TOTAL     %8d bytes (%.0f KB)  %4.1f%%" % (perm, perm / 1024.0, 100.0 * perm / heap))
print("  FREE AT REST        %8d bytes (%.0f KB)  %4.1f%%   <- STACK ALREADY SUBTRACTED"
      % (heap - perm, (heap - perm) / 1024.0, 100.0 * (heap - perm) / heap))
print("                                  (was %d before the stack was counted;"
      % (heap + (HEAP_TOP - STACK_FLOOR) - perm))
print("                                   THAT figure is what the old cliff was measured in)")
print()

biggest = max((rounded(n, m) for n, m, _ in kept + scoped), default=0)
print("  >>> THIS IS A RESTING TOTAL AND IT DOES NOT MODEL A TRANSITION'S PEAK.")
print("      THAT IS WHAT KILLS YOU, AND IT IS PER-ROOM. <<<")
print("      The GARDEN COURTYARD is the tightest door in the game: it is the")
print("      only transition that loads SND_BANK_BOSS, whose EMERGE clip is a")
print("      71,680-byte malloc — the largest single transient anywhere here —")
print("      and the Rabisu's model is loaded at the same door. Held together")
print("      they would be 176,128 bytes at one instant; main.c SEQUENCES them")
print("      (free before the bank swap, load after) so the peak is the model")
print("      alone. Work the peak out by hand for any room you touch.")
print("      See tools/DIAGNOSING_A_BOOT_CRASH.txt section 8.")
print()
print("  >>> AND IN SEPTEMBER 2026 THAT DOOR STOPPED FITTING. IT IS MEASURED")
print("      NOW, AND THE NUMBERS ARE THESE. <<< Asag's fight and its ending")
print("      moved _end up under a heap with no margin left. Nothing about the")
print("      courtyard changed; its 104,448-byte model simply stopped fitting")
print("      in what was under the stack, malloc handed out stack memory as it")
print("      always does, and CdRead DMA'd the clip through a return address:")
print("      the room went black and never loaded.")
print("          run under the stack at that door   91,992 bytes  (MEASURED)")
print("          asked for, before                 104,448       CRASH")
print("          asks for now                       89,984       fits")
print("      The clip is PVA2 (packed, a quarter smaller: tools/pack_pva.py),")
print("      which bought 18,848 bytes, and 3,968 of those went back on an")
print("      unpack scratch. THAT LEAVES ABOUT 2 KB OF SLACK AT THIS DOOR, so")
print("      it is the first thing to re-measure after anything that moves")
print("      _end. src/rabisu.c's read_file now REFUSES a read that would")
print("      cross $sp, so the next time this runs out it is a boss holding")
print("      its bind pose — or not drawn at all — and not a wild jump.")
print("      Method: force-boot the room in a headless build and print each")
print("      buffer against $sp. See src/rabisu.c and PART 6 of")
print("      tools/ADDING_THE_ASAG_FIGHT.txt.")
print()
print("  >>> AND THE ~234 KB CLIFF IN THAT DOCUMENT WAS A UNITS BUG. <<<")
print("      It was measured against a FREE AT REST that did not subtract")
print("      main()'s 145 KB RenderContext stack frame, and was ~16 KB out on")
print("      top of that for the TIM-scratch miscount. In the units printed")
print("      above, that cliff is about 74 KB - i.e. it was simply the heap")
print("      being full, not a threshold. Asag's arena proved it in September")
print("      2026: 229 KB of boss loads against a reported 361 KB free crashed")
print("      inside CdReadSync with $sp INSIDE the buffer being read. See")
print("      tools/ADDING_THE_ASAG_FIGHT.txt PART 6A.")
print()
print("  The largest single transient allocation still has to fit in what is")
print("  free, and startup makes several: the kitchen's 32768-byte scratch, and")
print("  every read_file above (largest %d bytes). Keep FREE AT REST well clear" % biggest)
print("  of six figures. >>> AND IT IS STILL A RESTING TOTAL. <<< A transition")
print("  can hold several of the loads above at once, and the room whose door")
print("  is tightest is not the room you are editing. Work the peak out by")
print("  hand: permanent, plus everything live at that one instant.")
print("  MEASURED at the tightest door in the game (Asag's arena, where the")
# >>> THE DOOR FIGURE IS NOW AN ADDRESS, NOT A BRACKET, AND IT IS HALF WHAT
# THIS SCRIPT PRINTS. <<< The first Asag inferred "~171 KB" and it was quoted
# for months without being re-measured. Asag Version Two bracketed it to
# [121 KB, 147 KB) from which reads succeeded and which one was refused. In
# September 2026 it was pinned exactly, by probing malloc on the arena
# transition in a headless run:
#
#     a 94,208-byte probe landed at 0x801C3200      <- the allocator's top
#     $sp inside read_file()      was 0x801DBD50
#
# so the CONTIGUOUS run under the stack is about 101 KB, and about 93 KB once
# read_file()'s 8 KB margin comes off. Everything below that top is either live
# or in holes too small to take a clip: a 4,096-byte probe found one at
# 0x801BB1B8, a 94,208-byte one did not. THE BRACKET WAS NOT WRONG, IT WAS
# MEASURING SOMETHING ELSE - cumulative bytes read, some of which went into
# those low holes.
#
# (Measured booting STRAIGHT INTO the arena, which is the kindest case: a real
# transition arrives with the previous room's frees still fragmenting the heap.
# Treat ~93 KB as an upper bound on what a clip load can have.)
#
# WHAT IT COST THE SECOND TIME. Asag's six files wasted 6,308 bytes in
# sector-rounding, all of it at the top, and the faint - the last read - was
# refused by 936 BYTES. Silently, again: no crash, no log, a boss that stood on
# its bind pose for one second where a five-second faint belonged. src/asag.c's
# read_file() now sizes the buffer to the FILE and reads the partial last sector
# through a shared scratch, which is where EXACT_SIZE_MODULES at the top of this
# script comes from.
print("  boss model is read): about 101 KB contiguous under the stack, 93 KB")
print("  once read_file's margin comes off - against the %d KB printed above." % ((heap - perm) // 1024))
print("  The difference is other transients and holes too small to reuse.")
print("  Treat anything under 128 KB free at rest as already in trouble.")
print()
if heap - perm < 128 * 1024:
    print("  *** WARNING: under 128 KB free at rest, with the stack already")
    print("      subtracted. Do not add a registration without reclaiming one")
    print("      first. See the two no-cost patterns in the docstring at the")
    print("      top of this file. ***")
else:
    print("  OK: comfortable margin.")
print()
print("Regenerate with:  python tools/heap_budget.py > tools/HEAP_BUDGET.txt")
