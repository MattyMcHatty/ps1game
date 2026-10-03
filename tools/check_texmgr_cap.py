"""
check_texmgr_cap.py - fail the build if src/ registers more textures than
src/texmgr.c's TEXMGR_MAX allows.

    python tools/check_texmgr_cap.py        (run from the project root)

Run by CMake before every compile (the texmgr_cap target in CMakeLists.txt).

WHY: a texmgr_register() past the cap returns -1, and that texture then draws as
whatever was last in its VRAM page in EVERY room that uses it - no crash, no
message. And it is not necessarily the NEW texture that breaks: registration
order is fixed in main(), so whichever registers LAST falls off the end. The
East Stairwell once pushed the concrete and the dresser off that way.

The count comes from tools/texmgr_count.py, the same scanner tools/heap_budget.py
uses. It may over-count (it does not strip comments) but should not under-count
any registration made the ways the tree makes them today; main()'s
texmgr_refused() red screen is the runtime backstop for one it cannot see.

THE FIX WHEN THIS FAILS is to raise TEXMGR_MAX in src/texmgr.c. Each slot is ~40
bytes of BSS; the pixels are loaded per texture bank, not per slot.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from texmgr_count import scan, texmgr_max  # noqa: E402

regs, deferred = scan()
used = len(regs) + len(deferred)
cap = texmgr_max()

if used > cap:
    print('check_texmgr_cap.py: ERROR: %d texmgr registrations but TEXMGR_MAX is %d.'
          % (used, cap))
    print('  The last %d to register would be refused at boot and draw the wrong'
          % (used - cap))
    print('  texture in every room. Raise TEXMGR_MAX in src/texmgr.c.')
    sys.exit(1)

print('check_texmgr_cap.py: %d of %d texmgr registrations (%d spare)'
      % (used, cap, cap - used))
