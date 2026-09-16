"""Tripo 由来の Player に Rigify 操作系と独立したゲーム骨格を設定する。

Author: Hasegawa Jin
Date: 2026-09-13
"""

import bpy
import json
import math
import time
import numpy as np
from pathlib import Path
from mathutils import Vector, Matrix

ROOT=Path(r"C:\Users\jinhs\Downloads\FBZZ_Engine")
OUT=ROOT/'GreenWare/Assets/_src/MiniBotC/GameReady'
START=time.time()
scene=bpy.data.scenes['MiniBotC_GameReady']
bpy.context.window.scene=scene


def Log(message):
    print(f'RIG [{time.time()-START:.1f}s] {message}',flush=True)


def Activate(obj):
    if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='DESELECT')
    obj.hide_set(False);obj.select_set(True);bpy.context.view_layer.objects.active=obj


def Smooth(value):
    t=np.clip(value,0,1)
    return t*t*(3-2*t)


def SkinBody(obj,rig):
    coords=np.empty(len(obj.data.vertices)*3,np.float32)
    obj.data.vertices.foreach_get('co',coords)
    coords=coords.reshape(-1,3)
    x,y,z=coords.T;a=np.abs(x)
    n=len(x)
    weights={b.name:np.zeros(n,np.float32) for b in rig.data.bones if b.use_deform}
    assigned=np.zeros(n,bool)
    def Set(mask,name):
        weights[name][mask]=1;assigned[mask]=True
    def Blend(mask,first,second,t):
        t=np.clip(t,0,1)
        weights[first][mask]=1-t[mask];weights[second][mask]=t[mask];assigned[mask]=True
    Set(z>=1.257,'Head')
    neck=(z>=1.20)&(z<1.257)&(a<.104)
    Blend(neck,'Neck','Head',Smooth((z-1.245)/.012))
    arm=((z>1.08)&(a>.151)|(z>.81)&(a>.204)|(z<=.81)&(a>.285))&(z>.50)&(~assigned)
    for side,sign in [('Left',1),('Right',-1)]:
        mask=arm&(x*sign>0)
        upper=mask&(z>=.955)
        Blend(upper,side+'ForeArm',side+'Arm',Smooth((z-.955)/.052))
        lower=mask&(z<.955)&(z>=.766)
        Blend(lower,side+'Hand',side+'ForeArm',Smooth((z-.766)/.040))
        hand=mask&(z<.766)
        Set(hand,side+'Hand')
        thumb=hand&(a<.359)&(z<.746)&(y<.020)
        weights[side+'Hand'][thumb]=0
        Set(thumb&(z>=.700),side+'HandThumb1')
        middle=thumb&(z<.700)&(z>=.670)
        Blend(middle,side+'HandThumb2',side+'HandThumb1',Smooth((z-.692)/.009))
        tip=thumb&(z<.670)
        Blend(tip,side+'HandThumb3',side+'HandThumb2',Smooth((z-.665)/.009))
    torso=~assigned&(z>=.807)
    Set(torso&(z<.921),'Hips')
    band=torso&(z>=.921)&(z<1.018)
    Blend(band,'Spine','Chest',Smooth((z-.964)/.040))
    band=torso&(z>=1.018)
    Blend(band,'Chest','UpperChest',Smooth((z-1.074)/.04))
    legs=~assigned
    for side,sign in [('Left',1),('Right',-1)]:
        mask=legs&(x*sign>=0)
        knee=mask&(z>=.493)
        Blend(knee,side+'Leg',side+'UpLeg',Smooth((z-.512)/.065))
        shin=mask&(z<.493)&(z>=.102)
        Blend(shin,side+'Foot',side+'Leg',Smooth((z-.107)/.047))
        foot=mask&(z<.102)
        Blend(foot,side+'Foot',side+'ToeBase',Smooth((-y-.132)/.05))
    assert assigned.all(),'Unclassified body vertices'
    total=sum(weights.values())
    assert np.min(total)>.999 and np.max(total)<1.001,'Invalid weight sum'
    # 連結した生成メッシュの領域境界を緩め、装甲の境目に細い突起が伸びるのを防ぐ。
    names=list(weights)
    values=np.stack(list(weights.values()),axis=1)
    edges=np.array([e.vertices[:] for e in obj.data.edges],dtype=np.int32)
    source=np.concatenate((edges[:,0],edges[:,1]))
    target=np.concatenate((edges[:,1],edges[:,0]))
    degree=np.maximum(np.bincount(target,minlength=n),1)
    for iteration in range(24):
        active=np.where(values.max(axis=0)>.0001)[0]
        for index in active:
            neighbor=np.bincount(target,weights=values[source,index],minlength=n)/degree
            values[:,index]=.4*values[:,index]+.6*neighbor
    keep=np.argpartition(values,-4,axis=1)[:,-4:]
    limited=np.zeros_like(values)
    np.put_along_axis(limited,keep,np.take_along_axis(values,keep,axis=1),axis=1)
    limited[limited<.001]=0
    limited/=limited.sum(axis=1,keepdims=True)
    integer=np.rint(limited*1024).astype(np.int32)
    integer[np.arange(n),np.argmax(integer,axis=1)]+=1024-integer.sum(axis=1)
    weights={name:integer[:,i]/1024 for i,name in enumerate(names)}
    obj.vertex_groups.clear()
    for name,w in weights.items():
        ids=np.where(w>.0001)[0]
        if not len(ids):continue
        group=obj.vertex_groups.new(name=name)
        quantized=np.round(w[ids]*1024)/1024
        for value in np.unique(quantized):
            if value>0:group.add(ids[quantized==value].tolist(),float(value),'REPLACE')
    return {'vertices':n,'max_influences':int(np.max(sum(w>.0001 for w in weights.values())))}


def Attach(obj,rig):
    obj.parent=rig
    modifier=obj.modifiers.new('Humanoid skin','ARMATURE')
    modifier.object=rig
    modifier.use_deform_preserve_volume=False


if not bpy.context.preferences.addons.get('rigify'):
    bpy.ops.preferences.addon_enable(module='rigify')
bpy.ops.object.select_all(action='DESELECT')
bpy.ops.object.armature_basic_human_metarig_add()
meta=bpy.context.object;meta.name='MiniBotC_Metarig'
Activate(meta);bpy.ops.object.mode_set(mode='EDIT')
bones=meta.data.edit_bones
for name in ['breast.L','breast.R','pelvis.L','pelvis.R']:
    if name in bones:bones.remove(bones[name])
levels=[.835,.930,1.020,1.100,1.200,1.235,1.257,1.550]
for i in range(7):
    b=bones['spine' if i==0 else f'spine.{i:03d}']
    b.head=(0,.018,levels[i]);b.tail=(0,.018,levels[i+1]);b.roll=0
finger_landmarks=json.loads(bpy.data.objects['Player_Fingers_LOD0']['finger_landmarks'])
for side,prefix,sign in [('L','Left',1),('R','Right',-1)]:
    points={
        'shoulder':((sign*.065,.018,1.182),(sign*.187,.010,1.155)),
        'upper_arm':((sign*.187,.010,1.155),(sign*.269,.031,.981)),
        'forearm':((sign*.269,.031,.981),(sign*.362,.020,.787)),
        'hand':((sign*.362,.020,.787),(sign*.402,.036,.728)),
        'thigh':((sign*.112,.014,.849),(sign*.158,-.012,.545)),
        'shin':((sign*.158,-.012,.545),(sign*.215,.028,.132)),
        'foot':((sign*.215,.028,.132),(sign*.220,-.135,.054)),
        'toe':((sign*.220,-.135,.054),(sign*.220,-.227,.045)),
        'heel.02':((sign*.215-.06,.103,.022),(sign*.215+.06,.103,.022)),
    }
    for stem,(a,b) in points.items():
        bone=bones[f'{stem}.{side}'];bone.head=a;bone.tail=b
        bone.align_roll(Vector((0,1,0)))
    for stem,label in [('f_index','Index'),('f_middle','Middle'),('f_ring','Ring'),('f_pinky','Pinky'),('thumb','Thumb')]:
        thumb_points=[(sign*.350,-.002,.740),(sign*.338,-.010,.701),(sign*.333,-.025,.675),(sign*.332,-.038,.652)]
        for j in range(1,4):
            name=f'{stem}.{j:02d}.{side}'
            b=bones.get(name) or bones.new(name)
            if label=='Thumb':b.head,b.tail=thumb_points[j-1:j+1]
            else:b.head,b.tail=finger_landmarks[f'{prefix}Hand{label}{j}']
            b.parent=bones[f'hand.{side}' if j==1 else f'{stem}.{j-1:02d}.{side}']
            b.use_connect=j>1
            b.align_roll(Vector((-sign,0,0)))
bpy.ops.object.mode_set(mode='OBJECT')
for side in ['L','R']:
    for stem in ['upper_arm','thigh']:
        parameters=meta.pose.bones[f'{stem}.{side}'].rigify_parameters
        parameters.segments=1;parameters.bbones=1;parameters.rotation_axis='x'
    for stem in ['f_index','f_middle','f_ring','f_pinky','thumb']:
        pb=meta.pose.bones[f'{stem}.01.{side}']
        pb.rigify_type='limbs.super_finger'
        pb.rigify_parameters.rotation_axis='x'
Log('Generate Rigify controls')
bpy.ops.pose.rigify_generate()
control=bpy.context.object;control.name='MiniBotC_ControlRig';control.show_in_front=True
for pb in control.pose.bones:
    if 'IK_Stretch' in pb:pb['IK_Stretch']=0.0
    if 'stretch_length' in pb:pb['stretch_length']=1.0
control['two_hand_grip']=0.0
control.id_properties_ui('two_hand_grip').update(min=0,max=1,description='左手 IK を剣の補助グリップへ追従させる')
meta.hide_set(True);meta.hide_render=True
mapping={'Root':'root','Hips':'ORG-spine','Spine':'ORG-spine.001','Chest':'ORG-spine.002',
         'UpperChest':'ORG-spine.003','Neck':'ORG-spine.004','Head':'ORG-spine.006'}
parents={'Root':None,'Hips':'Root','Spine':'Hips','Chest':'Spine','UpperChest':'Chest','Neck':'UpperChest','Head':'Neck'}
for side,prefix in [('L','Left'),('R','Right')]:
    for chain,initial in [([('Shoulder','shoulder'),('Arm','upper_arm'),('ForeArm','forearm'),('Hand','hand')],'UpperChest'),
                          ([('UpLeg','thigh'),('Leg','shin'),('Foot','foot'),('ToeBase','toe')],'Hips')]:
        parent=initial
        for label,stem in chain:
            name=prefix+label;mapping[name]=f'ORG-{stem}.{side}';parents[name]=parent;parent=name
    for stem,label in [('f_index','Index'),('f_middle','Middle'),('f_ring','Ring'),('f_pinky','Pinky'),('thumb','Thumb')]:
        parent=prefix+'Hand'
        for j in range(1,4):
            name=f'{prefix}Hand{label}{j}'
            mapping[name]=f'ORG-{stem}.{j:02d}.{side}';parents[name]=parent;parent=name
data=bpy.data.armatures.new('MiniBotC_Humanoid')
rig=bpy.data.objects.new('MiniBotC_Humanoid',data);scene.collection.objects.link(rig)
Activate(rig);bpy.ops.object.mode_set(mode='EDIT')
for name,source in mapping.items():
    ref=control.data.bones[source]
    b=data.edit_bones.new(name)
    b.head=ref.head_local;b.tail=ref.tail_local
    b.matrix=ref.matrix_local.copy()
    b.use_deform=name!='Root'
    if parents[name]:b.parent=data.edit_bones[parents[name]]
socket=data.edit_bones.new('Socket_Weapon_R')
socket.head=(-.378,.037,.730);socket.tail=(-.378,-.063,.730)
socket.parent=data.edit_bones['RightHand'];socket.use_deform=True
socket.align_roll(Vector((0,0,1)))
bpy.ops.object.mode_set(mode='OBJECT')
for name,source in mapping.items():
    error=max(abs(v) for row in (data.bones[name].matrix_local-control.data.bones[source].matrix_local) for v in row)
    assert error<.00001,f'Bind matrix mismatch: {name}: {error}'
for name,source in mapping.items():
    c=rig.pose.bones[name].constraints.new('COPY_TRANSFORMS');c.target=control;c.subtarget=source
    c.name='Rigify evaluated pose';c.owner_space=c.target_space='POSE'
rig['humanoid_mapping']=json.dumps(mapping)
rig['source_archive']='../Archive/Tripo_HighPoly_20260913.blend'
rig['binding']='Linear blend skinning; maximum four influences on body, rigid articulated finger pieces'
Log('Bind fitted body and independent digits')
body=bpy.data.objects['Player_LOD0']
skin_report=SkinBody(body,rig)
Attach(body,rig);Attach(bpy.data.objects['Player_Fingers_LOD0'],rig)
sword=bpy.data.objects['Sword_LOD0']
sword.data.transform(Matrix.Translation((0,0,-.230)))
sword.parent=rig;sword.parent_type='BONE';sword.parent_bone='Socket_Weapon_R'
bpy.context.view_layer.update()
direction=Vector((-.42,-.82,-.40)).normalized()
sword.matrix_world=Matrix.Translation((-.378,.037,.730)) @ Vector((0,0,1)).rotation_difference(direction).to_matrix().to_4x4()
grip=bpy.data.objects.new('LeftHand_GripTarget',None);scene.collection.objects.link(grip)
grip.empty_display_type='ARROWS';grip.empty_display_size=.065;grip.parent=sword
grip.location=(0,0,-.110)
bpy.context.view_layer.update()
grip.rotation_euler=(sword.matrix_world.inverted().to_quaternion() @ control.pose.bones['hand_ik.L'].matrix.to_quaternion()).to_euler()
constraint=control.pose.bones['hand_ik.L'].constraints.new('COPY_TRANSFORMS')
constraint.name='Two hand grip';constraint.target=grip;constraint.influence=0
driver=constraint.driver_add('influence').driver
driver.expression='grip'
variable=driver.variables.new();variable.name='grip';variable.type='SINGLE_PROP'
variable.targets[0].id=control;variable.targets[0].data_path='["two_hand_grip"]'
rig.hide_set(True);rig.hide_render=True
lod_collection=bpy.data.collections.new('GAME_LOD1')
scene.collection.children.link(lod_collection)
lod_report={}
for src in [body,bpy.data.objects['Player_Fingers_LOD0'],sword]:
    lod=src.copy();lod.data=src.data.copy();lod.name=src.name.replace('LOD0','LOD1')
    lod_collection.objects.link(lod)
    Activate(lod)
    modifier=lod.modifiers.new('Distance LOD','DECIMATE');modifier.ratio=.45;modifier.use_collapse_triangulate=True
    while lod.modifiers.find(modifier.name)>0:bpy.ops.object.modifier_move_up(modifier=modifier.name)
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    lod.hide_set(True);lod.hide_render=True
    lod_report[lod.name]=sum(len(p.vertices)-2 for p in lod.data.polygons)
lod_collection.hide_render=True
for src in [body,bpy.data.objects['Player_Fingers_LOD0'],sword]:src.hide_set(False);src.hide_render=False
Activate(control)
bpy.context.view_layer.update()
scene['pipeline_stage']='rigged'
report={'control_bones':len(control.data.bones),'export_bones':len(rig.data.bones),
        'humanoid_mapping':mapping,'body_skin':skin_report,'lod1_triangles':lod_report,
        'finger_repair':json.loads(bpy.data.objects['Player_Fingers_LOD0']['finger_landmarks']),
        'controls':{'arm_ik':['hand_ik.L','hand_ik.R'],'foot_ik':['foot_ik.L','foot_ik.R'],
                    'grip_property':'MiniBotC_ControlRig["two_hand_grip"]','weapon_socket':'Socket_Weapon_R'}}
(OUT/'RigReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'03_Rigged.blend'),compress=True)
Log('RIG COMPLETE')
