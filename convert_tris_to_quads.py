"""
Convert consecutive F3 triangle pairs in an SMD file to F4 quads.

SMD header (24 bytes):
  [0-3]   "SMD\x01"
  [4-5]   uint16 version
  [6-7]   uint16 n_verts
  [8-9]   uint16 n_norms
  [10-11] uint16 n_prims
  [12-15] uint32 offset_verts
  [16-19] uint32 offset_norms
  [20-23] uint32 offset_prims

F3 prim (stride=20): [0-3] hdr, [4-9] v0,v1,v2 (uint16), [10-11] PAD,
                      [12-13] n0, [14-15] PAD, [16-18] rgb, [19] code
F4 prim (stride=20): [0-3] hdr, [4-11] v0,v1,v2,v3 (uint16),
                      [12-13] n0, [14-15] PAD, [16-18] rgb, [19] code
"""
import struct, sys

def convert(src_path, dst_path):
    with open(src_path, 'rb') as f:
        data = bytearray(f.read())

    magic    = data[0:4]
    version  = struct.unpack_from('<H', data, 4)[0]
    n_verts  = struct.unpack_from('<H', data, 6)[0]
    n_norms  = struct.unpack_from('<H', data, 8)[0]
    n_prims  = struct.unpack_from('<H', data, 10)[0]
    off_prims = struct.unpack_from('<I', data, 20)[0]
    print(f"SMD v{version}: {n_verts} verts, {n_norms} norms, {n_prims} prims, prims@{off_prims}")

    # Collect primitives
    prims = []
    p = off_prims
    for _ in range(n_prims):
        hdr0   = data[p]
        stride = data[p + 3]
        ptype  = hdr0 & 0x03
        prims.append((ptype, stride, bytes(data[p:p + stride])))
        p += stride

    def tri_verts(pb):
        return struct.unpack_from('<HHH', pb, 4)

    def quad_n0(pb):
        return struct.unpack_from('<H', pb, 12)[0]

    def shared_edge(t1, t2):
        s = set(t1) & set(t2)
        if len(s) != 2:
            return None
        u1 = [v for v in t1 if v not in s]
        u2 = [v for v in t2 if v not in s]
        if len(u1) != 1 or len(u2) != 1:
            return None
        return list(s), u1[0], u2[0]

    def make_quad(pb1, va, vb, vc, vd):
        out = bytearray(20)
        # byte0: change type bits 01 -> 10
        out[0] = (pb1[0] & ~0x03) | 0x02
        out[1] = pb1[1]; out[2] = pb1[2]; out[3] = pb1[3]
        struct.pack_into('<HHHH', out, 4, vc, va, vb, vd)
        struct.pack_into('<H', out, 12, quad_n0(pb1))
        out[14] = 0; out[15] = 0
        out[16] = pb1[16]; out[17] = pb1[17]; out[18] = pb1[18]
        out[19] = pb1[19]
        return bytes(out)

    out_prims = []
    paired = 0
    kept = 0
    i = 0
    while i < len(prims):
        ptype1, stride1, pb1 = prims[i]
        if ptype1 == 1 and stride1 == 20 and i + 1 < len(prims):
            ptype2, stride2, pb2 = prims[i + 1]
            if ptype2 == 1 and stride2 == 20 and quad_n0(pb1) == quad_n0(pb2):
                t1 = tri_verts(pb1)
                t2 = tri_verts(pb2)
                result = shared_edge(t1, t2)
                if result is not None:
                    shared, uc1, uc2 = result
                    va, vb = shared[0], shared[1]
                    quad_bytes = make_quad(pb1, va, vb, uc1, uc2)
                    out_prims.append(quad_bytes)
                    paired += 1
                    i += 2
                    continue
        out_prims.append(pb1)
        kept += 1
        i += 1

    print(f"Paired {paired} tri pairs -> quads, kept {kept} solo tris")
    print(f"Output: {len(out_prims)} primitives")

    # Rebuild: copy header verbatim, update n_prims, then all prims
    out_data = bytearray(data[:off_prims])
    struct.pack_into('<H', out_data, 10, len(out_prims))
    for pb in out_prims:
        out_data.extend(pb)

    with open(dst_path, 'wb') as f:
        f.write(out_data)
    print(f"Written: {dst_path}")

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\virtu\Documents\ps1game\assets\testroom_flipped2.smd'
    dst = sys.argv[2] if len(sys.argv) > 2 else r'C:\Users\virtu\Documents\ps1game\assets\testroom_quads.smd'
    convert(src, dst)
