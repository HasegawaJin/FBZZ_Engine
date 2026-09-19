"""制作台帳と同期して基準4本を出力し、再読込用の比較座標を残す。"""
import bpy
import json
import sys
import numpy as np
from pathlib import Path
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'GreenWare/Assets/Models/MiniBotC_AnimationReady/Animations'
ART=ROOT/'Docs/Art/MiniBotC/Foundation'
sys.path.insert(0,str(ROOT/'GreenWare/Tools/BlenderExport'))
from export_animation_ready_clip import export_clip
scene=bpy.context.scene;control=bpy.data.objects['MiniBotC_ControlRig'];rig=bpy.data.objects['MiniBotC_Humanoid']
manifest=json.loads((OUT/'motion_manifest.json').read_text(encoding='utf8'))
old=control.animation_data.action;old_frame=scene.frame_current
samples={};report={}
for name,clip in manifest['clips'].items():
    action=bpy.data.actions[name];control.animation_data.action=action
    start,end=clip['start_frame'],clip['end_frame']
    check_frames=sorted(set([start,end,(start+end)//2]+[x['frame'] for x in clip['markers'].values()]))
    for frame in check_frames:
        scene.frame_set(frame);bpy.context.view_layer.update()
        for objname in ['Player_LOD0','Player_Fingers_LOD0']:
            obj=bpy.data.objects[objname];ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh()
            samples[f'{name}_{frame}_{objname}']=np.array([ev.matrix_world@v.co for v in mesh.vertices],dtype=np.float32)
            ev.to_mesh_clear()
        samples[f'{name}_{frame}_bones']=np.array([p.matrix for p in rig.pose.bones],dtype=np.float32)
        assert max(abs(float(s)-1) for p in rig.pose.bones for s in p.matrix.to_scale())<1e-4
    result=export_clip(OUT/(name+'.fbx'),action=action,start=start,end=end)
    result['sample_frames']=check_frames
    report[name]=result
    clip['fbx_file']=name+'.fbx'
    clip['baked_action']=name+'_Baked'
    hint={'sourceDcc':'blender','sourceFile':'../../../_src/MiniBotC/MiniBotC_PlayerMotions.blend','sourceAction':name,'fps':30,'frameStart':start,'frameEnd':end,'loop':clip['loop']}
    (OUT/(name+'.fbx.fzhint')).write_text(json.dumps(hint,indent=2),encoding='utf8')
    print('FOUNDATION EXPORTED '+name,flush=True)
control.animation_data.action=old;scene.frame_set(old_frame)
np.savez_compressed(ART/'ExportReference.npz',**samples)
(ART/'ExportReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')
(OUT/'motion_manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
print('FOUNDATION EXPORT COMPLETE',flush=True)
