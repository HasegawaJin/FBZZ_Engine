"""承認済みPlayerを一時コピーからFBXへ書き出す。配置と旧資産の退避は別工程。"""

import importlib.util
import json
import math
import shutil
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('player_export_preflight', Path(__file__).with_name('prepare_minibot_c_export.py'))
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)


def rows(matrix):
    return [list(row) for row in matrix]


class ExportSession:
    def __init__(self, directory):
        self.out = Path(directory).resolve()
        assert self.out.is_relative_to(ROOT) and not self.out.exists(), self.out
        self.manifest = prep.collect()
        assert not self.manifest['preflight']['errors']
        self.scene = bpy.context.scene
        self.control = bpy.data.objects[prep.CONTROL]
        self.source = bpy.data.objects[prep.RIG]
        self.sword = bpy.data.objects['Sword_Control']
        self.before = {a.name: prep.fingerprint(a) for a in bpy.data.actions if a.name.startswith('MB_C_')}
        self.saved = {
            'action': self.control.animation_data.action,
            'slot': self.control.animation_data.action_slot,
            'frame': self.scene.frame_current, 'subframe': self.scene.frame_subframe,
            'range': (self.scene.frame_start, self.scene.frame_end), 'name': self.scene.name,
            'active': bpy.context.view_layer.objects.active,
            'selected': list(bpy.context.selected_objects),
            'mode': bpy.context.object.mode if bpy.context.object else 'OBJECT',
            'playing': bool(bpy.context.screen and bpy.context.screen.is_animation_playing),
        }
        self.created = []
        self.mesh_data = []
        self.armature_data = []
        self.materials = {}
        self.images = {}
        self.baked = None
        self.rig = None
        if self.saved['playing']:
            bpy.ops.screen.animation_cancel(restore_frame=False)
        if self.saved['mode'] != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        self.out.mkdir(parents=True)
        self.refs = self.out.parent / 'ValidationReferences'
        self.refs.mkdir()
        self.collection = bpy.data.collections.new('TEMP_PlayerExport')
        self.scene.collection.children.link(self.collection)
        attachment = Matrix(json.loads(self.sword['grip_calibration'])['attachment'])
        self.socket_rest = self.source.data.bones['RightHand'].matrix_local @ attachment
        self.manifest['socket_policy']['export_rest_matrix'] = rows(self.socket_rest)
        self.report = {'models': [], 'clips': [], 'source_unchanged': False, 'runtime_tested': False}
        self._textures()
        self.manifest['fbx_settings']['path_mode'] = 'RELATIVE'
        self._save_report()

    def _textures(self):
        target = self.out / 'Textures'
        target.mkdir()
        for tex in self.manifest['textures']:
            src = Path(tex['source_path'])
            assert src.is_file(), src
            dest = target / tex['export_filename']
            assert not dest.exists(), dest
            shutil.copy2(src, dest)
            image = bpy.data.images[tex['image']].copy()
            if image.packed_file:
                image.unpack(method='REMOVE')
            image.filepath = str(dest)
            image.filepath_raw = str(dest)
            self.images[tex['image']] = image
            tex['export_path'] = 'Textures/' + dest.name
        for mat in self.manifest['materials']:
            clone = bpy.data.materials[mat['name']].copy()
            clone.name = 'PlayerExport_' + mat['name']
            for node in clone.node_tree.nodes:
                if node.type == 'TEX_IMAGE' and node.image and node.image.name in self.images:
                    node.image = self.images[node.image.name]
            self.materials[mat['name']] = clone

    def _armature(self, source, name, socket=False):
        obj = source.copy()
        obj.data = source.data.copy()
        obj.name = name
        obj.animation_data_clear()
        obj.data.animation_data_clear()
        obj.parent = None
        obj.matrix_world = Matrix.Identity(4)
        self.collection.objects.link(obj)
        self.created.append(obj)
        self.armature_data.append(obj.data)
        obj.hide_viewport = False
        obj.hide_set(False)
        for constraint in list(obj.constraints):
            obj.constraints.remove(constraint)
        for bone in obj.pose.bones:
            for constraint in list(bone.constraints):
                bone.constraints.remove(constraint)
            bone.custom_shape = None
            bone.rotation_mode = 'QUATERNION'
            bone.matrix_basis = Matrix.Identity(4)
        if socket:
            self._select([obj], obj)
            bpy.ops.object.mode_set(mode='EDIT')
            bone = obj.data.edit_bones['Socket_Weapon_R']
            bone.use_connect = False
            bone.matrix = self.socket_rest
            bone.length = .1
            bpy.ops.object.mode_set(mode='OBJECT')
        return obj

    def _mesh(self, source, parent):
        obj = source.copy()
        obj.data = source.data.copy()
        obj.name = 'Export_' + source.name
        obj.animation_data_clear()
        obj.parent = parent
        obj.parent_type = 'OBJECT'
        obj.matrix_parent_inverse = Matrix.Identity(4)
        obj.matrix_basis = Matrix.Identity(4)
        self.collection.objects.link(obj)
        self.created.append(obj)
        self.mesh_data.append(obj.data)
        obj.hide_viewport = False
        obj.hide_set(False)
        for constraint in list(obj.constraints):
            obj.constraints.remove(constraint)
        for modifier in obj.modifiers:
            if modifier.type == 'ARMATURE':
                modifier.object = parent
        for index, material in enumerate(obj.data.materials):
            if material:
                obj.data.materials[index] = self.materials[material.name]
        return obj

    @staticmethod
    def _select(objects, active):
        bpy.ops.object.select_all(action='DESELECT')
        for obj in objects:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = active

    def _export(self, relative, objects, active, animate=False):
        path = self.out / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        self._select(objects, active)
        options = dict(prep.FBX_SETTINGS, path_mode='RELATIVE')
        options.update(prep.ANIMATION_SETTINGS if animate else {'bake_anim': False})
        bpy.ops.export_scene.fbx(filepath=str(path), object_types={'ARMATURE', 'MESH', 'EMPTY'}, **options)
        assert path.is_file() and path.stat().st_size > 1000
        return path

    def models(self):
        self.rig = self._armature(self.source, 'PlayerRig', socket=True)
        self.rig.data.pose_position = 'REST'
        companion = self._armature(bpy.data.objects['Companion_Rig'], 'CompanionRig')
        companion.data.pose_position = 'REST'
        grip = bpy.data.objects.new('Attach_Grip', None)
        self.collection.objects.link(grip)
        self.created.append(grip)
        geometry = {}
        skeletons = {}
        for label, rig in [('Player', self.rig), ('Companion', companion)]:
            skeletons[label] = {b.name: {'parent': b.parent.name if b.parent else None,
                                       'rest': rows(b.matrix_local)} for b in rig.data.bones}
        for entry in self.manifest['models']:
            label, lod = entry['asset'], entry['lod']
            rig = self.rig if label == 'Player' else companion if label == 'Companion' else None
            meshes = [self._mesh(bpy.data.objects[name], rig) for name in entry['meshes']]
            for obj, source_name in zip(meshes, entry['meshes']):
                geometry[obj.name] = np.asarray([v.co[:] for v in obj.data.vertices], dtype=np.float32)
            filename = label + (f'_LOD{lod}' if lod else '') + '.fbx'
            relative = filename if label == 'Player' else label + '/' + filename
            objects = ([rig] if rig else [grip]) + meshes
            self._export(relative, objects, rig or grip)
            entry['fbx'] = 'GreenWare/Assets/Models/Player/' + relative
            entry['export_meshes'] = [m.name for m in meshes]
            self.report['models'].append({'file': relative, 'triangles': entry['triangles'], 'bones': entry['bones']})
            for obj in meshes:
                obj.hide_set(True)
        companion.hide_set(True)
        grip.hide_set(True)
        np.savez_compressed(self.refs / 'Geometry.npz', **geometry)
        (self.refs / 'Skeletons.json').write_text(json.dumps(skeletons), encoding='utf-8')
        self.rig.data.pose_position = 'POSE'
        self._save_report()
        return self.report['models']

    def clip(self, label):
        entry = next(c for c in self.manifest['clips'] if c['take'] == label)
        assert not any(c['take'] == label for c in self.report['clips'])
        action = bpy.data.actions[entry['action']]
        self.control.animation_data.action = action
        self.control.animation_data.action_slot = action.slots[0]
        start, end = entry['source_frames']
        frames = np.arange(start, end + .125, .25, dtype=np.float64)
        bones = list(self.rig.pose.bones)
        names = [b.name for b in bones]
        values = np.empty((len(frames), len(bones), 10), dtype=np.float32)
        reference = np.empty((len(frames), len(bones), 4, 4), dtype=np.float32)
        previous = {}
        for index, frame in enumerate(frames):
            self.scene.frame_set(int(frame), subframe=float(frame % 1))
            self.control.update_tag()
            self.source.update_tag()
            bpy.context.view_layer.update()
            matrices = {b.name: b.matrix.copy() for b in self.source.pose.bones}
            matrices['Socket_Weapon_R'] = self.source.matrix_world.inverted() @ self.sword.matrix_world
            for bindex, bone in enumerate(bones):
                matrix = matrices[bone.name]
                reference[index, bindex] = matrix
                kwargs = {'parent_matrix': matrices[bone.parent.name],
                          'parent_matrix_local': bone.parent.bone.matrix_local} if bone.parent else {}
                basis = bone.bone.convert_local_to_pose(matrix, bone.bone.matrix_local, invert=True, **kwargs)
                loc, quat, scale = basis.decompose()
                if bone.name in previous and quat.dot(previous[bone.name]) < 0:
                    quat.negate()
                previous[bone.name] = quat.copy()
                values[index, bindex] = (*loc, *quat, *scale)
        assert np.isfinite(values).all() and np.isfinite(reference).all()
        np.savez_compressed(self.refs / (label + '.npz'), frames=frames, names=names, matrices=reference)
        self.rig.animation_data_clear()
        for bone in bones:
            for prop in ('location', 'rotation_quaternion', 'scale'):
                bone.keyframe_insert(prop, frame=start, group=bone.name)
        self.baked = self.rig.animation_data.action
        self.baked.name = 'TEMP_EXPORT_' + label
        channels = {(f.data_path, f.array_index): f for f in prep.curves(self.baked)}
        for bindex, bone in enumerate(bones):
            offset = 0
            for prop, size in [('location', 3), ('rotation_quaternion', 4), ('scale', 3)]:
                for component in range(size):
                    curve = channels[(bone.path_from_id(prop), component)]
                    curve.keyframe_points.add(len(frames))
                    rest_value = 1.0 if prop == 'scale' or (prop == 'rotation_quaternion' and component == 0) else 0.0
                    times = np.concatenate(([start-1], frames))
                    channel = np.concatenate(([rest_value], values[:, bindex, offset + component]))
                    curve.keyframe_points.foreach_set('co', np.column_stack((times, channel)).ravel())
                    for point in curve.keyframe_points:
                        point.interpolation = 'LINEAR'
                    curve.update()
                offset += size
        maximum = 0.0
        for index in sorted({0, len(frames)//4, len(frames)//2, 3*len(frames)//4, len(frames)-1}):
            frame = frames[index]
            self.scene.frame_set(int(frame), subframe=float(frame % 1))
            bpy.context.view_layer.update()
            for bindex, bone in enumerate(bones):
                maximum = max(maximum, float(np.max(np.abs(np.asarray(bone.matrix) - reference[index, bindex]))))
        assert maximum < .0001, (label, maximum)
        self.scene.name = label
        self.scene.frame_start, self.scene.frame_end = start, end
        # スキンなしFBXの既定ノード変換にも、全クリップ共通のバインド姿勢を格納する。
        # この補助キーはベイク範囲外であり、モーションの尺には含めない。
        self.scene.frame_set(start-1)
        relative = 'Animations/' + label + '.fbx'
        self._export(relative, [self.rig], self.rig, animate=True)
        entry['fbx'] = 'GreenWare/Assets/Models/Player/' + relative
        self.rig.animation_data_clear()
        bpy.data.actions.remove(self.baked)
        self.baked = None
        record = {'take': label, 'file': relative, 'samples': len(frames), 'duration_seconds': (end-start)/30,
                  'loop': entry['loop'], 'maximum_bake_matrix_error': maximum}
        self.report['clips'].append(record)
        self._save_report()
        return record

    def _save_report(self):
        (self.out / 'ExportManifest.json').write_text(json.dumps(self.manifest, ensure_ascii=False, indent=2), encoding='utf-8')
        (self.out.parent / 'BakeReport.json').write_text(json.dumps(self.report, indent=2), encoding='utf-8')

    def close(self):
        if self.rig:
            self.rig.animation_data_clear()
        if self.baked:
            bpy.data.actions.remove(self.baked)
        for obj in reversed(self.created):
            bpy.data.objects.remove(obj, do_unlink=True)
        for data in self.mesh_data:
            if data.users == 0:
                bpy.data.meshes.remove(data)
        for data in self.armature_data:
            if data.users == 0:
                bpy.data.armatures.remove(data)
        for material in self.materials.values():
            if material.users == 0:
                bpy.data.materials.remove(material)
        for image in self.images.values():
            if image.users == 0:
                bpy.data.images.remove(image)
        bpy.data.collections.remove(self.collection)
        self.control.animation_data.action = self.saved['action']
        self.control.animation_data.action_slot = self.saved['slot']
        self.scene.name = self.saved['name']
        self.scene.frame_start, self.scene.frame_end = self.saved['range']
        self.scene.frame_set(self.saved['frame'], subframe=self.saved['subframe'])
        bpy.context.view_layer.update()
        self._select([o for o in self.saved['selected'] if o.visible_get()], self.saved['active'])
        if self.saved['active'] and self.saved['mode'] != 'OBJECT':
            bpy.ops.object.mode_set(mode=self.saved['mode'])
        self.report['source_unchanged'] = self.before == {a.name: prep.fingerprint(a) for a in bpy.data.actions if a.name.startswith('MB_C_')}
        assert self.report['source_unchanged'], 'Source Action changed during export'
        complete = len(self.report['models']) == 9 and len(self.report['clips']) == 22
        self.manifest['status'] = 'exported_pending_validation' if complete else 'export_incomplete'
        self._save_report()
        if self.saved['playing'] and bpy.context.screen and not bpy.context.screen.is_animation_playing:
            bpy.ops.screen.animation_play()
        return self.report
