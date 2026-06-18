# This plugin is part of Scarlet Engine (formerly Project Scarlet)
# Updated for Blender 4.x/5.x compatibility by Claude
# Fixed to preserve quads and export real world coordinates

"""
This script exports Scarlet Game Engine SDK compatible SMX files.
Supports normals, colors and texture mapped triangles and quads.
Exports actual world-space coordinates, not normalized values.
Only one object can be exported at a time.
"""

import bpy
import math
import mathutils
from bpy.props import StringProperty, BoolProperty, FloatProperty, IntProperty
from bpy_extras.io_utils import ExportHelper

bl_info = {
    "name":         "Export: Project Scarlet SMX Raw Model",
    "author":       "Lameguy64, updated for Blender 4/5 by Claude",
    "blender":      (4, 0, 0),
    "version":      (3, 4, 0),
    "location":     "File > Export",
    "description":  "Export mesh to Project Scarlet SMX model format (real coordinates, preserves quads)",
    "category":     "Import-Export"
}


def get_material_base_color(obj, material_index):
    """Return a polygon's material base colour as (r, g, b) floats in 0-1.

    Looks up the Principled BSDF "Base Color" first, then any node exposing a
    "Base Color"/"Color" input, then the material's viewport display colour,
    finally falling back to mid-grey when no material is present.
    """
    if 0 <= material_index < len(obj.material_slots):
        mat = obj.material_slots[material_index].material
        if mat:
            if mat.use_nodes and mat.node_tree:
                # Prefer the Principled BSDF base colour.
                for node in mat.node_tree.nodes:
                    if node.type == 'BSDF_PRINCIPLED':
                        inp = node.inputs.get("Base Color")
                        if inp is not None:
                            v = inp.default_value
                            return (v[0], v[1], v[2])
                # Otherwise any node with a usable colour input.
                for node in mat.node_tree.nodes:
                    inp = node.inputs.get("Base Color") or node.inputs.get("Color")
                    if inp is not None and hasattr(inp, "default_value"):
                        v = inp.default_value
                        try:
                            return (v[0], v[1], v[2])
                        except TypeError:
                            pass
            # Fallback: viewport display colour.
            dc = mat.diffuse_color
            return (dc[0], dc[1], dc[2])
    return (0.5, 0.5, 0.5)


class ExportSMX(bpy.types.Operator, ExportHelper):

    bl_idname    = "export_test.smx"
    bl_label     = "Export SMX"
    filename_ext = ".smx"

    filter_glob: StringProperty(default="*.smx", options={'HIDDEN'})

    exp_applyModifiers: BoolProperty(
        name="Apply Modifiers",
        description="Apply modifiers to the exported mesh",
        default=True,
    )

    exp_writeNormals: BoolProperty(
        name="Normals",
        description="Export normals for smooth and hard shaded faces",
        default=True,
    )

    exp_scale: FloatProperty(
        name="Scale",
        description="Multiply all coordinates by this value (1.0 = export as-is, 100.0 = 1 Blender unit becomes 100 PS1 units)",
        default=100.0,
        min=0.001,
        max=100000.0,
    )

    exp_tex_size: IntProperty(
        name="Texture Size",
        description="VRAM texture size in texels that UVs are scaled to (must match the in-game texture size and the renderer's texture window; 128 for 8bpp 128x128 textures)",
        default=128,
        min=8,
        max=256,
    )

    def execute(self, context):
        obj = context.object
        if obj is None or obj.type != 'MESH':
            self.report({'ERROR'}, "No mesh object selected")
            return {'CANCELLED'}

        depsgraph = context.evaluated_depsgraph_get()
        obj_eval = obj.evaluated_get(depsgraph) if self.exp_applyModifiers else obj
        mesh = obj_eval.to_mesh()

        # Get the world matrix to apply object transforms (location, rotation, scale)
        world_matrix = obj.matrix_world
        normal_matrix = world_matrix.inverted().transposed().to_3x3()

        scale = self.exp_scale

        for p in mesh.polygons:
            if len(p.vertices) > 4:
                self.report({'WARNING'}, "Mesh has n-gons. Please convert to quads/tris first.")
                break

        filepath = bpy.path.ensure_ext(self.filepath, self.filename_ext)

        has_flats = any(not p.use_smooth for p in mesh.polygons)
        flatnorms_start = len(mesh.vertices)

        uv_layer = mesh.uv_layers.active
        mesh_uvs = uv_layer.data if uv_layer else None

        mesh_cols = None
        if mesh.color_attributes:
            mesh_cols = mesh.color_attributes.active_color

        tex_files = []
        tex_table = []

        if mesh_uvs is not None:
            for p in mesh.polygons:
                img = None
                if p.material_index < len(obj.material_slots):
                    mat = obj.material_slots[p.material_index].material
                    if mat and mat.use_nodes:
                        for node in mat.node_tree.nodes:
                            if node.type == 'TEX_IMAGE' and node.image:
                                img = node.image
                                break
                if img is not None:
                    tex_name = bpy.path.display_name_from_filepath(img.filepath)
                    if tex_name in tex_files:
                        tex_table.append(tex_files.index(tex_name) + 1)
                    else:
                        tex_files.append(tex_name)
                        tex_table.append(len(tex_files))
                else:
                    tex_table.append(0)
        else:
            tex_table = None
            tex_files = None

        poly_count = len(mesh.polygons)

        with open(filepath, "w") as f:
            f.write("<!-- Created using Project Scarlet SMX Export Plug-in for Blender (Blender 4/5, real coords, quad support) -->\n")
            f.write("<!-- Scale factor used: %f (1 Blender unit = %f PS1 units) -->\n" % (scale, scale))
            f.write("<model version=\"1\">\n")

            # Write vertices using world matrix so scale/rotation/location are baked in
            # PS1 coordinate system: swap Y and Z, negate new Y (old Z)
            f.write("<vertices count=\"%d\">\n" % len(mesh.vertices))
            for v in mesh.vertices:
                # Apply world matrix to get real world position
                world_co = world_matrix @ v.co
                x = world_co.x * scale
                y = -world_co.z * scale
                z = world_co.y * scale
                f.write("<v x=\"%f\" y=\"%f\" z=\"%f\"/>\n" % (x, y, z))
            f.write("</vertices>\n")

            if self.exp_writeNormals:
                normal_count = len(mesh.vertices) + (len(mesh.polygons) if has_flats else 0)
                f.write("<normals count=\"%d\">\n" % normal_count)
                f.write("<!-- Smooth normals begin here -->\n")
                for v in mesh.vertices:
                    world_normal = (normal_matrix @ v.normal).normalized()
                    f.write("<v x=\"%f\" y=\"%f\" z=\"%f\"/>\n" % (
                        world_normal.x, -world_normal.z, world_normal.y))
                if has_flats:
                    f.write("<!-- Flat normals begin here -->\n")
                    for p in mesh.polygons:
                        world_normal = (normal_matrix @ p.normal).normalized()
                        f.write("<v x=\"%f\" y=\"%f\" z=\"%f\"/>\n" % (
                            world_normal.x, -world_normal.z, world_normal.y))
                f.write("</normals>\n")

            if tex_files is not None:
                f.write("<textures count=\"%d\">\n" % len(tex_files))
                for n in tex_files:
                    f.write("<texture file=\"%s\"/>\n" % n)
                f.write("</textures>\n")

            f.write("<primitives count=\"%d\">\n" % poly_count)

            for i, p in enumerate(mesh.polygons):
                n_verts = len(p.vertices)
                if n_verts < 3 or n_verts > 4:
                    continue

                f.write("<poly ")

                if n_verts == 3:
                    f.write("v0=\"%d\" v1=\"%d\" v2=\"%d\" " % (
                        p.vertices[0], p.vertices[2], p.vertices[1]))
                else:
                    f.write("v0=\"%d\" v1=\"%d\" v2=\"%d\" v3=\"%d\" " % (
                        p.vertices[3], p.vertices[2], p.vertices[0], p.vertices[1]))

                if self.exp_writeNormals:
                    if p.use_smooth:
                        if n_verts == 3:
                            f.write("n0=\"%d\" n1=\"%d\" n2=\"%d\" " % (
                                p.vertices[0], p.vertices[2], p.vertices[1]))
                        else:
                            f.write("n0=\"%d\" n1=\"%d\" n2=\"%d\" n3=\"%d\" " % (
                                p.vertices[3], p.vertices[2], p.vertices[0], p.vertices[1]))
                        f.write("shading=\"S\" ")
                    else:
                        f.write("n0=\"%d\" " % (flatnorms_start + i))
                        f.write("shading=\"F\" ")

                is_textured = (tex_table is not None and tex_table[i] > 0)
                color_mul = 128.0 if is_textured else 255.0

                typecode = "F"
                if mesh_cols is None:
                    if is_textured:
                        # Textured face, no vertex colours: neutral (white)
                        # modulation so the texture shows unaltered.
                        f.write("r0=\"128\" g0=\"128\" b0=\"128\" ")
                    else:
                        # No texture and no vertex colours: fall back to the
                        # polygon's material base colour instead of flat grey.
                        bc = get_material_base_color(obj, p.material_index)
                        br = min(255, max(0, int(bc[0] * color_mul)))
                        bg = min(255, max(0, int(bc[1] * color_mul)))
                        bb = min(255, max(0, int(bc[2] * color_mul)))
                        f.write("r0=\"%d\" g0=\"%d\" b0=\"%d\" " % (br, bg, bb))
                else:
                    loop_start = p.loop_start
                    cols = []
                    for j in range(n_verts):
                        c = mesh_cols.data[loop_start + j].color
                        cols.append((int(c[0]*color_mul), int(c[1]*color_mul), int(c[2]*color_mul)))

                    reordered = [cols[0], cols[2], cols[1]] if n_verts == 3 else [cols[3], cols[2], cols[0], cols[1]]
                    flat = all(c == reordered[0] for c in reordered)

                    if flat:
                        f.write("r0=\"%d\" g0=\"%d\" b0=\"%d\" " % reordered[0])
                    else:
                        for j, c in enumerate(reordered):
                            f.write("r%d=\"%d\" g%d=\"%d\" b%d=\"%d\" " % (j, c[0], j, c[1], j, c[2]))
                        typecode = "G"

                if tex_table is not None and tex_table[i] > 0:
                    f.write("texture=\"%d\" " % (tex_table[i] - 1))
                    img = None
                    if p.material_index < len(obj.material_slots):
                        mat = obj.material_slots[p.material_index].material
                        if mat and mat.use_nodes:
                            for node in mat.node_tree.nodes:
                                if node.type == 'TEX_IMAGE' and node.image:
                                    img = node.image
                                    break
                    loop_start = p.loop_start
                    uv_order = [0, 2, 1] if n_verts == 3 else [3, 2, 0, 1]

                    # PS1-ready UVs. Coordinates are scaled to a fixed target
                    # texel size (exp_tex_size, the VRAM texture size) rather
                    # than the Blender image resolution, so 1 UV tile = that
                    # many texels regardless of source image size. V is flipped
                    # (PS1 V grows downward).
                    S = float(self.exp_tex_size)
                    us = []
                    vs = []
                    for uv_idx in uv_order:
                        uv = mesh_uvs[loop_start + uv_idx].uv
                        us.append(uv.x * S)
                        vs.append((1.0 - uv.y) * S)

                    # PS1 stores UVs as 8-bit (0-255) and wraps via a texture
                    # window. Shift each face's UVs down by whole tiles so the
                    # minimum lands in [0, S): this keeps values 8-bit-safe and
                    # preserves the sub-tile phase (multiples of S), so tiling
                    # stays continuous across adjacent faces.
                    ou = math.floor(min(us) / S) * S
                    ov = math.floor(min(vs) / S) * S
                    for j in range(len(uv_order)):
                        tu = round(us[j] - ou)
                        tv = round(vs[j] - ov)
                        if tu < 0: tu = 0
                        if tu > 255: tu = 255
                        if tv < 0: tv = 0
                        if tv > 255: tv = 255
                        f.write("tu%d=\"%d\" tv%d=\"%d\" " % (j, tu, j, tv))
                    typecode += "T"

                typecode += "%d" % n_verts
                f.write("type=\"%s\" />\n" % typecode)

            f.write("</primitives>\n")
            f.write("</model>\n")

        obj_eval.to_mesh_clear()
        self.report({'INFO'}, "Exported %d polygons to %s" % (poly_count, filepath))
        return {'FINISHED'}


def menu_func(self, context):
    self.layout.operator(ExportSMX.bl_idname, text="Scarlet 3D SMX v3 (.smx)")


def register():
    bpy.utils.register_class(ExportSMX)
    bpy.types.TOPBAR_MT_file_export.append(menu_func)


def unregister():
    bpy.utils.unregister_class(ExportSMX)
    bpy.types.TOPBAR_MT_file_export.remove(menu_func)


if __name__ == "__main__":
    register()
