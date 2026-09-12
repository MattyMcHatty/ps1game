"""Re-export Asag's body and its six clips from "Asag Version Two.blend".

    blender -b "blender models/Garden/Asag Version Two.blend" -P tools/export_asag.py

WHAT THIS BOSS IS NOW. ONE mesh and SIX clips, all on the object `Head`. The
first Asag was eight separate parts on eight armatures with fourteen clips
between them; that whole model is gone (see tools/ADDING_THE_ASAG_FIGHT.txt).
This script shrank with it, and most of what made the old one long - joining six
boils, holding a different idle action per part, diffing duplicate actions - has
no subject any more.

WHY IT STILL EXISTS RATHER THAN TWO COMMAND LINES:

  1. THE .smd's BIND POSE MUST BE FRAME 1 OF THE CLIP THE MODEL RESTS ON. The
     SMX exporter applies modifiers, so it bakes whatever pose the armature
     happens to be in when you export. Mistake 2 in
     tools/ANIMATING_A_3D_MODEL.txt is exactly this: the Rabisu's shipped mesh
     sat 216 units off its clip's first frame and playback snapped on the first
     tick. THE HAND EXPORT THIS BOSS ARRIVED AS HAD THE SAME FAULT - it was
     saved with Head_Faint_Baked active and sat up to 164 units off frame 1 of
     the idle, with byte-identical topology and UVs. This script sets
     Head_Idle_Baked and frame 1 before exporting, so the pose the game draws at
     rest IS the pose its idle clip starts from.

  2. EVERY CLIP IS BAKED PACKED (PVA2, pack=True). PVA1 stores a halfword per
     vertex per frame that is always zero, which is a quarter of the payload;
     at six clips that mattered. The unpacked six came to 133,120 bytes
     sector-rounded and the LAST of them - the faint - was REFUSED by
     src/asag.c's heap guard on a real console, so the boss stood on its bind
     pose where that clip should have been. Packed they are 102,400. Do not
     export these through the Blender UI, which still writes PVA1.

  3. EVERY CLIP IS BAKED AT step=3: 24 fps authored, 8 fps PLAYED, which is
     ASAG_ANIM_FPS in src/asag.h. Note 60/8 is 7.5, so asag_update() runs an
     ACCUMULATOR rather than a tick countdown - the first Asag could use a whole
     ASAG_ANIM_TICKS because its 6 fps divided 60 and this one cannot.

     Re-baking a clip by hand WITHOUT --step 3 does not just play it fast, it
     may not FIT - and "not fit" here is not a failed malloc, it is the CD DMA
     writing through the stack. See PART 6A of tools/ADDING_THE_ASAG_FIGHT.txt,
     which is the crash this number comes from.

>>> THE BLEND'S ACTION NAMES DO NOT ALL MATCH THE CLIP NAMES. <<< Four of the
six _Baked actions are named for their clip (Head_Idle_Baked, Head_Emerge_Baked,
Head_Faint_Baked, Head_Attack_Slam_Baked) and two are not: the laser's baked
action is Head_Laser_Baked, not Head_Attack_Laser_Baked, and the vomit's is
Head_Vomit_Baked. Both are unambiguous - each has exactly one candidate and its
frame range matches its plain counterpart's exactly - but the mapping is
explicit in CLIPS below rather than derived, so a rename in the blend fails here
with a KeyError instead of silently baking the rest pose.
"""
import bpy, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "bosses", "Asag Version Two")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import io_export_pva as pva
import io_export_smx_v3 as smx
try:
    smx.register()
except Exception:
    pass          # already registered when run inside a configured Blender

MESH = "Head"
SMX_OUT = "Asag Version Two_Boss"
BIND_ACTION = "Head_Idle_Baked"

# (pva basename, blender action). The order is src/asag.h's AsagClip order and
# the order the demo director cycles them in.
CLIPS = [
    ("Asag_head_idle",   "Head_Idle_Baked"),
    # NO EMERGE. Head_Emerge_Baked is still in the .blend and is deliberately
    # not baked: the travel it did by hand is now the position track in
    # src/asag.c (see asag.h), which every attack carries for itself. Put the
    # row back here, in src/asag.h's AsagClip, in clip_file[] and in disc.xml to
    # bring it back.
    ("Asag_head_laser",  "Head_Laser_Baked"),          # NOT Head_Attack_Laser_Baked
    ("Asag_head_slam",   "Head_Attack_Slam_Baked"),
    ("Asag_head_vomit",  "Head_Vomit_Baked"),          # NOT Head_Attack_Vomit_Baked
    ("Asag_head_faint",  "Head_Faint_Baked"),
]

STEP = 3


def armature_of(obj):
    for m in obj.modifiers:
        if m.type == 'ARMATURE' and m.object:
            return m.object
    return obj.parent


def set_action(obj, action_name):
    """Put `action_name` on whatever drives this mesh, and mute the NLA.

    The blend stashes its spare clips in NLA tracks. An unmuted stashed track
    OVERRIDES the active action and the bake then silently produces the wrong
    clip with no error anywhere, so muting them is the only way to be sure the
    action asked for is the one evaluated."""
    arm = armature_of(obj)
    if arm is None:
        raise RuntimeError("no armature drives %s" % obj.name)
    if arm.animation_data is None:
        arm.animation_data_create()
    for t in arm.animation_data.nla_tracks:
        t.mute = True
    arm.animation_data.action = bpy.data.actions[action_name]
    # Blender 4.4+ actions are SLOTTED. Without a slot assignment the action is
    # attached but evaluates to nothing, and every frame then bakes the REST
    # pose - which looks like a clip that exists and does not move.
    act = arm.animation_data.action
    if hasattr(act, "slots") and len(act.slots):
        arm.animation_data.action_slot = act.slots[0]
    # An action on the MESH would compose on top of the armature's.
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
