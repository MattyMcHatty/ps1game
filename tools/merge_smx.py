"""
merge_smx.py - concatenate several SMX exports into ONE SMX, in the order
given, for a room drawn from more than one Blender object.

A room's mesh rides the shared room arena as ONE .SMD (src/room_arena.h), with
its tex map, no-cull bits and walls packed onto the end of it
(src/room_data.h). A room built from SEVERAL exports - The Tube (Nov 2026): a
spinning tube plus doors that stay put - is therefore merged into one SMX
first and linked with smxlink as usual. The ORDER IS THE CONTRACT: the first
file's polys come first in the merged list (and so in the .SMD and its tex
map), then the second's, and so on, so a room can treat the first N prims
differently (The Tube draws its first THE_TUBE_SPIN_PRIMS with a spin matrix).

Vertex, normal and texture lists are concatenated and every poly's indices are
rebased. Textures with the same name are shared, not duplicated. Every other
attribute is copied as-is.

The output is an INTERMEDIATE: *.merged.smx is gitignored and regenerated from
the committed exports. Re-run after every re-export.

Usage:
    python tools/merge_smx.py <out.smx> <in1.smx> <in2.smx> [...]
"""
import sys
import xml.etree.ElementTree as ET


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out, ins = sys.argv[1], sys.argv[2:]

    verts, norms, texs, polys = [], [], [], []
    for path in ins:
        root = ET.parse(path).getroot()
        v_base, n_base = len(verts), len(norms)
        verts += [dict(v.attrib) for v in root.find('vertices').findall('v')]
        ne = root.find('normals')
        norms += [dict(n.attrib) for n in (ne.findall('v') if ne is not None else [])]
        te = root.find('textures')
        local = [t.get('file') for t in (te.findall('texture') if te is not None else [])]
        remap = {}
        for i, name in enumerate(local):
            if name not in texs:
                texs.append(name)
            remap[i] = texs.index(name)
        count = 0
        for p in root.find('primitives').findall('poly'):
            a = dict(p.attrib)
            for k in ('v0', 'v1', 'v2', 'v3'):
                if k in a:
                    a[k] = str(int(a[k]) + v_base)
            for k in ('n0', 'n1', 'n2', 'n3'):
                if k in a:
                    a[k] = str(int(a[k]) + n_base)
            if 'texture' in a:
                a['texture'] = str(remap[int(a['texture'])])
            polys.append(a)
            count += 1
        print(f"{path}: {count} polys (prims {len(polys) - count}..{len(polys) - 1}), "
              f"textures {local}")

    def attrs(d):
        return ' '.join(f'{k}="{v}"' for k, v in d.items())

    lines = ['<!-- Merged by tools/merge_smx.py from: ' +
             ', '.join(ins).replace('--', '-') + ' -->',
             '<model version="1">',
             f'<vertices count="{len(verts)}">']
    lines += [f'<v {attrs(v)}/>' for v in verts]
    lines += ['</vertices>', f'<normals count="{len(norms)}">']
    lines += [f'<v {attrs(n)}/>' for n in norms]
    lines += ['</normals>', f'<textures count="{len(texs)}">']
    lines += [f'<texture file="{t}"/>' for t in texs]
    lines += ['</textures>', f'<primitives count="{len(polys)}">']
    lines += [f'<poly {attrs(p)} />' for p in polys]
    lines += ['</primitives>', '</model>']
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Written {out}: {len(verts)} verts, {len(polys)} polys, textures {texs}")


if __name__ == '__main__':
    main()
