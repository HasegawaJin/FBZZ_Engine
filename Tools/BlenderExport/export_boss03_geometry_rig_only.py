"""Boss03 の既存 FBX から、ジオメトリとリグだけを羽単位で出力する。"""

from pathlib import Path
import json
import bpy


ROOT = Path(__file__).resolve().parents[2]
PATH = ROOT / "GreenWare/Assets/Models/Boss_03/Boss03.fbx"

GROUPS = [
    "Body",
    "Core",
    "Wing_L_Upper",
    "Wing_L_Middle",
    "Wing_L_Lower",
    "Wing_R_Upper",
    "Wing_R_Middle",
    "Wing_R_Lower",
]


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for collection in list(bpy.data.collections):
        if collection.name != "Collection":
            bpy.data.collections.remove(collection)


def separate_group(source, group_name):
    bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    source.select_set(True)
    bpy.context.view_layer.objects.active = source
    source.vertex_groups.active_index = source.vertex_groups[group_name].index
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="DESELECT")
    bpy.ops.object.vertex_group_select()
    bpy.ops.mesh.separate(type="SELECTED")
    bpy.ops.object.mode_set(mode="OBJECT")

    candidates = [obj for obj in bpy.context.selected_objects if obj.type == "MESH"]
    separated = [obj for obj in candidates if obj != source]
    if len(separated) != 1:
        raise RuntimeError(f"{group_name}: separate result is ambiguous: {[o.name for o in separated]}")
    result = separated[0]
    result.name = f"Boss03_{group_name}"
    result.data.name = result.name
    return result


def main():
    clear_scene()
    bpy.ops.import_scene.fbx(filepath=str(PATH))
    source = bpy.data.objects.get("Boss03_Mesh")
    rig = bpy.data.objects.get("Boss03Rig")
    if source is None or rig is None:
        raise RuntimeError("Boss03_Mesh or Boss03Rig was not found in the source FBX")

    meshes = []
    for group_name in GROUPS[:-1]:
        meshes.append(separate_group(source, group_name))

    source.name = "Boss03_Wing_R_Lower"
    source.data.name = source.name
    meshes.append(source)

    for obj in meshes:
        obj.parent = rig
        for modifier in obj.modifiers:
            if modifier.type == "ARMATURE":
                modifier.object = rig

    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    for obj in meshes:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = rig

    bpy.ops.export_scene.fbx(
        filepath=str(PATH),
        use_selection=True,
        object_types={"MESH", "ARMATURE"},
        global_scale=1.0,
        apply_unit_scale=True,
        apply_scale_options="FBX_SCALE_NONE",
        axis_forward="-Z",
        axis_up="Y",
        use_space_transform=True,
        bake_space_transform=False,
        add_leaf_bones=False,
        primary_bone_axis="Y",
        secondary_bone_axis="X",
        armature_nodetype="NULL",
        use_armature_deform_only=False,
        use_mesh_modifiers=True,
        mesh_smooth_type="OFF",
        use_tspace=False,
        path_mode="RELATIVE",
        bake_anim=False,
    )

    validation = {
        "path": str(PATH),
        "animation_exported": False,
        "armature": rig.name,
        "meshes": [{"name": obj.name, "vertices": len(obj.data.vertices), "polygons": len(obj.data.polygons)} for obj in meshes],
        "bones": [bone.name for bone in rig.data.bones],
    }
    (PATH.parent / "Boss03_ExportValidation.json").write_text(
        json.dumps(validation, indent=2), encoding="utf-8"
    )
    print("BOSS03_EXPORT " + json.dumps(validation, ensure_ascii=False), flush=True)


main()
