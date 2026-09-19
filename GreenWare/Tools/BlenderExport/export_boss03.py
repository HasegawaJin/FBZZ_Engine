"""Boss03 の制作データを変更せず、共通バインド姿勢でモデルとクリップを出力する。"""

import json
from pathlib import Path

import bpy
from mathutils import Matrix


ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'GreenWare/Assets/Models/Boss_03'


class ExportSession:
    def __init__(self, replace=False):
        self.scene = bpy.context.scene
        self.saved = (self.scene.name, self.scene.frame_current, self.scene.frame_start,
                      self.scene.frame_end, list(bpy.context.selected_objects),
                      bpy.context.view_layer.objects.active)
        self.playing = bool(bpy.context.screen and bpy.context.screen.is_animation_playing)
        if self.playing:
            bpy.ops.screen.animation_cancel(restore_frame=False)
        if bpy.context.object and bpy.context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        assert replace or not OUT.exists(), 'Export destination already exists'
        OUT.mkdir(parents=True, exist_ok=replace)
        (OUT / 'Animations').mkdir(exist_ok=replace)
        self.collection = bpy.data.collections.new('TEMP_Boss03_Export')
        self.scene.collection.children.link(self.collection)
        source = bpy.data.objects['Boss03_Armature']
        self.rig = source.copy()
        self.rig.data = source.data.copy()
        self.rig.animation_data_clear()
        self.rig.name = 'Boss03Rig'
        self.collection.objects.link(self.rig)
        for bone in self.rig.pose.bones:
            bone.matrix_basis = Matrix.Identity(4)
        self.meshes = []
        for source_mesh in bpy.data.collections['EXPORT_Boss03'].all_objects:
            if source_mesh.type != 'MESH':
                continue
            mesh = source_mesh.copy()
            mesh.data = source_mesh.data.copy()
            world = source_mesh.matrix_world.copy()
            mesh.parent = self.rig
            mesh.matrix_world = world
            self.collection.objects.link(mesh)
            mesh.hide_set(False)
            for modifier in mesh.modifiers:
                if modifier.type == 'ARMATURE':
                    modifier.object = self.rig
            self.meshes.append(mesh)
        self.rig.hide_set(False)
        self.report = {'source': bpy.data.filepath, 'fps': self.scene.render.fps,
                       'bones': [b.name for b in self.rig.data.bones], 'clips': [],
                       'source_meshes': len(self.meshes), 'materials': []}
        materials = {m.name: m for o in self.meshes for m in o.data.materials if m}
        for name, material in materials.items():
            node = next(n for n in material.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
            self.report['materials'].append({'name': name,
                'base_color': list(node.inputs['Base Color'].default_value),
                'metallic': node.inputs['Metallic'].default_value,
                'roughness': node.inputs['Roughness'].default_value,
                'emission': list(node.inputs['Emission Color'].default_value),
                'emission_strength': node.inputs['Emission Strength'].default_value})
        self.select(self.meshes)
        bpy.ops.object.join()
        self.meshes = [bpy.context.object]
        self.meshes[0].name = 'Boss03_Mesh'
        self.meshes[0].data.name = 'Boss03_Mesh'
        self.export(OUT / 'Boss03.fbx', [self.rig] + self.meshes, False)
        self.meshes[0].hide_set(True)
        self.save_report()

    def select(self, objects):
        bpy.ops.object.select_all(action='DESELECT')
        for obj in objects:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = objects[0]

    def export(self, path, objects, animate):
        self.select(objects)
        bpy.ops.export_scene.fbx(filepath=str(path), use_selection=True,
            object_types={'MESH', 'ARMATURE'}, global_scale=1.0,
            apply_unit_scale=True, apply_scale_options='FBX_SCALE_NONE',
            axis_forward='-Z', axis_up='Y', use_space_transform=True,
            bake_space_transform=False, add_leaf_bones=False,
            primary_bone_axis='Y', secondary_bone_axis='X',
            armature_nodetype='NULL', use_armature_deform_only=False,
            use_mesh_modifiers=True, mesh_smooth_type='OFF', use_tspace=False,
            path_mode='RELATIVE', bake_anim=animate, bake_anim_use_all_bones=True,
            bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False,
            bake_anim_force_startend_keying=True, bake_anim_step=0.5,
            bake_anim_simplify_factor=0.0)
        assert path.stat().st_size > 1000

    def clip(self, name):
        source = bpy.data.actions['Boss03_' + name]
        action = source.copy()
        action.name = 'TEMP_Boss03_' + name
        self.rig.animation_data_create()
        self.rig.animation_data.action = action
        if action.slots:
            self.rig.animation_data.action_slot = action.slots[0]
        start, end = map(int, source.frame_range)
        for bone in self.rig.pose.bones:
            bone.matrix_basis = Matrix.Identity(4)
            for prop in ('location', 'rotation_quaternion', 'scale'):
                bone.keyframe_insert(prop, frame=0, group=bone.name)
        self.scene.name = name
        self.scene.frame_start, self.scene.frame_end = start, end
        self.scene.frame_set(0)
        self.export(OUT / 'Animations' / (name + '.fbx'), [self.rig], True)
        self.rig.animation_data_clear()
        bpy.data.actions.remove(action)
        self.report['clips'].append({'name': name, 'frames': [start, end],
            'seconds': (end - start) / self.scene.render.fps})
        self.save_report()
        return name

    def save_report(self):
        (OUT / 'ExportManifest.json').write_text(json.dumps(self.report, indent=2), encoding='utf-8')

    def close(self):
        for obj in list(self.collection.objects):
            data, kind = obj.data, obj.type
            bpy.data.objects.remove(obj, do_unlink=True)
            if data.users == 0:
                (bpy.data.meshes if kind == 'MESH' else bpy.data.armatures).remove(data)
        bpy.data.collections.remove(self.collection)
        name, frame, start, end, selection, active = self.saved
        self.scene.name = name
        self.scene.frame_start, self.scene.frame_end = start, end
        self.scene.frame_set(frame)
        self.select(selection) if selection else bpy.ops.object.select_all(action='DESELECT')
        bpy.context.view_layer.objects.active = active
        if self.playing:
            bpy.ops.screen.animation_play()
        self.report['source_restored'] = True
        self.save_report()
