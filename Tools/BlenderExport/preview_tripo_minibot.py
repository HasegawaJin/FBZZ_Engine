"""ゲーム用モデルを材質とポーズの両面から確認する。"""

import bpy
import json
import math
import numpy as np
from pathlib import Path
from mathutils import Vector, Matrix

ROOT=Path(r"C:\Users\jinhs\Downloads\FBZZ_Engine")
OUT=ROOT/'Docs/Art/MiniBotC/GameReady'
OUT.mkdir(parents=True,exist_ok=True)
scene=bpy.data.scenes['MiniBotC_GameReady'];bpy.context.window.scene=scene
control=bpy.data.objects['MiniBotC_ControlRig']
body=bpy.data.objects['Player_LOD0']
fingers=bpy.data.objects['Player_Fingers_LOD0']
sword=bpy.data.objects['Sword_LOD0']
studio=bpy.data.collections.new('STUDIO_Preview');scene.collection.children.link(studio)


def Link(name,data):
    obj=bpy.data.objects.new(name,data);studio.objects.link(obj);return obj


def Aim(obj,target):
    obj.rotation_euler=(Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()


cam=Link('Preview_Camera',bpy.data.cameras.new('Preview_Camera'))
cam.data.type='ORTHO';scene.camera=cam
for name,location,power,size,color in [('Key',(-3,-4,5),700,4,(.88,.94,1)),
                                     ('Fill',(3,-2,3),400,3,(1,.94,.84)),
                                     ('Rim',(1,3,4),1000,3,(.75,.88,1))]:
    lamp=Link('Preview_'+name,bpy.data.lights.new('Preview_'+name,'AREA'))
    lamp.location=location;lamp.data.energy=power;lamp.data.shape='DISK';lamp.data.size=size;lamp.data.color=color;Aim(lamp,(0,0,.9))
world=scene.world
background=next(n for n in world.node_tree.nodes if n.type=='BACKGROUND')
background.inputs['Color'].default_value=(.16,.19,.23,1);background.inputs['Strength'].default_value=.4
scene.render.engine='CYCLES';scene.cycles.samples=24;scene.cycles.use_denoising=True
scene.cycles.device='GPU'
preferences=bpy.context.preferences.addons['cycles'].preferences
try:
    preferences.compute_device_type='OPTIX';preferences.refresh_devices()
    for d in preferences.devices:d.use=d.type=='OPTIX'
except (TypeError,RuntimeError):scene.cycles.device='CPU'
scene.render.resolution_percentage=100
scene.render.image_settings.file_format='PNG'
scene.render.film_transparent=False


def Render(name,location,target,scale,size=(1000,1400)):
    cam.location=location;Aim(cam,target);cam.data.ortho_scale=scale
    scene.render.resolution_x,scene.render.resolution_y=size
    scene.render.filepath=str(OUT/(name+'.png'))
    bpy.context.view_layer.update()
    bpy.ops.render.render(write_still=True)
    print('PREVIEW',name,flush=True)


def Vertices(obj):
    dg=bpy.context.evaluated_depsgraph_get();ev=obj.evaluated_get(dg)
    array=np.empty(len(ev.data.vertices)*3,np.float32);ev.data.vertices.foreach_get('co',array)
    return array.reshape(-1,3)


base=Vertices(body).copy()
finger_base=Vertices(fingers).copy()
bind_errors={}
for obj in [body,fingers]:
    raw=np.array([v.co[:] for v in obj.data.vertices])
    bind_errors[obj.name]=float(np.linalg.norm(Vertices(obj)-raw,axis=1).max())
    assert bind_errors[obj.name]<.0001, f'Rest bind changes mesh: {obj.name}: {bind_errors[obj.name]}'
rest={p.name:p.matrix_basis.copy() for p in control.pose.bones}
properties={p.name:{'IK_FK':p['IK_FK']} for p in control.pose.bones if 'IK_FK' in p}
print('CONTROLS',json.dumps({'ik_fk':properties,'finger_controls':[p.name for p in control.pose.bones if p.name.startswith('f_index')]}),flush=True)
sword.hide_render=True
Render('Textured_Front',(0,-4,.85),(0,0,.85),1.88)
Render('Textured_Face',(0,-3,1.40),(0,0,1.40),.60,(1100,1100))


def Reset():
    for p in control.pose.bones:p.matrix_basis=rest[p.name]
    for name,props in properties.items():
        for key,value in props.items():control.pose.bones[name][key]=value
    bpy.context.view_layer.update()


def Rotate(name,xyz):
    pb=control.pose.bones[name];pb.rotation_mode='XYZ';pb.rotation_euler=xyz


def Metrics(name):
    now=Vertices(body)
    movement=np.linalg.norm(now-base,axis=1)
    edges=np.array([e.vertices[:] for e in body.data.edges])
    old=np.linalg.norm(base[edges[:,0]]-base[edges[:,1]],axis=1)
    new=np.linalg.norm(now[edges[:,0]]-now[edges[:,1]],axis=1)
    ratio=new/np.maximum(old,.00001)
    result={'finite':bool(np.isfinite(now).all()),'moved_vertices':int(np.sum(movement>.001)),
            'maximum_displacement_m':float(movement.max()),'edge_stretch_p99':float(np.quantile(ratio,.99)),
            'edge_stretch_max':float(ratio.max())}
    assert result['finite'] and result['moved_vertices']>100,'Pose does not deform correctly'
    assert result['edge_stretch_p99']<2.5,'Excessive widespread mesh stretching'
    return result


checks={'RestBindMaximumError_m':bind_errors}
for side in ['L','R']:
    control.pose.bones[f'upper_arm_parent.{side}']['IK_FK']=1
Rotate('upper_arm_fk.L',(0,0,-.75));Rotate('upper_arm_fk.R',(0,0,.75))
Rotate('forearm_fk.L',(-1.15,0,0));Rotate('forearm_fk.R',(-1.15,0,0))
bpy.context.view_layer.update();checks['Arms']=Metrics('Arms')
Render('Pose_Arms',(2.8,-4,1.9),(0,0,.90),2.0,(1200,1200))
Reset()
control.pose.bones['torso'].location.z=-.15
bpy.context.view_layer.update();checks['Crouch']=Metrics('Crouch')
checks['Crouch']['foot_delta_m']={s:float((control.pose.bones[f'ORG-foot.{s}'].head-control.data.bones[f'ORG-foot.{s}'].head_local).length) for s in ['L','R']}
assert max(checks['Crouch']['foot_delta_m'].values())<.001,'Foot IK drift'
Render('Pose_Crouch',(2.8,-4,1.7),(0,0,.78),1.85,(1200,1200))
Reset()
for side in ['L','R']:
    for stem in ['f_index','f_middle','f_ring','f_pinky']:
        for j in range(1,4):
            name=f'{stem}.{j:02d}.{side}'
            if name in control.pose.bones:Rotate(name,(.65,0,0))
bpy.context.view_layer.update()
finger_deformed=Vertices(fingers)
checks['Fingers']={'finite':bool(np.isfinite(finger_deformed).all()),'vertex_count':len(finger_deformed),
                  'moved_vertices':int(np.sum(np.linalg.norm(finger_deformed-finger_base,axis=1)>.001))}
assert checks['Fingers']['finite'] and checks['Fingers']['moved_vertices']>100,'Finger controls do not deform fingers'
Render('Pose_Hand',(-1.0,-2.8,.95),(-.375,.02,.73),.38,(1100,1100))
Reset();sword.hide_render=False
Render('Hero',(3.0,-5,2.1),(-.18,-.12,.87),2.35,(1400,1400))
(OUT/'PoseValidation.json').write_text(json.dumps(checks,indent=2),encoding='utf8')
scene['preview_validation']='Arms, IK crouch, finger curl; see PoseValidation.json'
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'GreenWare/Assets/_src/MiniBotC/GameReady/04_Reviewed.blend'),compress=True)
print('PREVIEW COMPLETE',flush=True)
