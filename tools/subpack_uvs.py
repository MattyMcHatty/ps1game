"""
subpack_uvs.py - pack several SMALL textures into ONE 128x128 page tile by
baking a per-texture scale + offset into a level's UVs.

WHY THIS EXISTS
    tools/io_export_smx_v3.py scales every UV in an export to ONE texel size
    (exp_tex_size, 128). That is why the runbooks say every texture must be
    128x128: not hardware, just one global number. A 64x64 source therefore has
    to be stretched to 128x128 and burns a full 64x128 VRAM block for detail it
    does not have.

    This post-pass runs on the .smx BEFORE smxlink and rewrites the UVs of
    chosen textures from the 128-tile the exporter emitted onto a smaller
    sub-rect of that same tile:

        u' = round(u * size / 128) + u_off        (clamped to the sub-rect)

    Four 64x64 textures then live inside one 128x128 footprint at a quarter of
    the VRAM each (an eighth at 4bpp), each keeping its OWN CLUT, because clut
    is a per-poly field and each sub-texture is still its own TIM.

WHY NO C CHANGE IS NEEDED
    The room's single DR_TWIN (mask 128, sorted at OT_LENGTH-1) leaves any UV
    already in [0,128) completely alone, and the sub-rect is inside that range.
    The sub-texture's TIM sits at a non-page-aligned tx inside the page tile, so
    getTPage() still floors to the SAME tpage as its neighbours. The engine's
    tex_tpage[]/tex_clut[] tables need no new entries beyond the ordinary one
    slot per texture.

THE PRECONDITION - THE PACKED TEXTURE MUST NOT TILE
    Wrapping happens mod 128 across the WHOLE tile, so a sub-texture that tiles
    would wrap into its neighbour. Any texture whose UVs exceed 128 anywhere is
    REFUSED. Check first with --report, which needs no table.

USAGE
    python tools/subpack_uvs.py --report <in.smx>
    python tools/subpack_uvs.py <in.smx> <table.json> [-o <out.smx>]

    Default output is <in>.packed.smx. The input is NEVER modified: re-running
    the Blender export overwrites the source and this pass is re-run after it.
    smxlink then reads the .packed.smx:

        smxlink -tp textures -o assets\\<level>.smd assets\\<level>.packed.smx

    The tex-map generator (gen_<level>_tex_map.py) may read EITHER file - this
    pass changes no texture list, no poly order and no poly count.

TABLE FORMAT (JSON)
    {
      "textures": {
        "poison_flower_base": {"size": 64, "u_off":  0, "v_off": 0, "page": "gh_small"},
        "pipe_button_off":    {"size": 64, "u_off": 64, "v_off": 0, "page": "gh_small"}
      }
    }
    size          power of two, 8..128 - the texture's REAL texel size
    u_off/v_off   multiples of 8, and u_off+size <= 128 (same for v)
    page          optional group label; entries sharing one are checked for
                  overlap, because they share one 128x128 tile
"""
import json
import os
import re
import sys

STAMP = "<!-- subpack_uvs.py: UVs sub-packed; do not re-run this pass on this file -->"

POLY_RE = re.compile(r"<poly\b")
TEXATTR_RE = re.compile(r'\btexture="(\d+)"')
UV_RE = re.compile(r'\b(t[uv])(\d)="(\d+)"')
TEXFILE_RE = re.compile(r'<texture\s+file="([^"]*)"')


def read_smx(path):
    """Return (lines, names) - names is the <textures> list, in index order."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    names, in_block = [], False
    for ln in lines:
        if "<textures" in ln:
            in_block = True
            continue
        if "</textures>" in ln:
            in_block = False
            continue
        if in_block:
            m = TEXFILE_RE.search(ln)
            if m:
                names.append(m.group(1))
    return lines, names


def scan(lines, names):
    """Per texture index: [poly count, max UV seen]."""
    stats = {i: [0, 0] for i in range(len(names))}
    for ln in lines:
        if not POLY_RE.search(ln):
            continue
        m = TEXATTR_RE.search(ln)
        if not m:
            continue
        idx = int(m.group(1))
        if idx not in stats:
            stats[idx] = [0, 0]
        stats[idx][0] += 1
        for _, _, val in UV_RE.findall(ln):
            v = int(val)
            if v > stats[idx][1]:
                stats[idx][1] = v
    return stats


def find_sharers(path, names):
    """name -> [other .smx files using it].

    A texture is shared when more than one level references it (the fat doors
    and grss are used almost everywhere). Sub-packing rewrites UVs in ONE .smx
    but the TIM is global, so every OTHER consumer would still address the full
    128 tile and read whatever now sits beside it. Packing a shared texture
    means packing it in every level that draws it."""
    # Project root via this script's own home (tools/), not via the .smx: level
    # exports live at varying depths (assets/, assets/garden/, ...).
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets = os.path.join(root, "assets")
    me = os.path.abspath(path)
    sharers = {n: [] for n in names}
    if not os.path.isdir(assets):
        return sharers
    for cur, _dirs, files in os.walk(assets):
        for fn in files:
            if not fn.lower().endswith(".smx"):
                continue
            other = os.path.join(cur, fn)
            if os.path.abspath(other) == me:
                continue
            try:
                other_lines, other_names = read_smx(other)
            except OSError:
                continue
            # Skip this pass's own outputs: they are copies of a level, not
            # separate consumers, and would report every texture as shared.
            if any(STAMP in ln for ln in other_lines):
                continue
            for n in names:
                if n in other_names:
                    sharers[n].append(fn)
    return sharers


def do_report(path):
    lines, names = read_smx(path)
    stats = scan(lines, names)
    sharers = find_sharers(path, names)
    print("%s - %d textures" % (os.path.basename(path), len(names)))
    print("  %-4s %-24s %6s %7s  %s" % ("idx", "name", "polys", "max UV", "sub-packable?"))
    ok = 0
    for i, name in enumerate(names):
        polys, mx = stats.get(i, [0, 0])
        if polys == 0:
            note = "unused by any poly"
        elif mx > 128:
            note = "NO - tiles (max UV %d > 128)" % mx
        elif sharers.get(name):
            note = "yes, BUT SHARED with %d other level(s)" % len(sharers[name])
            ok += 1
        else:
            note = "yes - spans one tile"
            ok += 1
        print("  %-4d %-24s %6d %7d  %s" % (i, name, polys, mx, note))
    print("\n%d of %d textures are sub-packable." % (ok, len(names)))
    if ok:
        print("A packed 64x64 costs 1/4 the VRAM of a 128x128 at 8bpp, 1/8 at 4bpp.")
    shared_ok = [n for i, n in enumerate(names)
                 if sharers.get(n) and stats.get(i, [0, 0])[1] <= 128 and stats.get(i, [0, 0])[0]]
    for n in shared_ok:
        print("\n  SHARED: %r is also in %s." % (n, ", ".join(sorted(sharers[n]))))
        print("  Its TIM is global. Packing it here without packing it there too")
        print("  leaves those levels addressing the full 128 tile - they would")
        print("  sample whatever now sits beside it.")
    return 0


def load_table(path):
    with open(path, "r", encoding="utf-8") as f:
        table = json.load(f)
    entries = table.get("textures")
    if not isinstance(entries, dict) or not entries:
        raise SystemExit("error: %s has no non-empty \"textures\" object" % path)
    return entries


def face_uvs(line):
    """([tu...], [tv...]) for one <poly> line, in attribute order."""
    us, vs = [], []
    for axis, _slot, val in UV_RE.findall(line):
        (us if axis == "tu" else vs).append(int(val))
    return us, vs


def max_face_span(lines, idx):
    """Widest single-face UV span using texture `idx`, in the exporter's
    128-space. This is what decides whether a face can fit one 256 tile."""
    worst = 0
    for ln in lines:
        if not POLY_RE.search(ln):
            continue
        m = TEXATTR_RE.search(ln)
        if not m or int(m.group(1)) != idx:
            continue
        us, vs = face_uvs(ln)
        for arr in (us, vs):
            if arr:
                worst = max(worst, max(arr) - min(arr))
    return worst


def validate(entries, names, stats, lines):
    """Return name -> (size, u_off, v_off, page), or exit listing every problem."""
    errs = []
    resolved = {}
    for name, spec in entries.items():
        if name not in names:
            errs.append("%r is not in the SMX <textures> list (have: %s)"
                        % (name, ", ".join(names)))
            continue
        idx = names.index(name)
        size = spec.get("size")
        u_off = spec.get("u_off", 0)
        v_off = spec.get("v_off", 0)
        if size not in (8, 16, 32, 64, 128, 256):
            errs.append("%s: size %r must be a power of two, 8..256" % (name, size))
            continue
        if u_off % 8 or v_off % 8 or u_off < 0 or v_off < 0:
            errs.append("%s: u_off/v_off must be non-negative multiples of 8" % name)
            continue
        polys, mx = stats.get(idx, [0, 0])
        if polys == 0:
            errs.append("%s: no poly uses it - packing it would do nothing" % name)
            continue

        if size == 256:
            # UPSCALE mode - see remap_face_256(). A 256 texture is one whole
            # 4bpp tpage, so it cannot share and cannot be offset.
            if u_off or v_off:
                errs.append("%s: a 256 texture is a whole tpage; u_off/v_off must be 0"
                            % name)
                continue
            if spec.get("page"):
                errs.append("%s: a 256 texture fills its tpage and cannot share a "
                            "\"page\" with anything" % name)
                continue
            span = max_face_span(lines, idx)
            if span * 2 > 255:
                errs.append("%s: a face spans %d (=%d at 256), which cannot fit 0..255; "
                            "re-UV that face in Blender to stay inside one tile"
                            % (name, span, span * 2))
                continue
        else:
            if u_off + size > 128 or v_off + size > 128:
                errs.append("%s: sub-rect %dx%d at (%d,%d) leaves the 128 tile"
                            % (name, size, size, u_off, v_off))
                continue
            if mx > 128:
                errs.append("%s: TILES (max UV %d > 128); wrapping is mod 128 across the "
                            "whole tile, so it would bleed into its neighbour" % (name, mx))
                continue
        resolved[name] = (size, u_off, v_off, spec.get("page"))

    # Overlap, among entries sharing a page tile.
    pages = {}
    for name, (size, u_off, v_off, page) in resolved.items():
        if page is None:
            continue
        pages.setdefault(page, []).append((name, size, u_off, v_off))
    for page, members in sorted(pages.items()):
        for a in range(len(members)):
            for b in range(a + 1, len(members)):
                n0, s0, u0, v0 = members[a]
                n1, s1, u1, v1 = members[b]
                if u0 < u1 + s1 and u1 < u0 + s0 and v0 < v1 + s1 and v1 < v0 + s0:
                    errs.append("page %r: %s (%dx%d at %d,%d) overlaps %s (%dx%d at %d,%d)"
                                % (page, n0, s0, s0, u0, v0, n1, s1, s1, u1, v1))
    if errs:
        sys.stderr.write("subpack_uvs: refusing to pack -\n")
        for e in errs:
            sys.stderr.write("  * %s\n" % e)
        raise SystemExit(1)
    return resolved


def remap(value, size, off):
    """One UV from the exporter's 128 tile onto the sub-rect.

    The exporter emits a full-tile span as 0..128, where 128 is the wrap point
    rather than a texel. A sub-rect cannot wrap, so the far edge clamps to the
    sub-texture's LAST texel: the face still shows the whole texture, inset by
    half a texel at that edge."""
    v = int(round(value * size / 128.0)) + off
    if v < off:
        v = off
    if v > off + size - 1:
        v = off + size - 1
    return v


def remap_face_256(arr):
    """One axis of one face, from the exporter's 128-space to a 256 texture.

    Doubling keeps the texture at the SAME world scale with 4x the texel
    density. The exporter left each face's minimum in [0,128) and relied on the
    mask-128 window to wrap anything past the edge; a 256 texture has NO window
    (see the C note the tool prints), so every value must land in 0..255.

    Dropping whole 256-tiles preserves the sub-tile phase, and so the tiling
    continuity between neighbouring faces. A face that STILL overflows straddles
    a tile boundary: it is shifted the rest of the way, which fits it at the cost
    of breaking phase with its neighbours. Returns (values, straddled)."""
    d = [x * 2 for x in arr]
    d = [x - (min(d) // 256) * 256 for x in d]
    if max(d) <= 255:
        return d, False
    return [x - (max(d) - 255) for x in d], True


def rewrite(lines, names, resolved):
    by_index = {}
    for name, (size, u_off, v_off, _page) in resolved.items():
        by_index[names.index(name)] = (size, u_off, v_off)
    counts = {i: 0 for i in by_index}
    straddled = {i: 0 for i in by_index}

    out = []
    for ln in lines:
        m = TEXATTR_RE.search(ln) if POLY_RE.search(ln) else None
        if not m or int(m.group(1)) not in by_index:
            out.append(ln)
            continue
        idx = int(m.group(1))
        size, u_off, v_off = by_index[idx]
        counts[idx] += 1

        if size == 256:
            us, vs = face_uvs(ln)
            nu, hit_u = remap_face_256(us)
            nv, hit_v = remap_face_256(vs)
            if hit_u or hit_v:
                straddled[idx] += 1
            it = {"tu": iter(nu), "tv": iter(nv)}

            def sub(mm, it=it):
                return '%s%s="%d"' % (mm.group(1), mm.group(2), next(it[mm.group(1)]))
        else:
            def sub(mm, size=size, u_off=u_off, v_off=v_off):
                axis, slot, val = mm.group(1), mm.group(2), int(mm.group(3))
                off = u_off if axis == "tu" else v_off
                return '%s%s="%d"' % (axis, slot, remap(val, size, off))

        out.append(UV_RE.sub(sub, ln))
    return out, counts, straddled


def main(argv):
    if len(argv) >= 2 and argv[0] == "--report":
        return do_report(argv[1])
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2

    src, table_path = argv[0], argv[1]
    out_path = None
    if "-o" in argv:
        out_path = argv[argv.index("-o") + 1]
    if out_path is None:
        base = src[:-4] if src.lower().endswith(".smx") else src
        out_path = base + ".packed.smx"

    lines, names = read_smx(src)
    if any(STAMP in ln for ln in lines):
        raise SystemExit("error: %s is already sub-packed; run this pass on the "
                         "raw Blender export, not on its output" % src)
    stats = scan(lines, names)
    resolved = validate(load_table(table_path), names, stats, lines)
    out, counts, straddled = rewrite(lines, names, resolved)

    # Stamp after the exporter's own comment header.
    insert_at = 0
    while insert_at < len(out) and out[insert_at].startswith("<!--"):
        insert_at += 1
    out.insert(insert_at, STAMP)

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))

    print("sub-packed %s -> %s" % (os.path.basename(src), os.path.basename(out_path)))
    needs_bracket = []
    for name, (size, u_off, v_off, page) in sorted(resolved.items()):
        idx = names.index(name)
        if size == 256:
            print("  %-24s %3dx%-3d UPSCALED   %4d polys  (%d straddling faces "
                  "re-normalised)" % (name, size, size, counts[idx], straddled[idx]))
            needs_bracket.append((name, counts[idx]))
        else:
            print("  %-24s %3dx%-3d at (%3d,%3d)  %4d polys  page=%s"
                  % (name, size, size, u_off, v_off, counts[idx], page or "-"))

    for name, polys in needs_bracket:
        print("\n  *** %r NEEDS A C CHANGE - it is 256 wide, so it must be 4bpp"
              % name)
        print("      (a 256x256 8bpp texture spills its page) and the room's")
        print("      mask-128 window would wrap its UVs at 128. Bracket its %d"
              % polys)
        print("      polys with window-off/window-restore DR_TWINs, as")
        print("      src/copper_pot.c does. addPrim PREPENDS, so add in the")
        print("      order: restore, poly, disable.")
    sharers = find_sharers(src, list(resolved))
    for name in sorted(resolved):
        if sharers.get(name):
            print("\n  WARNING - %r is SHARED with %s." % (name, ", ".join(sorted(sharers[name]))))
            print("  Its TIM is global: pack it in those levels too, or they will")
            print("  address the full 128 tile and sample its new neighbour.")

    print("\n  >>> NOW GREP THE C FOR HAND-WRITTEN UVs ON THESE TEXTURES. <<<")
    print("      This pass rewrites MESH UVs only. Anything that draws one of")
    print("      them from UV constants in C - a door panel, a sprite, a menu")
    print("      icon - still spans the old 128 tile and will read off the end")
    print("      of the shrunk texture into whatever shares its page. That is")
    print("      exactly what happened to DOOR_PANEL_GREENHOUSE in door_anim.c,")
    print("      which drew three quarters of a double-door panel. Start with:")
    for name in sorted(resolved):
        print("        grep -rn TIM_TPAGE_ src/*.c   # then check each hit's UVs")
        break

    print("\nTIM PLACEMENT - each packed texture is its own TIM inside the tile:")
    print("  8bpp:  --tx <page_x + u_off/2>  --ty <page_y + v_off>")
    print("  4bpp:  --tx <page_x + u_off/4>  --ty <page_y + v_off>")
    print("  page_x stays 64-aligned and page_y in {0,256}; the sub-TIM's own tx")
    print("  is NOT page-aligned, which is correct - getTPage() floors to the")
    print("  same tpage and the baked u_off reaches across the tile.")
    print("\nNEXT: smxlink -tp textures -o <level>.smd %s" % out_path)
    print("      then re-run tools/vram_map.py and CHECK IT.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
