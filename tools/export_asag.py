"""Re-export every Asag part from blender models/Garden/Asag.blend.

    blender -b "blender models/Garden/Asag.blend" -P tools/export_asag.py

WHY THIS EXISTS RATHER THAN FIFTEEN COMMAND LINES. Asag is not one model, it is
THIRTEEN drawn parts and TWELVE clips that all have to agree with each other,
and two of the agreements are easy to break by hand:

  1. THE .smd's BIND POSE MUST BE FRAME 1 OF THE PART'S IDLE CLIP. The SMX
     exporter applies modifiers, so it bakes whatever pose the armature happens
     to be in when you export. Mistake 2 in tools/ANIMATING_A_3D_MODEL.txt is
     exactly this: the Rabisu's shipped mesh sat 216 units off its clip's first
     frame, so playback snapped on the first tick. This script assigns the idle
     action and sets frame 1 before every mesh export, so the part the game
     draws at rest IS the pose its idle clip starts from.

  2. EVERY CLIP IS BAKED AT step=4, WHICH IS QUARTER RATE: 24 fps authored, 6 fps
     played -> ASAG_ANIM_TICKS 10. If a clip is ever re-baked by hand, PASS
     --step 4 OR IT WILL NOT FIT, and "not fit" here does not mean a failed
     malloc - it means the CD DMA writing through the stack.

     THE NUMBER TO BEAT IS 171 KB, NOT 371. tools/heap_budget.py used to report
     371 KB free because it counted the top 145 KB of RAM as heap when it is
     actually main()'s RenderContext sitting on the stack. Asag was sized
     against that phantom figure at step 2 (229 KB of meshes and clips) and the
     arena crashed inside CdReadSync every time, with sp = 0x801DBD40 INSIDE the
     buffer being read. The script is fixed; the real budget at the arena door is
     171 KB measured, and step 4 fits it in ~119 KB.

It also resolves the blend's DUPLICATE ACTIONS. Several clips exist twice, once
plain and once "_Baked" (a constraint bake). The exporter evaluates the
depsgraph, which resolves constraints anyway, so the two should be identical;
the script bakes both and reports whether they are, rather than assuming.
"""
import bpy, os, sys, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SMX_DIR = os.path.join(ROOT, "assets", "garden", "asag")
PVA_DIR = os.path.join(ROOT, "assets", "bosses", "Asag")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import io_export_pva as pva
import io_export_smx_v3 as smx
try:
    smx.register()
except Exception:
    pass

# blender object -> (smx basename, the action to hold for the bind pose)
PARTS = [
    ("Head",               "Asag_Body",            "Head_Idle"),
    ("Leaf_North",         "Asag_Top_Leaf",        "Leaf_OpenClose"),
    ("Leaf_East",          "Asag_Right_Leaf",      "Leaf_OpenClose.003"),
    ("Leaf_South",         "Asag_Bottom_Leaf",     "Leaf_OpenClose.001"),
    ("Leaf_West",          "Asag_Left_Leaf",       "Leaf_OpenClose.002"),
    ("Tentacle_Left",      "Asag_Left_Tentacle",   "Tentacle_Idle"),
    ("Tentacle_Right.001", "Asag_Right_Tentacle",  "Tentacle_Idle.001"),
]

# The six boils ship as ONE joined mesh, not six.
#
# They never animate, they are always drawn together, they share one texture and
# one draw loop, and they sit at fixed world positions that the export bakes in -
# so six files buy nothing a join does not. What they COST is six ISO9660
# directory records in \TEX\, and that directory is the scarcest thing on this
# disc: the note in src/chain_room.c records a boot crash caused by growing it by
# TWO files. Joining saves five records outright.
#
# Blender source order fixes which boil is which; the join preserves it, so
# Boil One's five quads are prims 0..4, Boil Two's 5..9, and so on. That is what
# lets asag.c address one boil (to pop it) without six meshes.
BOILS = ["Sphere.001", "Sphere", "Sphere.002",
         "Sphere.005", "Sphere.004", "Sphere.003"]   # One..Six
BOILS_OUT = "Asag_Boils"

# blender object -> (pva basename, action, duplicate actions to diff against)
CLIPS = [
    ("Head", "Asag_head_idle",    "Head_Idle",          ["Head_Idle_Baked"]),
    ("Head", "Asag_head_speak",   "Head_Speak",         ["Head_Speak_Baked"]),
    ("Head", "Asag_head_emerge",  "Head_Emerge",        ["Head_Emerge_Baked"]),
    ("Head", "Asag_head_retract", "Head_Retract",       ["Head_Retract_Baked"]),
    ("Head", "Asag_head_vomit",   "Head_Attack_Vomit",  ["Head_Vomit_Baked"]),
    ("Head", "Asag_head_laser",   "Head_Attack_Laser",  ["Head_Laser_Baked"]),
    ("Leaf_North", "Asag_leaf_top",    "Leaf_OpenClose",     []),
    ("Leaf_East",  "Asag_leaf_right",  "Leaf_OpenClose.003", []),
    ("Leaf_South", "Asag_leaf_bottom", "Leaf_OpenClose.001", []),
    ("Leaf_West",  "Asag_leaf_left",   "Leaf_OpenClose.002", []),
    ("Tentacle_Left",      "Asag_tent_l_idle", "Tentacle_Idle",     []),
    ("Tentacle_Left",      "Asag_tent_l_atk",  "Teneacle_Left_Attack",
     ["Tentacle_Left_Attack_Baked", "Teneacle_Left_Baked"]),
    ("Tentacle_Right.001", "Asag_tent_r_idle", "Tentacle_Idle.001", []),
    ("Tentacle_Right.001", "Asag_tent_r_atk",  "Tentacle_Right_Attack",
     ["Tentacle_Right_Attack_Baked", "Tentacle_Right_Baked"]),
]

STEP = 4


def armature_of(obj):
    for m in obj.modifiers:
        if m.type == 'ARMATURE' and m.object:
            return m.object
    return obj.parent


def set_action(obj, action_name):
    """Put `action_name` on whatever drives this mesh, and mute the NLA.

    The blend stashes spare clips in NLA tracks. An unmuted stashed track
    overrides the active action, and the bake then silently produces the WRONG
    clip with no error anywhere - so muting them is the only way to be sure the
    action asked for is the one that gets evaluated."""
    arm = armature_of(obj)
    if arm is None:
        raise RuntimeError("no armature for %s" % obj.name)
    if arm.animation_data is None:
        arm.animation_data_create()
    for t in arm.animation_data.nla_tracks:
        t.mute = True
    arm.animation_data.action = bpy.data.actions[action_name]
    # Blender 4.4+ actions are SLOTTED. Without a slot assignment the action is
    # attached but evaluates to nothing, and every frame bakes the rest pose -
    # which looks like a clip that exists and does not move.
    act = arm.animation_data.action
    if hasattr(act, "slots") and len(act.slots):
        arm.animation_data.action_slot = act.slots[0]
    # An action on the MESH would compose on top of the armature's.
    if obj.animation_data:
        obj.animation_data.action = None
    return arm


def export_smx(obj, base):
    path = os.path.join(SMX_DIR, base + ".smx")
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_test.smx(filepath=path, exp_scale=100.0, exp_tex_size=128,
                            exp_applyModifiers=True, exp_writeNormals=True)
    return path


def clip_range(action):
    fs, fe = int(action.frame_range[0]), int(action.frame_range[1])
    return (fs if fs >= 1 else 1), fe


def main():
    sc = bpy.context.scene
    os.makedirs(PVA_DIR, exist_ok=True)

    print("\n@@@ BIND-POSE MESHES (frame 1 of each part's idle clip)")
    for obj_name, base, idle in PARTS:
        obj = bpy.data.objects[obj_name]
        if idle:
            set_action(obj, idle)
        sc.frame_set(1)
        export_smx(obj, base)
        print("@@@ SMX %-24s <- %-20s pose=%s"
              % (base, obj_name, idle or "static"))

    # The boils, joined. DUPLICATES are joined so the blend's own objects are
    # left alone - this script has to stay re-runnable without mutating its
    # source, and bpy.ops.object.join() destroys everything but the active
    # object.
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    copies = []
    for n in BOILS:
        src = bpy.data.objects[n]
        cp = src.copy()
        cp.data = src.data.copy()
        bpy.context.collection.objects.link(cp)
        copies.append(cp)
    for c in copies:
        c.select_set(True)
    bpy.context.view_layer.objects.active = copies[0]
    bpy.ops.object.join()
    joined = bpy.context.view_layer.objects.active
    joined.name = BOILS_OUT
    export_smx(joined, BOILS_OUT)
    print("@@@ SMX %-24s <- %d boils joined (%d verts)"
          % (BOILS_OUT, len(BOILS), len(joined.data.vertices)))
    bpy.data.objects.remove(joined, do_unlink=True)

    print("\n@@@ CLIPS (step %d)" % STEP)
    total = 0
    for obj_name, base, action, alts in CLIPS:
        obj = bpy.data.objects[obj_name]
        fs, fe = clip_range(bpy.data.actions[action])
        set_action(obj, action)
        out = os.path.join(PVA_DIR, base + ".pva")
        n = pva.export(obj, out, 100.0, fs, fe, None, True, step=STEP)
        total += n
        digest = hashlib.md5(open(out, "rb").read()).hexdigest()
        same, diff = [], []
        for a in alts:
            afs, afe = clip_range(bpy.data.actions[a])
            set_action(obj, a)
            tmp = out + ".alt"
            pva.export(obj, tmp, 100.0, afs, afe, None, True,
                       report=lambda *x: None, step=STEP)
            (same if hashlib.md5(open(tmp, "rb").read()).hexdigest() == digest
             else diff).append(a)
            os.remove(tmp)
        print("@@@ PVA %-18s <- %-22s %3d..%-3d %6d bytes  same=%s differs=%s"
              % (base, action, fs, fe, n, same, diff))
    print("\n@@@ TOTAL PVA %d bytes (%.1f KB)" % (total, total / 1024.0))


main()
