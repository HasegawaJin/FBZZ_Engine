"""書き出したPlayerを独立したBlenderプロセスで読み戻し、制作時の数値と比較する。"""

import importlib.util
import json
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.kdtree import KDTree


def surface_error(reference, actual):
    tree = KDTree(len(reference))
    for index, point in enumerate(reference):
        tree.insert(Vector(point), index)
    tree.balance()
    return max(tree.find(Vector(point))[2] for point in actual)


def import_file(path, animate):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = 30
    bpy.ops.import_scene.fbx(filepath=str(path), use_anim=animate, anim_offset=0.0,
                             automatic_bone_orientation=False, force_connect_children=False)
    rigs = [o for o in bpy.context.scene.objects if o.type == 'ARMATURE']
    for rig in rigs:
        bpy.context.view_layer.objects.active = rig
        rig.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        # FBX importerが推測した接続は、独立した骨の位置チャンネルを無効化する。
        for bone in rig.data.edit_bones:
            bone.use_connect = False
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.update()
    return rigs


def main(directory, reference_directory, output):
    out = Path(directory)
    refs = Path(reference_directory)
    manifest = json.loads((out / 'ExportManifest.json').read_text(encoding='utf-8'))
    skeletons = json.loads((refs / 'Skeletons.json').read_text(encoding='utf-8'))
    geometry = np.load(refs / 'Geometry.npz')
    spec = importlib.util.spec_from_file_location('fbx_inspect', Path(__file__).with_name('fbzz_fbx_inspect.py'))
    inspect = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inspect)
    report = {'models': [], 'clips': [], 'runtime_tested': False, 'fbx_roundtrip_tested': True}

    def binary_check(path, bones, take):
        result = inspect.inspect(str(path))
        assert result['bone_count'] == bones, result
        assert result['takes'] == ([take] if take else []), result
        version, roots = inspect.parse(path)
        global_settings = next(n for n in roots if n.name == 'GlobalSettings')
        properties = next(n for n in global_settings.children if n.name == 'Properties70')
        settings = {n.props[0]: n.props[-1] for n in properties.children}
        assert settings['UpAxis'] == 1 and settings['UpAxisSign'] == 1
        assert abs(settings['UnitScaleFactor'] - 1) < 1e-6, settings
        result['unit_scale_factor'] = settings['UnitScaleFactor']
        objects = next(n for n in roots if n.name == 'Objects')
        for node in objects.children:
            if node.name == 'Model' and str(node.props[1]).split('\x00')[0] in ('PlayerRig', 'CompanionRig'):
                props = next(n for n in node.children if n.name == 'Properties70')
                transforms = {n.props[0]: n.props[4:] for n in props.children}
                assert np.max(np.abs(np.asarray(transforms['Lcl Scaling']) - 100)) < 1e-5, transforms
                assert np.max(np.abs(np.asarray(transforms['Lcl Rotation']) - [-90, 0, 0])) < 1e-4, transforms
                result['armature_axis_rotation_degrees'] = transforms['Lcl Rotation']
                result['armature_scale'] = transforms['Lcl Scaling']
        return result

    for entry in manifest['models']:
        relative = entry['fbx'].split('Models/Player/')[1]
        path = out / relative
        binary = binary_check(path, entry['bones'], None)
        rigs = import_file(path, False)
        meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
        assert len(rigs) == int(entry['bones'] > 0)
        assert {o.name for o in meshes} == set(entry['export_meshes'])
        errors = {}
        if rigs:
            rig = rigs[0]
            expected = skeletons[entry['asset']]
            assert set(rig.data.bones.keys()) == set(expected)
            for bone in rig.data.bones:
                assert (bone.parent.name if bone.parent else None) == expected[bone.name]['parent']
                assert (rig.matrix_world @ bone.head_local - Matrix(expected[bone.name]['rest']).translation).length < 1e-4
        else:
            assert bpy.data.objects.get('Attach_Grip')
            assert bpy.data.objects['Attach_Grip'].matrix_world.translation.length < 1e-6
        triangles = 0
        for mesh in meshes:
            evaluated = mesh.evaluated_get(bpy.context.evaluated_depsgraph_get())
            data = evaluated.to_mesh()
            points = np.asarray([evaluated.matrix_world @ vertex.co for vertex in data.vertices])
            evaluated.to_mesh_clear()
            error = surface_error(geometry[mesh.name], points)
            assert error < 1e-4 and np.isfinite(points).all(), (relative, mesh.name, error)
            errors[mesh.name] = error
            triangles += sum(len(p.vertices)-2 for p in mesh.data.polygons)
            assert len(mesh.data.uv_layers) == 1
            assert np.isfinite(np.asarray([uv.uv[:] for uv in mesh.data.uv_layers.active.data])).all()
            if rigs:
                weights = [[g.weight for g in v.groups if g.weight > 0] for v in mesh.data.vertices]
                assert min(map(len, weights)) > 0 and max(map(len, weights)) <= 4
                assert max(abs(sum(w)-1) for w in weights) < 1e-4
            for material in mesh.data.materials:
                shader = next(n for n in material.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
                assert shader.inputs['Base Color'].is_linked
                if 'Fingers' not in mesh.name:
                    assert shader.inputs['Normal'].is_linked
                for node in material.node_tree.nodes:
                    if node.type == 'TEX_IMAGE' and node.image:
                        assert Path(bpy.path.abspath(node.image.filepath)).is_file(), node.image.filepath
        assert triangles == entry['triangles'], (relative, triangles, entry['triangles'])
        report['models'].append({'file': relative, 'surface_errors_m': errors, 'triangles': triangles, 'binary': binary})
        print('MODEL PASS ' + relative, flush=True)

    for entry in manifest['clips']:
        label = entry['take']
        path = out / 'Animations' / (label + '.fbx')
        binary = binary_check(path, 54, label)
        rigs = import_file(path, True)
        assert len(rigs) == 1 and not [o for o in bpy.context.scene.objects if o.type == 'MESH']
        rig = rigs[0]
        assert rig.name == 'PlayerRig' and len(bpy.data.actions) == 1
        reference = np.load(refs / (label + '.npz'))
        frames, names, matrices = reference['frames'], reference['names'].tolist(), reference['matrices']
        assert set(rig.data.bones.keys()) == set(names)
        action = rig.animation_data.action
        assert np.max(np.abs(np.asarray(action.frame_range) - entry['source_frames'])) < 1e-3, (label, list(action.frame_range))
        rests = {name: Matrix(skeletons['Player'][name]['rest']) for name in names}
        imported_rests = {name: rig.matrix_world @ rig.data.bones[name].matrix_local for name in names}
        maximum_head, maximum_deform, maximum_socket = 0., 0., 0.
        for index, frame in enumerate(frames):
            bpy.context.scene.frame_set(int(frame), subframe=float(frame % 1))
            bpy.context.view_layer.update()
            for bindex, name in enumerate(names):
                world = rig.matrix_world @ rig.pose.bones[name].matrix
                expected = Matrix(matrices[index, bindex].tolist())
                maximum_head = max(maximum_head, (world.translation-expected.translation).length)
                # インポーターが末端骨の軸を付け直しても、スキン変形の行列は同じであること。
                actual_deform = world @ imported_rests[name].inverted()
                expected_deform = expected @ rests[name].inverted()
                maximum_deform = max(maximum_deform, float(np.max(np.abs(np.asarray(actual_deform) - np.asarray(expected_deform)))))
                if name == 'Socket_Weapon_R':
                    socket = actual_deform @ rests[name]
                    maximum_socket = max(maximum_socket, float(np.max(np.abs(np.asarray(socket) - matrices[index, bindex]))))
        assert maximum_head < .0001 and maximum_deform < .0002 and maximum_socket < .0001, (label, maximum_head, maximum_deform, maximum_socket)
        report['clips'].append({'take': label, 'sampled_frames': len(frames), 'maximum_bone_position_error_m': maximum_head,
                                'maximum_deform_matrix_error': maximum_deform, 'maximum_socket_matrix_error': maximum_socket,
                                'duration_seconds': (action.frame_range[1]-action.frame_range[0])/30, 'binary': binary})
        print('CLIP PASS ' + label + ' ' + str(maximum_head), flush=True)
    assert len(report['models']) == 9 and len(report['clips']) == 22
    Path(output).write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('ALL 31 FBX FILES VERIFIED', flush=True)


if __name__ == '__main__':
    args = sys.argv[sys.argv.index('--')+1:]
    main(*args)
