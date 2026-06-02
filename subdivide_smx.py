"""
Subdivide large quads in an SMX file to reduce near-plane clipping.

PS1/SMD quad vertex order is Z-pattern:  v0(TL) v1(TR) v2(BL) v3(BR)
Edges are: v0-v1 (top), v0-v2 (left), v1-v3 (right), v2-v3 (bottom).
Each oversized quad is split into 4 sub-quads preserving this ordering.
Only floor/ceiling polys (Y-dominant normal) are subdivided.
type and texture attributes are preserved on all polys.
"""
import xml.etree.ElementTree as ET
import math
import sys

SMX_IN  = sys.argv[1] if len(sys.argv) > 1 else 'assets/level1.smx'
SMX_OUT = SMX_IN
THRESHOLD = 300

tree = ET.parse(SMX_IN)
root = tree.getroot()

verts = [(float(v.get('x')), float(v.get('y')), float(v.get('z')))
         for v in root.find('vertices').findall('v')]

normals = [(float(n.get('x')), float(n.get('y')), float(n.get('z')))
           for n in root.find('normals').findall('v')]

def is_floor_or_ceiling(n0_str):
    nx, ny, nz = normals[int(n0_str)]
    return abs(ny) > abs(nx) and abs(ny) > abs(nz)

vert_map = {(round(x), round(y), round(z)): i for i, (x, y, z) in enumerate(verts)}

def get_or_add(x, y, z):
    key = (round(x), round(y), round(z))
    if key in vert_map:
        return vert_map[key]
    idx = len(verts)
    verts.append((x, y, z))
    vert_map[key] = idx
    return idx

def mid(i, j):
    a, b = verts[i], verts[j]
    return get_or_add((a[0]+b[0])/2, (a[1]+b[1])/2, (a[2]+b[2])/2)

def edge_len(i, j):
    a, b = verts[i], verts[j]
    return math.sqrt((a[0]-b[0])**2 + (a[1]-b[1])**2 + (a[2]-b[2])**2)

def needs_split(v0, v1, v2, v3):
    return (edge_len(v0, v1) > THRESHOLD or
            edge_len(v0, v2) > THRESHOLD or
            edge_len(v1, v3) > THRESHOLD or
            edge_len(v2, v3) > THRESHOLD)

prims_elem = root.find('primitives')
old_prims  = list(prims_elem)
for p in old_prims:
    prims_elem.remove(p)

queue = []
for p in old_prims:
    vis  = [int(p.get(f'v{i}')) for i in range(4)]
    tex  = p.get('texture', None)   # None = untextured (F4)
    ptype = p.get('type', 'F4')
    queue.append((vis, p.get('n0'), p.get('r0','128'), p.get('g0','128'), p.get('b0','128'), tex, ptype))

new_prims = []
while queue:
    vis, n0, r, g, b, tex, ptype = queue.pop()
    v0, v1, v2, v3 = vis

    if not is_floor_or_ceiling(n0) or not needs_split(v0, v1, v2, v3):
        new_prims.append((vis, n0, r, g, b, tex, ptype))
        continue

    m_top   = mid(v0, v1)
    m_left  = mid(v0, v2)
    m_right = mid(v1, v3)
    m_bot   = mid(v2, v3)
    mc      = mid(m_top, m_bot)

    for sq in (
        [v0,     m_top,   m_left,  mc     ],
        [m_top,  v1,      mc,      m_right],
        [m_left, mc,      v2,      m_bot  ],
        [mc,     m_right, m_bot,   v3     ],
    ):
        queue.append((sq, n0, r, g, b, tex, ptype))

for vis, n0, r, g, b, tex, ptype in new_prims:
    el = ET.Element('poly')
    for i, vi in enumerate(vis):
        el.set(f'v{i}', str(vi))
    el.set('n0', n0)
    el.set('shading', 'F')
    el.set('r0', r); el.set('g0', g); el.set('b0', b)
    if tex is not None:
        el.set('texture', tex)
    el.set('type', ptype)
    prims_elem.append(el)

prims_elem.set('count', str(len(new_prims)))

verts_elem = root.find('vertices')
for v in list(verts_elem):
    verts_elem.remove(v)
for x, y, z in verts:
    el = ET.Element('v')
    el.set('x', f'{x:.6f}'); el.set('y', f'{y:.6f}'); el.set('z', f'{z:.6f}')
    verts_elem.append(el)
verts_elem.set('count', str(len(verts)))

with open(SMX_OUT, 'w') as f:
    f.write('<!-- Created using Project Scarlet SMX Export Plug-in for Blender (Blender 4/5, real coords, quad support) -->\n')
    f.write('<!-- Scale factor used: 100.000000 (1 Blender unit = 100.000000 PS1 units) -->\n')
    ET.indent(tree, space='')
    f.write(ET.tostring(root, encoding='unicode'))
    f.write('\n')

print(f"Primitives: {len(old_prims)} -> {len(new_prims)}")
print(f"Vertices:   {len(verts)}")
