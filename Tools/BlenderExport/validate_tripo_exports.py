"""FBX と GLB を再読込し、座標・骨格・ウェイト・材質の受け渡しを検証する。"""

import bpy
import json
import struct
import numpy as np
from pathlib import Path
from mathutils import Vector
from mathutils.kdtree import KDTree

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'GreenWare/Assets/Models/MiniBotC_GameReady'
REPORT=ROOT/'Docs/Art/MiniBotC/GameReady'
bpy.context.window.scene=bpy.data.scenes['MiniBotC_GameReady']
if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
rig=bpy.data.objects['MiniBotC_Humanoid']
expected_bones={b.name:{'parent':b.parent.name if b.parent else None,'head':list(b.head_local)} for b in rig.data.bones}
expected={}
for obj in bpy.context.scene.objects:
    if obj.type=='MESH' and 'LOD' in obj.name:
        expected[obj.name]={'vertices':[v.co.copy() for v in obj.data.vertices],
                            'triangles':sum(len(p.vertices)-2 for p in obj.data.polygons)}


def Positions(obj):
    evaluated=obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    return np.array([evaluated.matrix_world@v.co for v in evaluated.data.vertices])


reports={}
for path in sorted(OUT.glob('*.*')):
    if path.suffix not in ['.fbx','.glb']:continue
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.suffix=='.fbx':bpy.ops.import_scene.fbx(filepath=str(path),use_anim=False)
    else:bpy.ops.import_scene.gltf(filepath=str(path))
    bpy.context.view_layer.update()
    bones=[o for o in bpy.context.scene.objects if o.type=='ARMATURE']
    widgets={p.custom_shape for arm in bones for p in arm.pose.bones if p.custom_shape}
    meshes=[o for o in bpy.context.scene.objects if o.type=='MESH' and o not in widgets]
    result={'meshes':{},'armatures':len(bones),'missing_images':[]}
    if 'Player' in path.stem:
        assert len(bones)==1,f'Armature count: {path.name}'
        arm=bones[0]
        assert set(arm.data.bones.keys())==set(expected_bones),f'Bone names: {path.name}'
        bone_error=0
        for name,ref in expected_bones.items():
            bone=arm.data.bones[name]
            assert (bone.parent.name if bone.parent else None)==ref['parent'],f'Bone hierarchy: {name}'
            bone_error=max(bone_error,(arm.matrix_world@bone.head_local-Vector(ref['head'])).length)
        assert bone_error<.0001,f'Bone world position: {bone_error}'
        result['bone_count']=len(arm.data.bones);result['bone_head_max_error_m']=bone_error
    else:assert not bones,'Sword must be an independent static mesh'
    for mesh in meshes:
        ref=expected[mesh.name]
        tree=KDTree(len(ref['vertices']))
        for i,co in enumerate(ref['vertices']):tree.insert(co,i)
        tree.balance()
        co=Positions(mesh)
        error=max(tree.find(Vector(v))[2] for v in co)
        assert error<.0001,f'Mesh rest changed {mesh.name}: {error}'
        triangles=sum(len(p.vertices)-2 for p in mesh.data.polygons)
        assert triangles==ref['triangles'],f'Triangle count: {mesh.name}'
        assert mesh.data.uv_layers and np.isfinite(co).all(),f'Missing UV/non-finite vertices: {mesh.name}'
        info={'triangles':triangles,'vertices':len(co),'rest_max_error_m':error,'uv_layers':len(mesh.data.uv_layers)}
        if 'Player' in mesh.name:
            weights=[[g.weight for g in v.groups if g.weight>0] for v in mesh.data.vertices]
            info['max_influences']=max(map(len,weights))
            info['weight_sum_max_error']=max(abs(sum(w)-1) for w in weights)
            assert info['max_influences']<=4 and info['weight_sum_max_error']<.0001,f'Invalid weights: {mesh.name}'
        images={n.image for mat in mesh.data.materials for n in mat.node_tree.nodes if n.type=='TEX_IMAGE' and n.image}
        info['material_images']=len(images)
        for image in images:
            if not image.packed_file and not Path(bpy.path.abspath(image.filepath)).is_file():result['missing_images'].append(image.filepath)
        assert images,'No texture references'
        result['meshes'][mesh.name]=info
    assert not result['missing_images'],f'Missing images: {path.name}'
    if bones:
        arm=bones[0]
        main=next(o for o in meshes if 'Fingers' not in o.name)
        before=Positions(main)
        pb=arm.pose.bones['LeftForeArm'];pb.rotation_mode='XYZ';pb.rotation_euler.x=.6
        bpy.context.view_layer.update()
        delta=np.linalg.norm(Positions(main)-before,axis=1)
        result['forearm_pose_moved_vertices']=int(np.sum(delta>.001))
        result['forearm_pose_foot_drift_m']=float(delta[before[:,2]<.15].max())
        assert result['forearm_pose_moved_vertices']>100,'Imported rig does not deform'
        assert result['forearm_pose_foot_drift_m']<.0001,'Hand weights leaked into feet'
    if path.suffix=='.glb':
        binary=path.read_bytes();length=struct.unpack_from('<I',binary,12)[0]
        gltf=json.loads(binary[20:20+length])
        materials=gltf['materials']
        for material in materials:
            assert 'baseColorTexture' in material['pbrMetallicRoughness'],'GLB Base Color missing'
            assert 'metallicRoughnessTexture' in material['pbrMetallicRoughness'],'GLB ORM missing'
            assert 'occlusionTexture' in material,'GLB AO missing'
        result['glb_materials_with_orm_ao']=len(materials)
    reports[path.name]=result
    print('ROUNDTRIP PASS '+path.name+' '+json.dumps(result),flush=True)
(REPORT/'ExportValidation.json').write_text(json.dumps(reports,indent=2),encoding='utf8')
print('ALL EXPORTS VERIFIED',flush=True)
