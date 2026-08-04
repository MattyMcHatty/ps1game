#!/usr/bin/env python3
"""
retarget_tim.py - clone an existing .tim to a different VRAM address.

Why this exists
---------------
Sometimes a room needs a texture that ALREADY lives somewhere in VRAM, but that
home address is occupied by something else the same room draws. The Garden
Stairs hit this twice: it draws grss, whose slot (x768) it fills with
brick_wall, and gravel_texture, whose slot (x640) it fills with chnlnk. The fix
is a second copy of each at a page the room does not otherwise touch.

Rebuilding those copies with png_to_tim.py is NOT equivalent:
  - the PNG in textures/ is not always the source the shipped .tim was built
    from (grss.png is 64x64; grss.tim is 128x128 — rebuilding it would produce a
    64px texture that a mesh's 0..127 UVs read straight past, into whatever sits
    next to it in VRAM);
  - re-quantising picks a fresh palette, so the copy would not match the
    original even when the source size is right.

This copies the pixel data and CLUT byte for byte and only rewrites the VRAM
coordinates in the two block headers.

Usage:
    python tools/retarget_tim.py in.tim out.tim --tx N --ty N [--cx N --cy N]

The destination must be page-aligned in the same way as the original (x a
multiple of 64) if the mesh's baked UVs are to stay valid: UVs are offsets
WITHIN a tpage, so a texture that sat at offset 0 of its page must land at
offset 0 of the new one. Check the result with tools/vram_map.py.
"""

import struct
import sys


def retarget(src, dst, tx, ty, cx, cy):
    with open(src, "rb") as f:
        d = bytearray(f.read())

    if d[0] != 0x10:
        sys.exit("%s is not a TIM (magic 0x%02X)" % (src, d[0]))

    flags = struct.unpack("<I", d[4:8])[0]
    off   = 8

    if flags & 8:                      # CLUT block
        bs = struct.unpack("<I", d[off:off + 4])[0]
        old_cx, old_cy = struct.unpack("<HH", d[off + 4:off + 8])
        if cx is None:
            cx, cy = old_cx, old_cy
        struct.pack_into("<HH", d, off + 4, cx, cy)
        print("  CLUT   (%d,%d) -> (%d,%d)" % (old_cx, old_cy, cx, cy))
        off += bs
    elif cx is not None:
        print("  (no CLUT block; --cx/--cy ignored)")

    bs = struct.unpack("<I", d[off:off + 4])[0]
    old_tx, old_ty = struct.unpack("<HH", d[off + 4:off + 8])
    w, h           = struct.unpack("<HH", d[off + 8:off + 12])
    struct.pack_into("<HH", d, off + 4, tx, ty)
    bpp = {0: 4, 1: 8, 2: 16, 3: 16}[flags & 3]
    print("  pixels (%d,%d) -> (%d,%d)   %d words x %d rows = %dx%d px @ %dbpp"
          % (old_tx, old_ty, tx, ty, w, h, w * (16 // bpp), h, bpp))
    if tx % 64:
        print("  WARNING: x=%d is not page-aligned (multiple of 64); baked UVs "
              "will be off unless the original was mid-page too." % tx)

    with open(dst, "wb") as f:
        f.write(d)
    print("  written: %s" % dst)


def main():
    args = sys.argv[1:]
    if len(args) < 2 or args[0] in ("-h", "--help"):
        print(__doc__.strip())
        sys.exit(0)

    src, dst = args[0], args[1]
    tx = ty = cx = cy = None
    i = 2
    while i < len(args):
        if   args[i] == "--tx" and i + 1 < len(args): tx = int(args[i + 1]); i += 2
        elif args[i] == "--ty" and i + 1 < len(args): ty = int(args[i + 1]); i += 2
        elif args[i] == "--cx" and i + 1 < len(args): cx = int(args[i + 1]); i += 2
        elif args[i] == "--cy" and i + 1 < len(args): cy = int(args[i + 1]); i += 2
        else: i += 1

    if tx is None or ty is None:
        sys.exit("--tx and --ty are required")
    if (cx is None) != (cy is None):
        sys.exit("--cx and --cy must be given together")

    print("Retargeting %s" % src)
    retarget(src, dst, tx, ty, cx, cy)


if __name__ == "__main__":
    main()
