"""完成版の出力対象・座標契約・検証結果を整理する。FBXは書き出さない。"""

import csv
import hashlib
import json
import math
from pathlib import Path

import bpy
from bpy_extras.io_utils import axis_conversion
from mathutils import Matrix, Vector


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'GreenWare/Assets/_src/MiniBotC/MiniBotC_PlayerMotions.blend'
OUT = ROOT / 'Docs/Art/MiniBotC/ExportPreparation'
CONTROL = 'MiniBotC_ControlRig'
RIG = 'MiniBotC_Humanoid'
CLIPS = (
    ('Idle', '待機', 'Locomotion'), ('SwordWalk', '剣歩き（両手）', 'Locomotion'),
    ('Run_F', '走り', 'Locomotion'), ('RunStop', '走り停止', 'Locomotion'),
    ('Dodge', '回避ローリング', 'Locomotion'), ('JumpStart', 'ジャンプ踏切', 'Air'),
    ('FallLoop', '空中・落下', 'Air'), ('Land', '着地', 'Air'),
    ('Slash01', '斬撃', 'Attack'), ('HighSpinAttack', '回転斬り', 'Attack'),
    ('JumpAttack', 'ジャンプ攻撃', 'Attack'), ('SlideAttack', 'スライド斬り', 'Attack'),
    ('UnderSlash', '切り上げ', 'Attack'), ('UnderSlashandUpperSlash', '連続斬り', 'Attack'),
    ('ToBlocking', 'ガード開始', 'Defense'), ('Blocking', 'ガード維持', 'Defense'),
    ('GuardHit', 'ガードヒット', 'Defense'), ('BlockingToIdle', 'ガード解除', 'Defense'),
    ('Hit', '被弾', 'Reaction'), ('Death', '死亡', 'Reaction'),
    ('Victory', '勝利', 'Result'), ('DefeatIdle', '敗北', 'Result'),
)

# 現行インポーターは scale 100 の除去時に静止表示用の bindBakeRotation も設定する。
FBX_SETTINGS = {
    'use_selection': True, 'use_visible': False, 'global_scale': 1.0,
    'apply_unit_scale': True, 'apply_scale_options': 'FBX_SCALE_NONE',
    'axis_forward': '-Z', 'axis_up': 'Y', 'use_space_transform': True,
    'bake_space_transform': False, 'add_leaf_bones': False,
    'primary_bone_axis': 'Y', 'secondary_bone_axis': 'X',
    'armature_nodetype': 'NULL', 'use_armature_deform_only': False,
    'use_mesh_modifiers': True, 'mesh_smooth_type': 'OFF', 'use_tspace': True,
    'path_mode': 'COPY', 'embed_textures': False,
}
ANIMATION_SETTINGS = {
    'bake_anim': True, 'bake_anim_use_all_bones': True,
    'bake_anim_use_all_actions': False, 'bake_anim_use_nla_strips': False,
    'bake_anim_force_startend_keying': True, 'bake_anim_step': .25,
    'bake_anim_simplify_factor': 0.0,
}


def curves(action):
    for slot in action.slots:
        for layer in action.layers:
            for strip in layer.strips:
                bag = strip.channelbag(slot)
                if bag:
                    yield from bag.fcurves


def fingerprint(action):
    values = sorted((c.data_path, c.array_index,
                     [(tuple(p.co), p.interpolation, tuple(p.handle_left), tuple(p.handle_right))
                      for p in c.keyframe_points]) for c in curves(action))
    return hashlib.sha256(json.dumps(values).encode()).hexdigest()


def matrix_rows(matrix):
    return [[float(v) for v in row] for row in matrix]


def collect():
    scene = bpy.context.scene
    control = bpy.data.objects[CONTROL]
    rig = bpy.data.objects[RIG]
    assert Path(bpy.data.filepath).resolve() == SOURCE.resolve(), 'Open the approved PlayerMotions source'
    assert scene.unit_settings.scale_length == 1 and scene.render.fps / scene.render.fps_base == 30
    assert len(rig.data.bones) == 54
    assert {a.name for a in bpy.data.actions if a.name.startswith('MB_C_')} == {'MB_C_'+c[0] for c in CLIPS} | {'MB_C_Stance'}
    models, material_names, errors = [], set(), []
    for asset in ('Player', 'Sword', 'Companion'):
        for lod in range(3):
            meshes = [bpy.data.objects[f'{asset}_LOD{lod}']]
            if asset == 'Player':
                meshes.append(bpy.data.objects[f'Player_Fingers_LOD{lod}'])
            skeleton = RIG if asset == 'Player' else 'Companion_Rig' if asset == 'Companion' else None
            record = {
                'asset': asset, 'lod': lod, 'meshes': [o.name for o in meshes],
                'source_rig': skeleton, 'bones': len(bpy.data.objects[skeleton].data.bones) if skeleton else 0,
                'fbx': f'GreenWare/Assets/Models/MiniBotC/{asset}/{asset}_LOD{lod}.fbx',
                'triangles': sum(len(p.vertices)-2 for o in meshes for p in o.data.polygons),
                'vertices': sum(len(o.data.vertices) for o in meshes),
                'bind_pose_only': True, 'include_animation': False,
                'mesh_checks': [],
            }
            for obj in meshes:
                material_names.update(m.name for m in obj.data.materials if m)
                check = {'name':obj.name, 'uv_maps':[u.name for u in obj.data.uv_layers]}
                if not obj.data.uv_layers:
                    errors.append(obj.name+': missing UV')
                if skeleton:
                    weights = [[g.weight for g in v.groups if g.weight > 1e-8] for v in obj.data.vertices]
                    check['maximum_influences'] = max(map(len, weights), default=0)
                    check['maximum_weight_sum_error'] = max((abs(sum(w)-1) for w in weights), default=1)
                    if check['maximum_influences'] > 4 or check['maximum_weight_sum_error'] > 1e-4:
                        errors.append(obj.name+': skin weights')
                    groups = {g.index:g.name for g in obj.vertex_groups}
                    unknown = sorted({groups[g.group] for v in obj.data.vertices for g in v.groups
                                      if g.weight > 1e-8 and groups[g.group] not in bpy.data.objects[skeleton].data.bones})
                    if unknown:
                        errors.append(obj.name+': unknown skin bones '+str(unknown))
                    socket_weight = sum(any(groups[g.group] == 'Socket_Weapon_R' and g.weight > 0 for g in v.groups)
                                        for v in obj.data.vertices)
                    check['weighted_socket_vertices'] = socket_weight
                    if socket_weight:
                        errors.append(obj.name+': weapon socket deforms player')
                record['mesh_checks'].append(check)
            models.append(record)
    clips = []
    for label, title, category in CLIPS:
        action = bpy.data.actions['MB_C_'+label]
        assert len(action.slots) == 1 and action.slots[0].identifier == 'OB'+CONTROL
        start, end = int(action['start_frame']), int(action['end_frame'])
        loop = bool(action['loop'])
        clips.append({
            'action':action.name, 'label':title, 'category':category, 'take':label,
            'fbx':f'GreenWare/Assets/Models/MiniBotC/Player/Animations/{label}.fbx',
            'source_frames':[start,end], 'preview_frames':[start,end-1 if loop else end],
            'duration_seconds':(end-start)/30, 'loop':loop,
            'import_clip':{'name':label,'loop':loop,'start_frame':0.0,'end_frame':-1.0,'output_name':label},
            'weapon_released':bool(action.get('weapon_released',False)),
            'markers':{m.name:m.frame for m in action.pose_markers},
            'fcurve_sha256':fingerprint(action),
        })
    materials, textures = [], {}
    for name in sorted(material_names):
        material = bpy.data.materials[name]
        nodes = material.node_tree.nodes
        bsdf = next(n for n in nodes if n.type == 'BSDF_PRINCIPLED')
        rec = {'name':name, 'scalars':{}, 'textures':[]}
        for input_name in ('Metallic','Roughness','Emission Color','Emission Strength','Alpha'):
            value = bsdf.inputs[input_name].default_value
            rec['scalars'][input_name] = list(value) if hasattr(value,'__len__') else float(value)
        for node in nodes:
            if node.type != 'TEX_IMAGE' or not node.image:
                continue
            image = node.image
            uses = [(l.to_node.type,l.to_socket.name) for l in material.node_tree.links if l.from_node == node]
            if not uses:
                continue
            normal = any(kind == 'NORMAL_MAP' for kind,socket in uses)
            image_path = Path(bpy.path.abspath(image.filepath))
            rec['textures'].append({'image':image.name,'role':'Normal' if normal else 'BaseColor'})
            textures[image.name] = {
                'image':image.name,'source_path':str(image_path),'source_exists':image_path.is_file(),
                'packed':bool(image.packed_file),'size':list(image.size),
                'colorspace':image.colorspace_settings.name,'role':'Normal' if normal else 'BaseColor',
                'export_filename':image_path.name,
                'fbzz_normal_convention':'OpenGL' if normal else None,
            }
            if not image.packed_file and not image_path.is_file():
                errors.append('Missing texture '+image.name)
        materials.append(rec)
    conversion = axis_conversion(to_forward='-Z',to_up='Y').to_4x4()
    combined = Matrix.Diagonal((1,1,-1,1)) @ conversion
    return {
        'schema_version':1,'status':'prepared_not_exported','source_blend':str(SOURCE),
        'authoring_fps':30,'source_unit_meters':1.0,'fbx_settings':FBX_SETTINGS,
        'animation_settings':ANIMATION_SETTINGS,
        'coordinate_contract':{'blender_up':'+Z','blender_player_forward':'-Y',
            'engine_up':'+Y','engine_game_forward':'+Z','imported_visual_forward':'-Z',
            'visual_child_yaw_degrees':180,'source_point_to_engine_model':matrix_rows(combined),
            'source_point_to_engine_game':matrix_rows(Matrix.Rotation(math.pi,4,'Y')@combined)},
        'models':models,'clips':clips,'materials':materials,'textures':list(textures.values()),
        'export_exclusions':{'actions':[a.name for a in bpy.data.actions if a.name not in {c['action'] for c in clips}],
                             'objects':[o.name for o in bpy.data.objects if o.name not in {n for m in models for n in m['meshes']} | {RIG,'Companion_Rig'}]},
        'socket_policy':{'bone':'Socket_Weapon_R','evaluated_source':'Sword_Control',
                         'operation':'Bake evaluated weapon transform into export-copy socket; calibrate identical socket rest matrix in every model/clip export.',
                         'source_skeleton_unchanged':True,'source_weights_on_socket':0,
                         'sword_model_attach_node':'Attach_Grip','result_weapon_physics':'not_connected'},
        'import_options':{'source_dcc':'Blender','up_axis':0,'normal_map_convention':'OpenGL',
                          'unit_scale_multiplier':1.0,'generate_normals':True,'generate_tangents':True,
                          'root_motion_node':'','root_motion_enabled_in_animator':False},
        'preflight':{'errors':errors,'runtime_tested':False,'fbx_roundtrip_tested':False},
    }


def inspect_poses(manifest):
    scene = bpy.context.scene
    control, rig = bpy.data.objects[CONTROL], bpy.data.objects[RIG]
    saved = (control.animation_data.action, control.animation_data.action_slot, scene.frame_current, scene.frame_subframe)
    before = {a.name:fingerprint(a) for a in bpy.data.actions if a.name.startswith('MB_C_')}
    report = {'sampled_frames':0,'evaluated_bones':len(rig.pose.bones),'finite':True,
              'maximum_root_translation_m':0.,'maximum_scale_deviation':0.}
    try:
        for clip in manifest['clips']:
            action = bpy.data.actions[clip['action']]
            control.animation_data.action = action
            control.animation_data.action_slot = action.slots[0]
            start,end = clip['source_frames']
            for frame in sorted({float(start),(start+end)/2,float(end)}):
                scene.frame_set(int(frame),subframe=frame%1)
                control.update_tag();rig.update_tag();bpy.context.view_layer.update()
                report['sampled_frames'] += 1
                report['maximum_root_translation_m'] = max(report['maximum_root_translation_m'],rig.pose.bones['Root'].head.length)
                for bone in rig.pose.bones:
                    report['finite'] &= all(math.isfinite(v) for row in bone.matrix for v in row)
                    report['maximum_scale_deviation'] = max(report['maximum_scale_deviation'],max(abs(v-1) for v in bone.matrix.to_scale()))
                if not all(math.isfinite(v) for row in bpy.data.objects['Sword_Control'].matrix_world for v in row):
                    report['finite'] = False
        if not report['finite'] or report['maximum_root_translation_m'] > .001:
            manifest['preflight']['errors'].append('Invalid pose or non-stationary root')
        manifest['preflight']['pose_samples'] = report
    finally:
        control.animation_data.action,control.animation_data.action_slot = saved[:2]
        scene.frame_set(saved[2],subframe=saved[3]);bpy.context.view_layer.update()
        assert before == {a.name:fingerprint(a) for a in bpy.data.actions if a.name.startswith('MB_C_')}


def organize_collections():
    root = bpy.context.scene.collection
    visibility = {o.name:o.hide_get() for o in bpy.context.scene.objects}
    groups = [('01_Models_LOD',('TRIPO_LOD0','TRIPO_LOD1','TRIPO_LOD2')),
              ('02_Rigs_Controls',('WGTS_MiniBotC_Metarig',)),
              ('03_Motion_References',('Animation_References',)),
              ('04_Preview',('STUDIO_Preview','Animation_Preview'))]
    for name,children in groups:
        group = bpy.data.collections.get(name) or bpy.data.collections.new(name)
        if name not in root.children:
            root.children.link(group)
        for child_name in children:
            child = bpy.data.collections.get(child_name)
            if child is None:
                continue
            if child.name not in group.children:
                group.children.link(child)
            if child.name in root.children:
                root.children.unlink(child)
    rigs = bpy.data.collections['02_Rigs_Controls']
    for name in (CONTROL,RIG,'MiniBotC_Metarig','Sword_Control','Companion_Rig',
                 'LeftHand_GripTarget','LeftHand_GripPivot','Defeat_SwordGround'):
        obj = bpy.data.objects[name]
        if obj.name not in rigs.objects:
            rigs.objects.link(obj)
        if obj.name in root.objects:
            root.objects.unlink(obj)
    for name,hidden in visibility.items():
        bpy.data.objects[name].hide_set(hidden)


def main(organize=False):
    manifest = collect()
    inspect_poses(manifest)
    assert not manifest['preflight']['errors'], manifest['preflight']['errors']
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/'ExportManifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    with (OUT/'Clips.csv').open('w',encoding='utf-8-sig',newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['Action','表示名','分類','開始','末尾を含む終了','秒数','ループ','FBX'])
        for c in manifest['clips']:
            writer.writerow([c['action'],c['label'],c['category'],*c['source_frames'],c['duration_seconds'],c['loop'],c['fbx']])
    if organize:
        organize_collections()
    text = bpy.data.texts.get('MiniBotC_ExportManifest.json') or bpy.data.texts.new('MiniBotC_ExportManifest.json')
    text.clear();text.write(json.dumps(manifest,ensure_ascii=False,indent=2))
    return manifest


if __name__ == '__main__':
    manifest = main()
    print(json.dumps({'status':manifest['status'],'models':len(manifest['models']),
                      'clips':len(manifest['clips']),'textures':len(manifest['textures']),
                      'preflight':manifest['preflight']},ensure_ascii=False))
