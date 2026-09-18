"""外装と内部フレームのクリアランスを確定し、配布用状態で保存する。"""

import math

import bpy
from mathutils import Euler, Vector

rig = bpy.data.objects['Boss03_Armature']
if not rig.get('ArmorClearanceFinal',False):
    for side,s in [('L',-1),('R',1)]:
        for tier,angle in [('Upper',18),('Middle',61),('Lower',139)]:
            obj = bpy.data.objects[f'Wing_{side}_{tier}_ShieldStructure']
            delta = obj.matrix_world.to_3x3().inverted() @ (
                Euler((0,math.radians(s*angle),0)).to_matrix() @ Vector((0,.24,0)))
            for v in obj.data.vertices:
                v.co += delta
            obj.data.update()
    rig['ArmorClearanceFinal'] = True
if not rig.get('HullRetractionFinal',False):
    closures = {
        'Boss03_Cocoon_Idle':[(1,1),(31,1),(61,1),(91,1),(121,1)],
        'Boss03_Close':[(1,0),(15,.05),(45,1),(61,1)],
        'Boss03_Deploy':[(1,1),(18,1),(54,.08),(72,0),(91,0)],
        'Boss03_Dash_InPlace':[(1,0),(23,1),(33,1),(54,1),(65,.65),(91,0)],
        'Boss03_Death':[(1,0),(24,.18),(55,.65),(85,.72),(121,.72)],
        'Boss03_Showcase':[(1,0),(31,0),(81,1),(121,1),(151,1),(211,0),(241,0)]}
    for action_name,keys in closures.items():
        action = bpy.data.actions[action_name]
        rig.animation_data.action = action
        rig.animation_data.action_slot = action.slots[0]
        for frame,factor in keys:
            bpy.context.scene.frame_set(frame)
            body = rig.pose.bones['Body']
            body.location.y = -.50*factor
            body.keyframe_insert(data_path='location',frame=frame,group='Body')
    for obj in bpy.data.collections['EXPORT_Boss03'].objects:
        if obj.type == 'MESH' and '_RearVent_' in obj.name:
            for v in obj.data.vertices:
                v.co.y += .30
            obj.data.update()
    rig['HullRetractionFinal'] = True
rig.animation_data.action = bpy.data.actions['Boss03_Showcase']
rig.animation_data.action_slot = rig.animation_data.action.slots[0]
scene = bpy.context.scene
scene.frame_set(1)
scene.camera = bpy.data.objects['Camera_Hero']
bpy.data.collections['STUDIO_Preview_Only'].hide_viewport = True
bpy.ops.wm.save_as_mainfile(filepath=bpy.data.filepath)
