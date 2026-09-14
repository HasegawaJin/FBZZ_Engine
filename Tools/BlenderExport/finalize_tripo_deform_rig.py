"""Rigify の背骨に由来する非一様スケールをゲーム骨格へ持ち込まない。"""
import bpy,json
from pathlib import Path
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine');scene=bpy.context.scene
rig=bpy.data.objects['MiniBotC_Humanoid'];control=bpy.data.objects['MiniBotC_ControlRig'];mapping=json.loads(rig['humanoid_mapping'])
for name,target in mapping.items():
    pb=rig.pose.bones[name]
    for c in list(pb.constraints):pb.constraints.remove(c)
    pb.scale=(1,1,1)
    for kind in ['COPY_LOCATION','COPY_ROTATION']:
        c=pb.constraints.new(kind);c.target=control;c.subtarget=target;c.owner_space=c.target_space='POSE';c.name='Rigify position' if kind=='COPY_LOCATION' else 'Rigify rotation'
old=control.animation_data.action;control.animation_data.action=bpy.data.actions['DEMO_RigCheck']
scales={}
for frame in [1,16,31,46,61,76,91]:
    scene.frame_set(frame);bpy.context.view_layer.update()
    error=max(abs(s-1) for p in rig.pose.bones for s in p.matrix.to_scale())
    assert error<1e-5,f'Non-rigid game bone {frame}: {error}'
    scales[str(frame)]=error
control.animation_data.action=old;scene.frame_set(1)
rig['binding']='Position/rotation from Rigify; unit scale on every game bone; linear skinning max four weights'
(ROOT/'Docs/Art/MiniBotC/AnimationReady/GameBoneScaleValidation.json').write_text(json.dumps(scales,indent=2),encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'GreenWare/Assets/_src/MiniBotC/AnimationReady/03_ExportPose.blend'),compress=True)
print('UNIT-SCALE GAME BONES VERIFIED '+str(scales),flush=True)
