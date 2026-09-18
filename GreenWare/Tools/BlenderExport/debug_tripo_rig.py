import bpy,json
from mathutils import Matrix
scene=bpy.data.scenes['MiniBotC_GameReady'];bpy.context.window.scene=scene
control=bpy.data.objects['MiniBotC_ControlRig'];rig=bpy.data.objects['MiniBotC_Humanoid']
bpy.context.view_layer.update()
def State():
    out=[]
    for name in ['Root','Hips','Chest','Head','LeftArm','LeftForeArm','LeftHand','LeftUpLeg','LeftLeg','LeftFoot']:
        p=rig.pose.bones[name];b=rig.data.bones[name]
        src=control.pose.bones[json.loads(rig['humanoid_mapping'])[name]]
        delta=p.matrix@b.matrix_local.inverted()
        out.append({'name':name,'rest_head':list(b.head_local),'pose_head':list(p.head),'rest_length':b.length,
                    'pose_scale':list(p.matrix.to_scale()),'delta':[[round(x,4) for x in row] for row in delta],
                    'source_rest_head':list(src.bone.head_local),'source_pose_head':list(src.head),'source_pose_scale':list(src.matrix.to_scale())})
    return out
print('BEFORE',json.dumps(State()),flush=True)
for side in ['L','R']:
    control.pose.bones[f'upper_arm_parent.{side}']['IK_FK']=1
    control.pose.bones[f'thigh_parent.{side}']['IK_FK']=1
bpy.context.view_layer.update()
print('AFTER_FK',json.dumps(State()),flush=True)
print('OBJECTS',[(o.name,list(o.location),list(o.scale)) for o in [control,rig,bpy.data.objects['Player_LOD0']]],flush=True)
