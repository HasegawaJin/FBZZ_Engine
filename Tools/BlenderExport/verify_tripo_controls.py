"""武器追従と両手 IK、および遠距離 LOD の表示を確認する。"""

import bpy
import json
import sys
import numpy as np
from pathlib import Path
from mathutils import Vector

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'Docs/Art/MiniBotC/GameReady'
scene=bpy.data.scenes['MiniBotC_GameReady'];bpy.context.window.scene=scene
control=bpy.data.objects['MiniBotC_ControlRig']
rig=bpy.data.objects['MiniBotC_Humanoid']
sword=bpy.data.objects['Sword_LOD0']
grip=bpy.data.objects['LeftHand_GripTarget']
rest={p.name:p.matrix_basis.copy() for p in control.pose.bones}
cam=scene.camera
scene.cycles.device='GPU'
preferences=bpy.context.preferences.addons['cycles'].preferences
preferences.compute_device_type='OPTIX';preferences.refresh_devices()
for device in preferences.devices:device.use=device.type=='OPTIX'
scene.cycles.samples=24


def Update():
    control.update_tag();scene.frame_set(scene.frame_current);bpy.context.view_layer.update()


def Render(name,location,target,scale):
    cam.location=location;cam.rotation_euler=(Vector(target)-cam.location).to_track_quat('-Z','Y').to_euler()
    cam.data.ortho_scale=scale
    scene.render.resolution_x=1400;scene.render.resolution_y=1400
    scene.render.filepath=str(OUT/(name+'.png'))
    bpy.ops.render.render(write_still=True)


if '--lod-only' in sys.argv:
    for obj in scene.objects:
        if 'LOD0' in obj.name:obj.hide_render=True
        elif 'LOD1' in obj.name:obj.hide_render=False;obj.hide_set(False)
    bpy.data.collections['GAME_LOD1'].hide_render=False
    Render('LOD1_Preview',(3,-5,2.1),(-.18,-.12,.87),2.35)
    print('LOD1 NORMAL PREVIEW COMPLETE',flush=True)
    raise SystemExit(0)

before=sword.matrix_world.translation.copy()
right=control.pose.bones['hand_ik.R']
matrix=right.matrix.copy();matrix.translation=Vector((-.025,-.22,1.025));right.matrix=matrix
Update()
weapon_delta=(sword.matrix_world.translation-before).length
assert weapon_delta>.20,'Sword socket does not follow right hand'
control['two_hand_grip']=1.0
Update()
left_error=(control.pose.bones['hand_ik.L'].matrix.translation-grip.matrix_world.translation).length
hand_error=(rig.pose.bones['LeftHand'].head-grip.matrix_world.translation).length
report={'weapon_follow_displacement_m':weapon_delta,'left_ik_target_error_m':left_error,
        'left_hand_grip_error_m':hand_error,'grip_influence':control.pose.bones['hand_ik.L'].constraints['Two hand grip'].influence}
print('CONTROL CHECK '+json.dumps(report),flush=True)
assert left_error<.0001 and hand_error<.005,'Left hand does not reach reachable grip target'
for side in ['L','R']:
    for stem in ['f_index','f_middle','f_ring','f_pinky']:
        for j in range(1,4):
            pb=control.pose.bones[f'{stem}.{j:02d}.{side}'];pb.rotation_mode='XYZ';pb.rotation_euler.x=.55
Update()
Render('Pose_TwoHand',(3,-5,2.0),(0,-.18,.90),2.25)
for p in control.pose.bones:p.matrix_basis=rest[p.name]
control['two_hand_grip']=0.0
Update()
for stem in ['f_index','f_middle','f_ring','f_pinky']:
    for j in range(1,4):
        pb=control.pose.bones[f'{stem}.{j:02d}.R'];pb.rotation_mode='XYZ';pb.rotation_euler.x=.55
Update()
Render('Hero',(3,-5,2.1),(-.18,-.12,.87),2.35)
for obj in scene.objects:
    if 'LOD0' in obj.name:obj.hide_render=True
    elif 'LOD1' in obj.name:obj.hide_render=False;obj.hide_set(False)
bpy.data.collections['GAME_LOD1'].hide_render=False
Render('LOD1_Preview',(3,-5,2.1),(-.18,-.12,.87),2.35)
(OUT/'ControlValidation.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print('CONTROLS AND LOD VERIFIED',flush=True)
