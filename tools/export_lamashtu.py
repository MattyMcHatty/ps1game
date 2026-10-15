"""Re-export Lamashtu's body and her two clips from "Lamashtu.blend".

    blender -b "blender models/Catacombs/Lamashtu.blend" -P tools/export_lamashtu.py

Then convert the mesh (the skin must already be at textures/lamashtu.tim):

    smxlink -tp textures -o assets\\bosses\\Lamashtu\\Lamashtu.smd ^
            assets\\bosses\\Lamashtu\\Lamashtu.smx

tools/export_asag.py with the names changed, for that script's three reasons:

  1. THE .smd's BIND POSE MUST BE FRAME 1 OF THE CLIP SHE RESTS ON. The SMX
     exporter applies modifiers, so it bakes whatever pose the armature is in
     at export time (tools/ANIMATING_A_3D_MODEL.txt mistake 2). This sets
     Idle_Baked and frame 1 first.

  2. EVERY CLIP IS BAKED PACKED (PVA2). The Blender UI still writes PVA1, a
     quarter bigger, and the clips are read at the arena door.

  3. EVERY CLIP IS BAKED AT step=2: 24 fps authored, 12 fps PLAYED, which is
     LMS_ANIM_TICKS 5 in src/lamashtu.h. 60/12 is whole, so unlike Asag's 8 fps
     the body steps on a plain tick countdown.

THE _Baked ACTIONS, as Asag's were. The blend holds Idle/Shriek and an
Idle_Baked/Shriek_Baked pair over the same frame ranges (1..48 and 1..72); the
baked pair is what an export should evaluate, because it does not depend on
any constraint or driver still resolving the same way headless.

Her mesh is `Cube`, parented to `Armature`. Both exporters bake matrix_world,
so the .smx and the .pva agree on the frame: feet on y=0 at the blend's world
origin, face toward -Z. src/lamashtu.c turns and places her; nothing here does.
"""
import bpy, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "bosses", "Lamashtu")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import io_export_pva as pva
import io_export_smx_v3 as smx
try:
    smx.register()
except Exception:
    pass          # already registered when run inside a configured Blender

MESH = "Cube"
SMX_OUT = "Lamashtu"
BIND_ACTION = "Idle_Baked"

# (pva basename, blender action). The order is src/lamashtu.h's LamashtuClip
# order.
CLIPS = [
    ("Lamashtu_idle",   "Idle_Baked"),
    ("Lamashtu_shriek", "Shriek_Baked"),
]

STEP = 2


def armature_of(obj):
    for m in obj.modifiers:
        if m.type == 'ARMATURE' and m.object:
            return m.object
    return obj.parent


def set_action(obj, action_name):
    """Put `action_name` on the armature and mute the NLA - an unmuted stashed
    track overrides the active action and bakes the wrong clip silently. See
    tools/export_asag.py, which this is copied from."""
    arm = armature_of(obj)
    if arm is None:
        raise RuntimeError("no armature drives %s" % obj.name)
    if arm.animation_data is None:
        arm.animation_data_create()
    for t in arm.animation_data.nla_tracks:
        t.mute = True
    arm.animation_data.action = bpy.data.actions[action_name]
    # Blender 4.4+ actions are SLOTTED; an unassigned slot evaluates to the
    # rest pose on every frame.
    act = arm.animation_data.action
    if hasattr(act, "slots") and len(act.slots):
        arm.animation_data.action_slot = act.slots[0]
    if obj.animation_data:
        obj.animation_data.action = None
    return arm


def clip_range(action):
    fs, fe = int(action.frame_range[0]), int(action.frame_range[1])
    return (fs if fs >= 1 else 1), fe


def main():
    sc = bpy.context.scene
    os.makedirs(OUT_DIR, exist_ok=True)
    obj = bpy.data.objects[MESH]

    print("\n@@@ BIND-POSE MESH (frame 1 of %s)" % BIND_ACTION)
    set_action(obj, BIND_ACTION)
    sc.frame_set(1)
    path = os.path.join(OUT_DIR, SMX_OUT + ".smx")
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_test.smx(filepath=path, exp_scale=100.0, exp_tex_size=128,
                            exp_applyModifiers=True, exp_writeNormals=True)
    print("@@@ SMX %s  (%d verts)" % (SMX_OUT, len(obj.data.vertices)))

    print("\n@@@ CLIPS (step %d)" % STEP)
    total = 0
    for base, action in CLIPS:
        fs, fe = clip_range(bpy.data.actions[action])
        set_action(obj, action)
        out = os.path.join(OUT_DIR, base + ".pva")
        n = pva.export(obj, out, 100.0, fs, fe, None, True, step=STEP,
                       pack=True)
        total += n
        print("@@@ PVA %-20s <- %-24s %3d..%-3d %7d bytes"
              % (base, action, fs, fe, n))
    print("\n@@@ TOTAL PVA %d bytes (%.1f KB)" % (total, total / 1024.0))


main()
