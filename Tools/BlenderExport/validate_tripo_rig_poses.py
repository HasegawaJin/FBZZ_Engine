"""制作コントロールで可動試験を行い、保存可能な確認アクションを残す。"""
import bpy,json,math,numpy as np
from pathlib import Path
from mathutils import Vector,Matrix,Quaternion
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'Docs/Art/MiniBotC/AnimationReady';OUT.mkdir(parents=True,exist_ok=True)
STAGE=ROOT/'GreenWare/Assets/_src/MiniBotC/AnimationReady'
scene=bpy.context.scene;control=bpy.data.objects['MiniBotC_ControlRig'];rig=bpy.data.objects['MiniBotC_Humanoid'];comp=bpy.data.objects['Companion_Rig']
body=bpy.data.objects['Player_LOD0'];weapon=bpy.data.objects['Sword_Control'];grip=bpy.data.objects['LeftHand_GripTarget']
rest={p.name:p.matrix_basis.copy() for p in control.pose.bones}
def update():
    control.update_tag();rig.update_tag();comp.update_tag();bpy.context.view_layer.update()
def reset():
    for p in control.pose.bones:p.matrix_basis=rest[p.name]
    control['two_hand_grip']=0.0
    for p in comp.pose.bones:p.matrix_basis=Matrix.Identity(4)
    update()
def move(name,xyz):
    p=control.pose.bones[name];m=p.matrix.copy();m.translation=Vector(xyz);p.matrix=m
def rotate(name,axis,angle):
    p=control.pose.bones[name];p.rotation_mode='QUATERNION';p.rotation_quaternion=Quaternion(axis,angle)
def vertices(obj):
    update();eo=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());me=eo.to_mesh()
    a=np.empty(len(me.vertices)*3,np.float32);me.vertices.foreach_get('co',a);eo.to_mesh_clear();return a.reshape(-1,3)
def curl(amount,side='R'):
    for stem in ['f_index','f_middle','f_ring','f_pinky']:
        for j in range(1,4):rotate(f'{stem}.{j:02d}.{side}',(1,0,0),amount)
def pose(name):
    reset()
    if name=='ArmsUp':
        move('hand_ik.L',(.52,-.12,1.48));move('hand_ik.R',(-.52,-.12,1.48))
    elif name=='Crouch':
        p=control.pose.bones['torso'];p.location.z=-.20;p.location.y=-.045
        rotate('chest',(1,0,0),.15)
        move('hand_ik.L',(.42,-.20,.77));move('hand_ik.R',(-.42,-.20,.77))
    elif name in ['Grip','TwoHand']:
        move('hand_ik.R',(-.15,-.30,1.01) if name=='Grip' else (-.015,-.14,1.08));rotate('hand_ik.R',(1,0,0),-.90);curl(.55,'R')
        if name=='TwoHand':control['two_hand_grip']=1.0;curl(.55,'L')
    elif name=='IndexCurl':
        for j in range(1,4):rotate(f'f_index.{j:02d}.L',(1,0,0),.65)
    elif name=='Step':
        move('foot_ik.L',(.215,-.30,.24));move('hand_ik.R',(-.28,-.20,.93));rotate('head',(0,0,1),.3)
    update()
scene.render.engine='CYCLES';scene.cycles.samples=20;scene.cycles.device='GPU'
prefs=bpy.context.preferences.addons['cycles'].preferences;prefs.compute_device_type='OPTIX';prefs.refresh_devices()
for d in prefs.devices:d.use=d.type=='OPTIX'
cam=scene.camera;cam.data.type='ORTHO'
def render(name,location=(2.5,-5,2.25),target=(-.1,-.10,.90),scale=2.70):
    cam.location=location;cam.rotation_euler=(Vector(target)-cam.location).to_track_quat('-Z','Y').to_euler();cam.data.ortho_scale=scale
    scene.render.resolution_x=1400;scene.render.resolution_y=1200;scene.render.resolution_percentage=100
    scene.render.filepath=str(OUT/(name+'.png'));bpy.ops.render.render(write_still=True)
for o in scene.objects:
    if o.type=='MESH' and o.name.startswith('WGT-'):o.hide_render=True
reset();base=vertices(body);original=np.array([v.co[:] for v in body.data.vertices]);rest_error=float(np.max(np.linalg.norm(base-original,axis=1)))
fingers=bpy.data.objects['Player_Fingers_LOD0'];finger_rest=vertices(fingers)
assert rest_error<1e-4,f'Rest binding changed shape {rest_error}'
edges=np.array([e.vertices[:] for e in body.data.edges]);length=np.linalg.norm(base[edges[:,0]]-base[edges[:,1]],axis=1)
report={'rest_error_m':rest_error,'poses':{}}
for label in ['ArmsUp','Crouch','Grip','TwoHand','IndexCurl','Step']:
    pose(label);p=vertices(body);ratio=np.linalg.norm(p[edges[:,0]]-p[edges[:,1]],axis=1)/np.maximum(length,1e-5)
    report['poses'][label]={'edge_stretch_p99':float(np.quantile(ratio,.99)),'edge_stretch_max':float(ratio.max()),'stretched_edges_gt_3':int((ratio>3).sum()),'finite':bool(np.isfinite(p).all())}
    assert np.isfinite(p).all()
    if label=='TwoHand':
        error=(rig.pose.bones['LeftHand'].head-grip.matrix_world.translation).length;report['two_hand_error_m']=error
        assert error<.005,f'Two-hand IK misses target {error}'
    if label=='IndexCurl':
        changed=np.linalg.norm(vertices(fingers)-finger_rest,axis=1)
        index_ids=[v.index for v in fingers.data.vertices if any(fingers.vertex_groups[g.group].name.startswith('LeftHandIndex') and g.weight>.9 for g in v.groups)]
        other_ids=[i for i in range(len(changed)) if i not in set(index_ids)]
        report['independent_index_moved_vertices']=int((changed[index_ids]>.001).sum());report['other_finger_drift_m']=float(changed[other_ids].max())
        assert report['independent_index_moved_vertices']>50 and report['other_finger_drift_m']<1e-5
        render('Pose_IndexCurl',(3,.055,.67),(.39,.045,.68),.32)
        np.savez_compressed(STAGE/'FingerPose.npz',rest=base,posed=p,edges=edges,ratio=ratio)
    else:render('Pose_'+label)
reset();before=weapon.matrix_world.copy();before_body=vertices(body)
control.pose.bones['root'].location.x=.3;update()
report['root_translation_error_m']=float(np.max(np.abs((vertices(body)-before_body)-np.array([.3,0,0]))))
report['weapon_root_follow_error_m']=abs((weapon.matrix_world.translation-before.translation).x-.3)
assert report['root_translation_error_m']<1e-4 and report['weapon_root_follow_error_m']<1e-4
reset();cbody=bpy.data.objects['Companion_LOD0'];cv=vertices(cbody);m=comp.pose.bones['Body'].matrix.copy();m.translation.z+=.08;comp.pose.bones['Body'].matrix=m;update()
report['companion_hover_error_m']=float(np.max(np.abs(vertices(cbody)-cv-np.array([0,0,.08]))))
assert report['companion_hover_error_m']<1e-4
reset()
keys=['root','torso','chest','hips','head','hand_ik.L','hand_ik.R','foot_ik.L','foot_ik.R']
keys += [f'{stem}.{j:02d}.{side}' for stem in ['f_index','f_middle','f_ring','f_pinky','thumb'] for j in range(1,4) for side in ['L','R']]
for frame,label in [(1,'Rest'),(16,'ArmsUp'),(31,'Crouch'),(46,'Grip'),(61,'TwoHand'),(76,'Step'),(91,'Rest')]:
    scene.frame_set(frame);pose(label)
    for name in keys:
        p=control.pose.bones[name];p.rotation_mode='QUATERNION'
        p.keyframe_insert('location',frame=frame,group=name);p.keyframe_insert('rotation_quaternion',frame=frame,group=name);p.keyframe_insert('scale',frame=frame,group=name)
    control.keyframe_insert('["two_hand_grip"]',frame=frame,group='Weapon')
    marker=scene.timeline_markers.new(label,frame=frame)
action=control.animation_data.action;action.name='DEMO_RigCheck';action.use_fake_user=True
report['diagnostic_action']=action.name
control.animation_data.action=None;reset();scene.frame_set(1)
for name in keys:
    p=control.pose.bones[name];p.keyframe_insert('location',frame=1,group=name);p.keyframe_insert('rotation_quaternion',frame=1,group=name);p.keyframe_insert('scale',frame=1,group=name)
control.keyframe_insert('["two_hand_grip"]',frame=1,group='Weapon')
control.animation_data.action.name='Player_NewAction'
scene.frame_end=91
(OUT/'PoseValidation.json').write_text(json.dumps(report,indent=2),encoding='utf8')
scene['pipeline_stage']='rig_pose_verified'
bpy.ops.wm.save_as_mainfile(filepath=str(STAGE/'02_PoseVerified.blend'),compress=True)
print('POSE CHECK COMPLETE '+json.dumps(report),flush=True)
