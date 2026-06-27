"""
smd_ft3_to_ft4.py — rewrite textured-triangle (FT3) prims as degenerate FT4 quads.

Why: the PSn00bSDK smdInitData / SMD loader in this project has only ever been
fed all-FT4 meshes (kitchen, delivery). Reception was the first SMD to contain
FT3 (textured triangles), and loading it crashed at startup before the title.
A degenerate quad (v3 = v2, uv3 = uv2) draws as the same triangle, so converting
every FT3 to FT4 keeps the visuals identical while making the mesh all-FT4 like
the others. Run this AFTER smxlink, in place.

Prim layout (smxlink revision-1, stride 32):
  [0]    type/flags byte: bits0-1 type (1=tri,2=quad), bit5 textured
  [3]    stride (32)
  [4-11] v0,v1,v2,v3 (uint16)
  [12-13] n0   [16-18] rgb   [20-27] u0,v0..u3,v3   [28-31] tpage,clut

Usage: python smd_ft3_to_ft4.py assets/Reception.smd
"""
import struct, sys

def convert(path):
    d = bytearray(open(path, 'rb').read())
    n_prims = struct.unpack_from('<H', d, 10)[0]
    off     = struct.unpack_from('<I', d, 20)[0]
    p = off
    converted = 0
    for _ in range(n_prims):
        b = d[p]; stride = d[p + 3]
        if (b & 0x03) == 1:                 # triangle -> make quad
            d[p] = (b & ~0x03) | 0x02       # type bits 01 -> 10
            d[p + 10] = d[p + 8]            # v3 = v2 (low byte)
            d[p + 11] = d[p + 9]            # v3 = v2 (high byte)
            d[p + 26] = d[p + 24]           # u3 = u2
            d[p + 27] = d[p + 25]           # v3uv = v2uv
            converted += 1
        p += stride
    open(path, 'wb').write(d)
    print(f"{path}: converted {converted} FT3 -> degenerate FT4 ({n_prims} prims total)")

if __name__ == '__main__':
    convert(sys.argv[1] if len(sys.argv) > 1 else 'assets/Reception.smd')
