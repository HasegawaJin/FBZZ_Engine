"""スキン付き LOD とベイク済み確認クリップを独立に読み直して検証する。"""
import bpy,json,numpy as np
from pathlib import Path
from mathutils import Vector
from mathutils.kdtree import KDTree
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine');OUT=ROOT/'GreenWare/Assets/Models/MiniBotC_AnimationReady'
ART=ROOT/'Docs/Art/MiniBotC/AnimationReady';report=json.loads((OUT/'AssetReport.json').read_text(encoding='utf8'))
if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
refs={}
for o in bpy.context.scene.objects:
    if o.type=='MESH' and '_LOD' in o.name:
        refs[o.name]={'points':np.array([v.co[:] for v in o.data.vertices]),'triangles':sum(len(p.vertices)-2 for p in o.data.polygons)}
bone_refs={}
source_rig=bpy.data.objects['MiniBotC_Humanoid'];source_control=bpy.data.objects['MiniBotC_ControlRig']
source_control.animation_data.action=bpy.data.actions['DEMO_RigCheck'];source_pose={}
for frame in [1,16,31,46,61,76,91]:
    bpy.context.scene.frame_set(frame);bpy.context.view_layer.update()
    source_pose[frame]={p.name:(source_rig.matrix_world@p.matrix).translation.copy() for p in source_rig.pose.bones}
for label,name in [('Player','MiniBotC_Humanoid'),('Companion','Companion_Rig')]:
    bone_refs[label]={b.name:{'parent':b.parent.name if b.parent else None,'head':b.head_local.copy()} for b in bpy.data.objects[name].data.bones}
def points(o):
    eo=o.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=eo.to_mesh();p=np.array([eo.matrix_world@v.co for v in mesh.vertices]);eo.to_mesh_clear();return p
def error(a,b):
    tree=KDTree(len(a))
    for i,p in enumerate(a):tree.insert(Vector(p),i)
    tree.balance();return max(tree.find(Vector(p))[2] for p in b)
results={}
for label,entry in report['models'].items():
    for ext in ['fbx','glb']:
        path=OUT/(label+'.'+ext);bpy.ops.wm.read_factory_settings(use_empty=True)
        if ext=='fbx':bpy.ops.import_scene.fbx(filepath=str(path),use_anim=False)
        else:bpy.ops.import_scene.gltf(filepath=str(path))
        bpy.context.view_layer.update()
        arms=[o for o in bpy.context.scene.objects if o.type=='ARMATURE'];widgets={p.custom_shape for arm in arms for p in arm.pose.bones if p.custom_shape}
        meshes=[o for o in bpy.context.scene.objects if o.type=='MESH' and o not in widgets]
        assert len(arms)==int(entry['bones']>0) and len(meshes)==len(entry['meshes']),f'Unexpected exported objects {path.name}'
        info={'meshes':{},'bones':entry['bones']}
        if arms:
            arm=arms[0];ref=bone_refs[label.split('_')[0]]
            assert set(arm.data.bones.keys())==set(ref),f'Bone names changed {path.name}'
            head_error=0
            for name,r in ref.items():
                b=arm.data.bones[name];assert (b.parent.name if b.parent else None)==r['parent']
                head_error=max(head_error,(arm.matrix_world@b.head_local-r['head']).length)
            assert head_error<1e-4,f'Bind bone position {path.name}: {head_error}'
            info['bind_bone_error_m']=head_error
        for obj in meshes:
            r=refs[obj.name];p=points(obj);maximum=error(r['points'],p)
            assert maximum<1e-4 and np.isfinite(p).all(),f'Rest geometry {path.name} {obj.name}: {maximum}'
            assert sum(len(p.vertices)-2 for p in obj.data.polygons)==r['triangles'] and len(obj.data.uv_layers)==1
            assert np.isfinite(np.array([v.uv[:] for v in obj.data.uv_layers.active.data])).all()
            data={'triangles':r['triangles'],'rest_error_m':maximum}
            if arms:
                weights=[[g.weight for g in v.groups if g.weight>0] for v in obj.data.vertices]
                assert min(map(len,weights))>0 and max(map(len,weights))<=4
                assert max(abs(sum(w)-1) for w in weights)<1e-4
                data['max_influences']=max(map(len,weights))
            for mat in obj.data.materials:
                shader=next(n for n in mat.node_tree.nodes if n.type=='BSDF_PRINCIPLED')
                assert shader.inputs['Base Color'].is_linked
                if 'Fingers' not in obj.name:assert shader.inputs['Normal'].is_linked
                images={n.image for n in mat.node_tree.nodes if n.type=='TEX_IMAGE' and n.image}
                assert images and all(i.packed_file or Path(bpy.path.abspath(i.filepath)).is_file() for i in images)
            info['meshes'][obj.name]=data
        if label.startswith('Player'):
            main=next(o for o in meshes if 'Fingers' not in o.name);base=points(main)
            pb=arms[0].pose.bones['LeftForeArm'];pb.rotation_mode='XYZ';pb.rotation_euler.x=.6;bpy.context.view_layer.update()
            delta=np.linalg.norm(points(main)-base,axis=1);info['arm_moved_vertices']=int((delta>.001).sum());info['foot_drift_m']=float(delta[base[:,2]<.15].max())
            assert info['arm_moved_vertices']>100 and info['foot_drift_m']<1e-4
        results[path.name]=info;print('PASS '+path.name,flush=True)
assert len(results)==18
reference=np.load(ROOT/'GreenWare/Assets/_src/MiniBotC/AnimationReady/ClipReference.npz')
bpy.ops.wm.read_factory_settings(use_empty=True);bpy.context.scene.render.fps=30
bpy.ops.import_scene.fbx(filepath=str(OUT/'Animations/DEMO_RigCheck.fbx'),use_anim=True,anim_offset=0.0)
arm=next(o for o in bpy.context.scene.objects if o.type=='ARMATURE');assert len(arm.data.bones)==54 and arm.animation_data and arm.animation_data.action
bpy.context.view_layer.objects.active=arm;arm.select_set(True);bpy.ops.object.mode_set(mode='EDIT')
# Blender の FBX importer が近接した骨を自動接続するため、正本と同じ非接続の親子関係へ戻す。
for bone in arm.data.edit_bones:bone.use_connect=False
bpy.ops.object.mode_set(mode='OBJECT')
clip={}
for frame in [1,16,31,46,61,76,91]:
    bpy.context.scene.frame_set(frame);bpy.context.view_layer.update();frame_error=0
    widgets={p.custom_shape for p in arm.pose.bones if p.custom_shape}
    for obj in [o for o in bpy.context.scene.objects if o.type=='MESH' and o not in widgets]:
        name=obj.name.removeprefix('EXPORT_');e=error(reference[f'{frame}_{name}'],points(obj));frame_error=max(frame_error,e)
    if frame_error>=.001:
        differences=sorted([((arm.matrix_world@p.matrix).translation-source_pose[frame][p.name]).length for p in arm.pose.bones],reverse=True)
        heads=sorted([(((arm.matrix_world@p.matrix).translation-source_pose[frame][p.name]).length,p.name) for p in arm.pose.bones],reverse=True)
        print('CLIP DEBUG '+str({'fps':bpy.context.scene.render.fps,'range':list(arm.animation_data.action.frame_range),'bone_errors':heads[:10]}),flush=True)
        print('SPINE DEBUG '+str({n:{'got':list((arm.matrix_world@arm.pose.bones[n].matrix).translation),'expected':list(source_pose[frame][n])} for n in ['Root','Hips','Spine','Chest','UpperChest','Neck','LeftUpLeg']}),flush=True)
    assert frame_error<.001,f'Baked animation differs at {frame}: {frame_error}'
    clip[str(frame)]={'max_surface_error_m':frame_error};print('CLIP FRAME PASS '+str(frame)+' '+str(frame_error),flush=True)
output={'verified_models':18,'models':results,'baked_animation_frames':clip,'fbzz_runtime_tested':False,
    'blender_fbx_reimport':'Animation offset=0; disable importer-inferred bone connections to retain per-bone translation, as in source rig'}
(ART/'ExportValidation.json').write_text(json.dumps(output,indent=2),encoding='utf8')
print('ALL RIGGED EXPORTS AND BAKED CLIP VERIFIED',flush=True)
