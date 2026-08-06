"""Check that an .smd's polygon winding faces the right way for the engine.

WHY THIS EXISTS
---------------
The draw loops backface-cull with gte_nclip and skip anything with nclip <= 0.
Get a model's winding backwards and you see its INSIDE: the silhouette is still
roughly right, so it reads as "odd shading" rather than "broken", and it is very
easy to ship. The Rabisu shipped that way — its mesh was built by mirroring, and
Blender's mirror does not flip normals, so four of its ten shells (one wing and
one set of legs) were inverted while the other six were fine.

HOW IT DECIDES
--------------
Signed volume, via the divergence theorem: sum v0 . (v1 x v2) / 6 over every
triangle. For a closed surface that is the enclosed volume, and its SIGN is the
winding. Unlike "do normals point away from the centroid", it does not care
whether the shape is convex -- which matters, because a creature is not, and the
centroid test splits ~50/50 on one no matter which way it is wound.

Every model the game draws correctly (crate, concrete block, save point) comes
out NEGATIVE, so negative is the convention. That is an empirical fact about the
exporter's quad reordering plus the GTE's screen-space handedness, not a choice.

It also splits the mesh into connected SHELLS and reports each one, because a
mirrored model typically has some shells right and some wrong -- a whole-model
figure can average out to the correct sign while half the thing is inverted.

USAGE
    py tools/check_model_winding.py assets/bosses/Rabisu.smd
    py tools/check_model_winding.py assets/props/*.smd

Exit code 1 if any shell is wound the wrong way.

THE FIX, when it fails: in Blender, select the mesh, Edit Mode, select all
(A), then Mesh > Normals > Recalculate Outside (Shift+N). Re-export and re-run.
"""

import glob
import struct
import sys

# Empirical: every model the game renders correctly has negative signed volume.
EXPECTED_SIGN = -1


def load(path):
    d = open(path, 'rb').read()
    if d[:3] != b'SMD':
        raise ValueError("not an SMD (no 'SMD' magic)")
    n_verts, _n_norms, n_prims = struct.unpack('<HHH', d[6:12])
    p_verts, _p_norms, p_prims = struct.unpack('<III', d[12:24])

    verts = [struct.unpack('<hhh', d[p_verts + i * 8:p_verts + i * 8 + 6])
             for i in range(n_verts)]
    prims, o = [], p_prims
    for _ in range(n_prims):
        length = d[o + 3]
        n = 4 if (d[o] & 3) >= 2 else 3
        prims.append(struct.unpack('<' + 'H' * n, d[o + 4:o + 4 + 2 * n]))
        o += length
    return verts, prims


def triangles(prims):
    """A quad is a GPU strip v0,v1,v2,v3 -> tris (v0,v1,v2) and (v2,v1,v3).

    The second triangle's order is reversed on purpose: that is what keeps a
    strip's two halves consistently wound.
    """
    for idx in prims:
        if len(idx) == 3:
            yield idx
        else:
            yield (idx[0], idx[1], idx[2])
            yield (idx[2], idx[1], idx[3])


def signed_volume(verts, prims):
    total = 0.0
    for a, b, c in triangles(prims):
        p, q, r = verts[a], verts[b], verts[c]
        total += (p[0] * (q[1] * r[2] - q[2] * r[1])
                  - p[1] * (q[0] * r[2] - q[2] * r[0])
                  + p[2] * (q[0] * r[1] - q[1] * r[0])) / 6.0
    return total


def shells(prims):
    """Group primitives into connected components (union-find over vertices)."""
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for idx in prims:
        for v in idx[1:]:
            a, b = find(idx[0]), find(v)
            if a != b:
                parent[a] = b

    groups = {}
    for idx in prims:
        groups.setdefault(find(idx[0]), []).append(idx)
    return sorted(groups.values(), key=len, reverse=True)


def open_edges(prims):
    """Directed edges with no opposing partner: >0 means the shell is not closed."""
    seen = {}
    for a, b, c in triangles(prims):
        for e in ((a, b), (b, c), (c, a)):
            if (e[1], e[0]) in seen and seen[(e[1], e[0])] > 0:
                seen[(e[1], e[0])] -= 1
            else:
                seen[e] = seen.get(e, 0) + 1
    return sum(n for n in seen.values() if n > 0)


def check(path):
    try:
        verts, prims = load(path)
    except Exception as e:                                    # noqa: BLE001
        print("%-42s ERROR: %s" % (path, e))
        return False

    total = signed_volume(verts, prims)
    parts = shells(prims)
    bad = []
    for i, part in enumerate(parts):
        v = signed_volume(verts, part)
        if v * EXPECTED_SIGN < 0:
            bad.append((i, len(part), v))

    status = "OK" if not bad else "%d/%d SHELLS INSIDE-OUT" % (len(bad), len(parts))
    print("%-42s %-24s volume %+13.0f  shells %d  open edges %d"
          % (path, status, total, len(parts), open_edges(prims)))
    for i, n, v in bad:
        print("      shell %-3d %4d prims  volume %+13.0f  <-- wound backwards"
              % (i, n, v))
    return not bad


def main(argv):
    paths = []
    for a in argv:
        paths.extend(glob.glob(a) or [a])
    if not paths:
        print(__doc__)
        return 2
    ok = True
    for p in paths:
        ok &= check(p)
    if not ok:
        print("\nFIX: in Blender select the mesh, Edit Mode, A to select all,")
        print("     Mesh > Normals > Recalculate Outside (Shift+N). Re-export.")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
