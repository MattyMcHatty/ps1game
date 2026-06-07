"""Convert Project Scarlet SMX (XML) to binary SMD format for PS1/PSn00bSDK."""
import struct, sys, xml.etree.ElementTree as ET

COORD_SCALE  = 10    # 1 Blender unit = 10 PS1 units
NORMAL_SCALE = 4096  # unit float -> fixed-point int16

def clamp16(v):
    return max(-32768, min(32767, int(round(v))))

def convert(src_path, dst_path):
    tree = ET.parse(src_path)
    root = tree.getroot()

    verts_el = root.find('vertices')
    verts = []
    for v in verts_el.findall('v'):
        verts.append((
            clamp16(float(v.get('x')) * COORD_SCALE),
            clamp16(float(v.get('y')) * COORD_SCALE),
            clamp16(float(v.get('z')) * COORD_SCALE),
        ))

    norms_el = root.find('normals')
    norms = []
    for v in norms_el.findall('v'):
        norms.append((
            clamp16(float(v.get('x')) * NORMAL_SCALE),
            clamp16(float(v.get('y')) * NORMAL_SCALE),
            clamp16(float(v.get('z')) * NORMAL_SCALE),
        ))

    prims_el = root.find('primitives')
    prims = []
    for poly in prims_el.findall('poly'):
        ptype   = poly.get('type')   # 'F3' or 'F4'
        shading = poly.get('shading', 'F')
        v0 = int(poly.get('v0'));  v1 = int(poly.get('v1'));  v2 = int(poly.get('v2'))
        n0 = int(poly.get('n0'))
        r0 = int(poly.get('r0'));  g0 = int(poly.get('g0'));  b0 = int(poly.get('b0'))

        # SMD_PRI_TYPE byte 0: bits[1:0]=type, bits[3:2]=l_type
        # type: 1=F3, 2=F4   l_type: 1=flat(F), 3=Gouraud(G)
        l_type = 1 if shading == 'F' else 3
        prim = bytearray(20)
        if ptype == 'F3':
            prim[0] = 1 | (l_type << 2)
            prim[3] = 20
            struct.pack_into('<HHH', prim, 4, v0, v1, v2)
            # offset 10-11: padding (0)
            struct.pack_into('<H',   prim, 12, n0)
            # offset 14-15: padding (0)
        else:  # F4
            v3 = int(poly.get('v3'))
            prim[0] = 2 | (l_type << 2)
            prim[3] = 20
            struct.pack_into('<HHHH', prim, 4, v0, v1, v2, v3)
            struct.pack_into('<H',    prim, 12, n0)
            # offset 14-15: padding (0)
        prim[16] = r0;  prim[17] = g0;  prim[18] = b0
        prims.append(bytes(prim))

    n_verts = len(verts);  n_norms = len(norms);  n_prims = len(prims)
    off_verts = 24
    off_norms = off_verts + n_verts * 8
    off_prims = off_norms + n_norms * 8

    out = bytearray()
    out += b'SMD'
    out += struct.pack('B', 1)          # version byte
    out += struct.pack('<H', 3)         # flags (matches existing SMD files)
    out += struct.pack('<HHH', n_verts, n_norms, n_prims)
    out += struct.pack('<III', off_verts, off_norms, off_prims)

    for vx, vy, vz in verts:
        out += struct.pack('<hhhH', vx, vy, vz, 0)
    for nx, ny, nz in norms:
        out += struct.pack('<hhhH', nx, ny, nz, 0)
    for p in prims:
        out += p

    with open(dst_path, 'wb') as f:
        f.write(out)
    print(f"Written: {n_verts} verts, {n_norms} norms, {n_prims} prims -> {dst_path}")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: smx_to_smd.py input.smx output.smd")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
