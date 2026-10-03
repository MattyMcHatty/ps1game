"""
texmgr_count.py - find every texmgr_register() call in src/, statically.

Shared by tools/heap_budget.py (which charges each registration to the heap) and
tools/check_texmgr_cap.py (which fails the build when there are more of them
than src/texmgr.c's TEXMGR_MAX). One scanner, so the two can never disagree
about the count.

WHAT IT RECOGNISES
  texmgr_register("\\DIR\\NAME.TIM;1")         a literal at the call
  texmgr_register(new_tex[i].file) etc.        a loop over a file-scope table
                                               named new_tex*/shared_tex*/raf_tex*;
                                               every literal in the table counts
Only names that disc.xml ships are kept, so a path that is not on the disc (a
typo, or a call in a dead #if) is not counted.

IT DOES NOT STRIP COMMENTS, which errs on the side of OVER-counting: a commented
example call is a false positive, never a false negative. A registration made
any other way (a path built at run time, a table with another name) is a false
NEGATIVE, and that is what main()'s texmgr_refused() boot check is the backstop
for. Extend the patterns here if you add such a call.
"""
import glob
import os
import re
import xml.etree.ElementTree as ET


def basename(path):
    return path.replace('\\\\', '\\').split('\\')[-1].split(';')[0].upper()


# The leading directory is optional and is matched LOOSELY, because there are
# three of them now (\TEX\, \TEXASAG\, \TEXCTCMB\) and a scan that knows only
# the first silently reports nothing the day a chapter gets its own.
LIT = re.compile(r'"(\\\\(?:[A-Za-z0-9_]+\\\\)?[A-Za-z0-9_ ]+\.(?:TIM|SMD|PVA))(?:;1)?"')

TABLE = r'(?:new_tex|shared_tex|raf_tex)'


def disc_files(disc_xml='disc.xml'):
    """{DISC NAME: source path} for every file disc.xml ships."""
    disc = {}
    for f in ET.parse(disc_xml).getroot().iter('file'):
        disc[f.get('name').upper()] = f.get('source')
    return disc


def scan(src_glob='src/*.c', disc=None):
    """Return (regs, deferred): lists of (disc name, module).

    `deferred` is for texmgr_register_deferred(), which no longer exists (every
    registration is header-only and banked now); it is still scanned so an old
    call cannot vanish from the count."""
    if disc is None:
        disc = disc_files()
    regs, deferred = [], []
    for c in sorted(glob.glob(src_glob)):
        src = open(c, encoding='utf-8', errors='replace').read()
        mod = os.path.basename(c)
        # The deferred scan runs first and the two are mutually exclusive by
        # construction: 'texmgr_register(' does not match
        # 'texmgr_register_deferred(' because of the open paren.
        for m in re.finditer(r'texmgr_register_deferred\(\s*"([^"]+)"', src):
            deferred.append((basename(m.group(1)), mod))
        if re.search(r'texmgr_register_deferred\(\s*' + TABLE, src):
            for m in re.finditer(TABLE + r'[a-z_]*\[[^\]]*\]\s*=\s*\{(.*?)\};',
                                 src, re.S):
                for lit in LIT.findall(m.group(1)):
                    deferred.append((basename(lit), mod))
            continue
        for m in re.finditer(r'texmgr_register\(\s*"([^"]+)"', src):
            regs.append((basename(m.group(1)), mod))
        if re.search(r'texmgr_register\(\s*' + TABLE, src):
            for m in re.finditer(TABLE + r'[a-z_]*\[[^\]]*\]\s*=\s*\{(.*?)\};',
                                 src, re.S):
                for lit in LIT.findall(m.group(1)):
                    regs.append((basename(lit), mod))
    regs = [(n, m) for n, m in regs if n in disc]
    deferred = [(n, m) for n, m in deferred if n in disc]
    return regs, deferred


def texmgr_max(texmgr_c='src/texmgr.c'):
    """The live TEXMGR_MAX, read from the #define rather than restated."""
    m = re.search(r'^\s*#define\s+TEXMGR_MAX\s+(\d+)', open(texmgr_c, encoding='utf-8').read(), re.M)
    if not m:
        raise SystemExit('texmgr_count.py: no TEXMGR_MAX #define in ' + texmgr_c)
    return int(m.group(1))
