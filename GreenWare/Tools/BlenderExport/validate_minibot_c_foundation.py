"""各FBXの時間・骨格・スキン変形を正本と照合する。"""
import bpy
import json
import numpy as np
from pathlib import Path
from mathutils import Vector
from mathutils.kdtree import KDTree
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
ART=ROOT/'Docs/Art/MiniBotC/Foundation'
OUT=ROOT/'GreenWare/Assets/Models/MiniBotC_AnimationReady/Animations'
reports=json.loads((ART/'ExportReport.json').read_text(encoding='utf8'))
reference=np.load(ART/'ExportReference.npz')
bone_ref={b.name:(b.parent.name if b.parent else None) for b in bpy.data.objects['MiniBotC_Humanoid'].data.bones}
results={}
for name,entry in reports.items():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps=30
    bpy.ops.import_scene.fbx(filepath=str(OUT/(name+'.fbx')),use_anim=True,anim_offset=0.0)
    arm=next(o for o in bpy.context.scene.objects if o.type=='ARMATURE')
    assert {b.name:(b.parent.name if b.parent else None) for b in arm.data.bones}==bone_ref
    assert arm.animation_data and arm.animation_data.action
    action=arm.animation_data.action
    assert np.max(np.abs(np.array(action.frame_range)-entry['frames']))<1e-4
    bpy.context.view_layer.objects.active=arm;arm.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    for b in arm.data.edit_bones:b.use_connect=False
    bpy.ops.object.mode_set(mode='OBJECT')
    meshes=[o for o in bpy.context.scene.objects if o.type=='MESH']
    assert len(meshes)==2
    maximum=0
    for frame in entry['sample_frames']:
        bpy.context.scene.frame_set(frame);bpy.context.view_layer.update()
        for obj in meshes:
            source_name=obj.name.removeprefix('EXPORT_')
            expected=reference[f'{name}_{frame}_{source_name}']
            tree=KDTree(len(expected))
            for i,point in enumerate(expected):tree.insert(Vector(point),i)
            tree.balance()
            evaluated=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=evaluated.to_mesh()
            error=max(tree.find(evaluated.matrix_world@v.co)[2] for v in mesh.vertices)
            evaluated.to_mesh_clear()
            maximum=max(maximum,error)
            assert error<.0001,(name,frame,obj.name,error)
            influences=[sum(g.weight>0 for g in v.groups) for v in obj.data.vertices]
            assert min(influences)>0 and max(influences)<=4
    results[name]={'passed':True,'bones':54,'frames':entry['frames'],'checked_frames':entry['sample_frames'],
                   'max_surface_error_m':maximum,'imported_action':action.name}
    print('FOUNDATION PASS '+name+' '+str(maximum),flush=True)
(ART/'ExportValidation.json').write_text(json.dumps({'fbzz_runtime_verified':False,'blender_reimport_connection_correction':True,'clips':results},indent=2),encoding='utf8')
print('FOUNDATION VALIDATION COMPLETE',flush=True)
