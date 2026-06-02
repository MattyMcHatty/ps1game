#!/usr/bin/env python3
"""
png_to_tim.py
Converts a PNG image to PS1 TIM format.

Requirements:
    pip install Pillow

Usage:
    python3 png_to_tim.py texture.png
    python3 png_to_tim.py texture.png --bpp 8 --cx 0 --cy 480 --tx 640 --ty 0

Options:
    --bpp N     Colour depth: 4 (16 colours), 8 (256 colours), 16 (full colour)
                Default: 8
    --tx N      VRAM X position for texture data (default: 640)
    --ty N      VRAM Y position for texture data (default: 0)
    --cx N      VRAM X position for CLUT/palette (default: 0)
    --cy N      VRAM Y position for CLUT/palette (default: 480)
    --out FILE  Output filename (default: same as input with .tim extension)

VRAM layout reminder:
    Framebuffer 1:  x=0,   y=0    (320x240)
    Framebuffer 2:  x=0,   y=240  (320x240)
    Textures start: x=640, y=0    (safe default)
    CLUTs start:    x=0,   y=480  (safe default)

TIM format reference:
    https://www.psxdev.net/forum/viewtopic.php?t=109
"""

import sys
import os
import struct

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is not installed.")
    print("Run: pip install Pillow")
    sys.exit(1)


# -----------------------------------------------------------------------
# TIM format constants
# -----------------------------------------------------------------------

TIM_4BIT  = 0x08   # 4-bit CLUT (16 colours)
TIM_8BIT  = 0x09   # 8-bit CLUT (256 colours)
TIM_16BIT = 0x02   # 16-bit direct colour (no CLUT)

TIM_MAGIC   = 0x10  # TIM file magic number
TIM_VERSION = 0x00  # TIM version


# -----------------------------------------------------------------------
# Colour conversion
# -----------------------------------------------------------------------

def rgb_to_ps1(r, g, b, a=255):
    """Convert 8-bit RGB to PS1 16-bit colour (BGR555 + STP bit).
    
    PS1 colour format: XBBBBBGGGGGRRRRR (16 bits)
    Each channel is 5 bits (0-31).
    Bit 15 (STP) = semi-transparency flag, set to 0.
    Black (0,0,0) is transparent on PS1 unless STP=1.
    """
    r5 = (r >> 3) & 0x1F
    g5 = (g >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F

    # If pixel is fully transparent set to 0x0000 (transparent black)
    if a == 0:
        return 0x0000

    # Pure black (0,0,0) is transparent on PS1
    # Use (0,0,0) with STP=1 (0x8000) to force opaque black if needed
    if r5 == 0 and g5 == 0 and b5 == 0 and a > 0:
        return 0x8000  # opaque black with STP bit set

    return (b5 << 10) | (g5 << 5) | r5


# -----------------------------------------------------------------------
# CLUT (palette) builder
# -----------------------------------------------------------------------

def build_clut_4bit(image):
    """Build a 16-colour palette from an image, return (clut_bytes, index_fn).
    Image must be palette mode with <= 16 colours."""
    img = image.convert('RGBA')

    # Get all unique colours
    colours = []
    seen = set()
    for y in range(img.height):
        for x in range(img.width):
            px = img.getpixel((x, y))
            key = (px[0]>>3, px[1]>>3, px[2]>>3)
            if key not in seen:
                seen.add(key)
                colours.append(px)
            if len(colours) >= 16:
                break
        if len(colours) >= 16:
            break

    # Pad to 16 colours
    while len(colours) < 16:
        colours.append((0, 0, 0, 255))

    # Build CLUT bytes
    clut_bytes = b''
    for c in colours:
        clut_bytes += struct.pack('<H', rgb_to_ps1(c[0], c[1], c[2], c[3]))

    # Build lookup: quantise each pixel to nearest palette entry
    def nearest(px):
        r, g, b = px[0]>>3, px[1]>>3, px[2]>>3
        best = 0
        best_dist = 999999
        for i, c in enumerate(colours):
            cr, cg, cb = c[0]>>3, c[1]>>3, c[2]>>3
            dist = (r-cr)**2 + (g-cg)**2 + (b-cb)**2
            if dist < best_dist:
                best_dist = dist
                best = i
        return best

    return clut_bytes, nearest


def build_clut_8bit(image):
    """Build a 256-colour palette from an image."""
    # Convert to palette mode with 256 colours
    img_p = image.convert('RGB').quantize(colors=256)
    palette = img_p.getpalette()  # flat list [r,g,b, r,g,b, ...]

    # Build 256-entry CLUT
    clut_bytes = b''
    for i in range(256):
        r = palette[i*3]
        g = palette[i*3+1]
        b = palette[i*3+2]
        clut_bytes += struct.pack('<H', rgb_to_ps1(r, g, b))

    # Index function: get palette index for each pixel
    def get_index(px_idx):
        return px_idx

    return clut_bytes, img_p


# -----------------------------------------------------------------------
# Pixel data builders
# -----------------------------------------------------------------------

def build_pixels_4bit(image, index_fn):
    """Pack 4-bit palette indices, two per byte."""
    img = image.convert('RGBA')
    w, h = img.width, img.height
    data = bytearray()

    for y in range(h):
        row = bytearray()
        for x in range(0, w, 2):
            lo = index_fn(img.getpixel((x,   y))) & 0xF
            hi = index_fn(img.getpixel((x+1, y))) & 0xF if x+1 < w else 0
            row.append(lo | (hi << 4))
        data.extend(row)

    return bytes(data)


def build_pixels_8bit(img_p):
    """Get 8-bit palette indices."""
    w, h = img_p.width, img_p.height
    data = bytearray()
    for y in range(h):
        for x in range(w):
            data.append(img_p.getpixel((x, y)) & 0xFF)
    return bytes(data)


def build_pixels_16bit(image):
    """Convert directly to 16-bit PS1 colour."""
    img = image.convert('RGBA')
    w, h = img.width, img.height
    data = bytearray()
    for y in range(h):
        for x in range(w):
            px = img.getpixel((x, y))
            data.extend(struct.pack('<H', rgb_to_ps1(px[0], px[1], px[2], px[3])))
    return bytes(data)


# -----------------------------------------------------------------------
# TIM file writer
# -----------------------------------------------------------------------

def write_tim(filepath, bpp, pixel_data, clut_data,
              tex_x, tex_y, tex_w, tex_h,
              clut_x, clut_y):

    with open(filepath, 'wb') as f:

        # TIM header (8 bytes)
        f.write(struct.pack('<BB', TIM_MAGIC, TIM_VERSION))
        f.write(b'\x00\x00')  # padding

        if bpp == 4:
            flags = TIM_4BIT
        elif bpp == 8:
            flags = TIM_8BIT
        else:
            flags = TIM_16BIT

        # Has CLUT flag
        if bpp in (4, 8):
            flags |= 0x08

        f.write(struct.pack('<I', flags))

        # CLUT block (for 4-bit and 8-bit only)
        if bpp in (4, 8):
            clut_colours = 16 if bpp == 4 else 256
            clut_block_size = 12 + clut_colours * 2  # header(12) + colours(2 bytes each)
            f.write(struct.pack('<I', clut_block_size))   # block size
            f.write(struct.pack('<HH', clut_x, clut_y))  # VRAM position
            f.write(struct.pack('<HH', clut_colours, 1)) # width=num colours, height=1
            f.write(clut_data)

        # Pixel data block
        # In TIM, width is measured in 16-bit units
        # 4-bit: width in pixels / 4 (4 pixels per 16-bit word)
        # 8-bit: width in pixels / 2 (2 pixels per 16-bit word)
        # 16-bit: width in pixels / 1
        if bpp == 4:
            tim_w = tex_w // 4
        elif bpp == 8:
            tim_w = tex_w // 2
        else:
            tim_w = tex_w

        pixel_block_size = 12 + len(pixel_data)
        f.write(struct.pack('<I', pixel_block_size))  # block size
        f.write(struct.pack('<HH', tex_x, tex_y))     # VRAM position
        f.write(struct.pack('<HH', tim_w, tex_h))     # dimensions
        f.write(pixel_data)


# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

def main():
    # Defaults
    bpp     = 8
    tex_x   = 640
    tex_y   = 0
    clut_x  = 0
    clut_y  = 480
    out_file = None

    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        print("Usage: python3 png_to_tim.py texture.png [options]")
        print("")
        print("Options:")
        print("  --bpp N     4, 8, or 16 (default: 8)")
        print("  --tx N      VRAM X for texture (default: 640)")
        print("  --ty N      VRAM Y for texture (default: 0)")
        print("  --cx N      VRAM X for CLUT    (default: 0)")
        print("  --cy N      VRAM Y for CLUT    (default: 480)")
        print("  --out FILE  Output .tim filename")
        print("")
        print("Example:")
        print("  python3 png_to_tim.py wall.png --bpp 8 --tx 640 --ty 0")
        sys.exit(0)

    png_file = args[0]
    i = 1
    while i < len(args):
        if args[i] == '--bpp'  and i+1 < len(args): bpp    = int(args[i+1]); i += 2
        elif args[i] == '--tx' and i+1 < len(args): tex_x  = int(args[i+1]); i += 2
        elif args[i] == '--ty' and i+1 < len(args): tex_y  = int(args[i+1]); i += 2
        elif args[i] == '--cx' and i+1 < len(args): clut_x = int(args[i+1]); i += 2
        elif args[i] == '--cy' and i+1 < len(args): clut_y = int(args[i+1]); i += 2
        elif args[i] == '--out'and i+1 < len(args): out_file = args[i+1];    i += 2
        else: i += 1

    if not os.path.exists(png_file):
        print("Error: file not found: %s" % png_file)
        sys.exit(1)

    if bpp not in (4, 8, 16):
        print("Error: --bpp must be 4, 8, or 16")
        sys.exit(1)

    if out_file is None:
        out_file = os.path.splitext(png_file)[0] + ".tim"

    print("Loading: %s" % png_file)
    image = Image.open(png_file)
    w, h  = image.width, image.height
    print("  Size: %dx%d" % (w, h))

    # Warn about non-power-of-2
    def is_pow2(n): return n > 0 and (n & (n-1)) == 0
    if not is_pow2(w) or not is_pow2(h):
        print("WARNING: image size %dx%d is not power-of-2!" % (w, h))
        print("         PS1 textures should be 16, 32, 64, 128, or 256 pixels.")

    # Warn about size limits
    if w > 256 or h > 256:
        print("WARNING: image is larger than 256x256 — may not fit in PS1 VRAM.")

    print("Converting to %d-bit TIM..." % bpp)
    clut_data   = b''
    pixel_data  = b''

    if bpp == 4:
        clut_data, index_fn = build_clut_4bit(image)
        pixel_data = build_pixels_4bit(image, index_fn)
        print("  Palette: 16 colours")

    elif bpp == 8:
        clut_data, img_p = build_clut_8bit(image)
        pixel_data = build_pixels_8bit(img_p)
        print("  Palette: 256 colours")

    elif bpp == 16:
        pixel_data = build_pixels_16bit(image)
        print("  Mode: direct 16-bit colour (no palette)")

    print("Writing: %s" % out_file)
    write_tim(out_file, bpp, pixel_data, clut_data,
              tex_x, tex_y, w, h,
              clut_x, clut_y)

    size = os.path.getsize(out_file)
    print("  File size: %d bytes (%.1f KB)" % (size, size/1024))
    print("  VRAM texture position: (%d, %d)" % (tex_x, tex_y))
    if bpp in (4, 8):
        print("  VRAM CLUT position:    (%d, %d)" % (clut_x, clut_y))
    print("")
    print("Done! Add %s to your disc.xml and load it in game." % os.path.basename(out_file))

if __name__ == "__main__":
    main()
