bl_info = {
    "name": "Export PS1 Vertex Animation (.pva)",
    "author": "ps1game",
    "version": (1, 0),
    "blender": (4, 0, 0),
    "location": "File > Export > PS1 Vertex Animation (.pva)",
    "description": "Bake a skinned/armature animation to per-frame vertex positions",
    "category": "Import-Export",
}

"""
Bake an armature animation down to per-frame VERTEX POSITIONS (.pva).

WHY BAKED VERTICES AND NOT BONES
--------------------------------
The Rabisu is smooth-skinned with up to THIRTEEN bone influences per vertex.
That rules out both PS1-era alternatives: the mesh cannot be split into rigid
per-bone pieces, and evaluating 13 weights across 496 vertices every frame is
far outside the GTE's budget. So Blender does the skinning ONCE, at export
time, and the console just plays back arrays of finished vertex positions.

Rig complexity is therefore FREE — bone count, IK, constraints, drivers, shape
keys, any modifier at all. Only the vertex COUNT and ORDER have to stay fixed.

THE ONE INVARIANT THAT MATTERS
------------------------------
A .pva carries positions only. The topology — which vertices each polygon uses,
and its colour — still comes from the .smd built by smxlink from the .smx. So
frame N of the .pva must be indexable by the .smd's polygon vertex indices,
which means:

    THE VERTEX ORDER HERE MUST MATCH THE .smx THAT MADE THE .smd.

That holds as long as both are exported from the SAME mesh object with the same
modifier stack, because both walk `mesh.vertices` in order. It is verified for
real at load time anyway: rabisus_load_assets refuses a .pva whose vertex count
disagrees with the .smd and falls back to the bind pose rather than drawing
garbage. Re-export BOTH files whenever you change the mesh's topology.

The coordinate transform below is copied verbatim from io_export_smx_v3.py
(world matrix, then x, -z, y, times the scale). Keep the two in step; if that
exporter's transform ever changes, this one must change with it.

USAGE — as a Blender add-on
---------------------------
    Edit > Preferences > Add-ons > Install... > pick this file > enable it
    Select the MESH object (not the armature), then
    File > Export > PS1 Vertex Animation (.pva)

USAGE — headless, which is how the build actually does it
---------------------------------------------------------
    blender -b "blender models/Rabisu-animated.blend" -P tools/io_export_pva.py -- \
        --object "Cube.008" --out assets/bosses/Rabisu_idle.pva --scale 100

    Options:  --start N --end N     frame range (default: the scene's)
              --no-trim-loop        keep the last frame even if it duplicates
                                    the first (default is to drop it)
              --fps N               stamped in the header; informational only,
                                    the game's own RBS_ANIM_TICKS sets the rate

FILE FORMAT (.pva, little-endian)
---------------------------------
    0   4   magic "PVA1"
    4   2   n_verts      must equal the .smd's vertex count
    6   2   n_frames
    8   2   fps          authoring rate, informational
    10  2   flags        bit 0 = loops seamlessly
    12  ..  n_frames * n_verts * 8 bytes
            each vertex an SVECTOR: int16 vx, vy, vz, pad(0)

    Header is 12 bytes so every SVECTOR lands 4-byte aligned in a buffer read
    off the CD, which is what the GTE load macros want.
"""

import os
import struct
import sys

import bpy

MAGIC = b"PVA1"
HEADER_SIZE = 12
FLAG_LOOP = 1

INT16_MIN, INT16_MAX = -32768, 32767


def _frame_positions(obj, scale):
    """This frame's vertex positions, in PS1 space, in mesh.vertices order.

    Transform copied verbatim from io_export_smx_v3.py. `matrix_world` comes
    from the ORIGINAL object (as there), not the evaluated copy.
    """
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh = obj_eval.to_mesh()
    try:
        world = obj.matrix_world
        out = []
        for v in mesh.vertices:
            w = world @ v.co
            out.append((w.x * scale, -w.z * scale, w.y * scale))
        return out
    finally:
        obj_eval.to_mesh_clear()


def bake(obj, frame_start, frame_end, scale, trim_loop=True, report=print):
    """Evaluate `obj` over the frame range. Returns (frames, looped)."""
    scene = bpy.context.scene
    saved = scene.frame_current

    frames = []
    try:
        for f in range(frame_start, frame_end + 1):
            scene.frame_set(f)
            frames.append(_frame_positions(obj, scale))
    finally:
        scene.frame_set(saved)

    if not frames:
        raise RuntimeError("empty frame range")

    n = len(frames[0])
    for i, fr in enumerate(frames):
        if len(fr) != n:
            raise RuntimeError(
                "vertex count changed at frame %d (%d, expected %d). A modifier "
                "is generating geometry per frame; bake or disable it."
                % (frame_start + i, len(fr), n))

    # A seamless loop's last frame is a duplicate of its first. Shipping both
    # holds the pose for one extra tick every cycle, which reads as a hitch.
    looped = False
    if len(frames) > 1:
        drift = max(abs(a - b)
                    for va, vb in zip(frames[0], frames[-1])
                    for a, b in zip(va, vb))
        if drift < 0.5:                      # under half a PS1 unit
            looped = True
            if trim_loop:
                report("  last frame duplicates the first (max drift %.3f) "
                       "-> dropped, clip loops" % drift)
                frames.pop()

    return frames, looped


def write_pva(path, frames, fps, looped, report=print):
    n_verts = len(frames[0])
    clamped = 0

    body = bytearray()
    for fr in frames:
        for (x, y, z) in fr:
            vals = []
            for c in (x, y, z):
                i = int(round(c))
                if i < INT16_MIN or i > INT16_MAX:
                    i = max(INT16_MIN, min(INT16_MAX, i))
                    clamped += 1
                vals.append(i)
            body += struct.pack("<hhhh", vals[0], vals[1], vals[2], 0)

    flags = FLAG_LOOP if looped else 0
    header = MAGIC + struct.pack("<HHHH", n_verts, len(frames), fps, flags)

    with open(path, "wb") as f:
        f.write(header)
        f.write(body)

    if clamped:
        report("  WARNING: %d coordinates clamped to int16 - the model is "
               "larger than the engine's vertex range" % clamped)

    report("  wrote %s" % path)
    report("  %d frames x %d verts = %d bytes (%.1f KB), loops=%s"
           % (len(frames), n_verts, HEADER_SIZE + len(body),
              (HEADER_SIZE + len(body)) / 1024.0, looped))

    # The union bounding box over ALL frames, which is what the collision
    # cylinder and the gun's aim box have to cover -- a static-mesh bbox is a
    # LOWER bound once the thing animates.
    lo = [min(v[i] for fr in frames for v in fr) for i in range(3)]
    hi = [max(v[i] for fr in frames for v in fr) for i in range(3)]
    report("  union bbox over all frames (PS1 units, -Y is up):")
    for i, axis in enumerate("xyz"):
        report("    %s [%7.1f, %7.1f]  span %.1f" % (axis, lo[i], hi[i], hi[i] - lo[i]))
    report("    -> RBS_FOOT_OFF %d, RBS_HEIGHT %d, RBS_HALF_W %d"
           % (round(-hi[1]), round(hi[1] - lo[1]),
              round(max(abs(lo[0]), abs(hi[0])))))
    return HEADER_SIZE + len(body)


def export(obj, path, scale=100.0, frame_start=None, frame_end=None,
           fps=None, trim_loop=True, report=print):
    scene = bpy.context.scene
    if frame_start is None:
        frame_start = scene.frame_start
    if frame_end is None:
        frame_end = scene.frame_end
    if fps is None:
        fps = scene.render.fps

    report("baking %s frames %d..%d at scale %g"
           % (obj.name, frame_start, frame_end, scale))
    frames, looped = bake(obj, frame_start, frame_end, scale, trim_loop, report)
    return write_pva(path, frames, fps, looped, report)


# --------------------------------------------------------------------------
# Blender add-on
# --------------------------------------------------------------------------
try:
    from bpy.props import BoolProperty, FloatProperty, IntProperty, StringProperty
    from bpy_extras.io_utils import ExportHelper

    class ExportPVA(bpy.types.Operator, ExportHelper):
        """Bake the armature animation to per-frame vertex positions"""
        bl_idname = "export_scene.pva"
        bl_label = "Export PS1 Vertex Animation"
        filename_ext = ".pva"
        filter_glob: StringProperty(default="*.pva", options={'HIDDEN'})

        exp_scale: FloatProperty(
            name="Scale", default=100.0,
            description="1 Blender unit = this many PS1 units. MUST match the "
                        "scale used for the .smx")
        exp_start: IntProperty(name="Start Frame", default=0,
                               description="0 = the scene's start frame")
        exp_end: IntProperty(name="End Frame", default=0,
                             description="0 = the scene's end frame")
        exp_trim: BoolProperty(
            name="Trim Duplicate Last Frame", default=True,
            description="Drop the last frame when it duplicates the first, so a "
                        "seamless loop does not hold that pose for an extra tick")

        def execute(self, context):
            obj = context.object
            if obj is None or obj.type != 'MESH':
                self.report({'ERROR'}, "Select the MESH object, not the armature")
                return {'CANCELLED'}
            path = bpy.path.ensure_ext(self.filepath, self.filename_ext)
            try:
                export(obj, path, self.exp_scale,
                       self.exp_start or None, self.exp_end or None,
                       None, self.exp_trim,
                       report=lambda m: self.report({'INFO'}, m))
            except Exception as e:                        # noqa: BLE001
                self.report({'ERROR'}, str(e))
                return {'CANCELLED'}
            return {'FINISHED'}

    def menu_func(self, context):
        self.layout.operator(ExportPVA.bl_idname, text="PS1 Vertex Animation (.pva)")

    def register():
        bpy.utils.register_class(ExportPVA)
        bpy.types.TOPBAR_MT_file_export.append(menu_func)

    def unregister():
        bpy.types.TOPBAR_MT_file_export.remove(menu_func)
        bpy.utils.unregister_class(ExportPVA)

except ImportError:       # running headless without the UI modules
    def register():
        pass

    def unregister():
        pass


# --------------------------------------------------------------------------
# Headless entry point:  blender -b file.blend -P this.py -- --object X --out Y
# --------------------------------------------------------------------------
def _main(argv):
    import argparse
    p = argparse.ArgumentParser(prog="io_export_pva")
    p.add_argument("--object", required=True, help="mesh object name")
    p.add_argument("--out", required=True, help="output .pva path")
    p.add_argument("--scale", type=float, default=100.0)
    p.add_argument("--start", type=int, default=None)
    p.add_argument("--end", type=int, default=None)
    p.add_argument("--fps", type=int, default=None)
    p.add_argument("--no-trim-loop", action="store_true")
    a = p.parse_args(argv)

    obj = bpy.data.objects.get(a.object)
    if obj is None:
        sys.exit("no object named %r (have: %s)"
                 % (a.object, [o.name for o in bpy.data.objects]))
    if obj.type != 'MESH':
        sys.exit("%r is a %s, not a MESH" % (a.object, obj.type))

    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    export(obj, out, a.scale, a.start, a.end, a.fps, not a.no_trim_loop)


if __name__ == "__main__":
    if "--" in sys.argv:
        _main(sys.argv[sys.argv.index("--") + 1:])
    else:
        register()
