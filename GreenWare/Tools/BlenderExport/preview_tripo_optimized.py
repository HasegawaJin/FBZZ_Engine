"""同じ照明とカメラで原本・各 LOD の見た目を比較する。"""

import bpy
import json
import sys
import numpy as np
from pathlib import Path
from mathutils import Vector

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'Docs/Art/MiniBotC/TripoOptimized'
scene=bpy.data.scenes['MiniBotC_TripoOptimized'];bpy.context.window.scene=scene
scene.render.engine='CYCLES';scene.cycles.samples=24;scene.cycles.use_denoising=True;scene.cycles.device='GPU'
pref=bpy.context.preferences.addons['cycles'].preferences
pref.compute_device_type='OPTIX';pref.refresh_devices()
for d in pref.devices:d.use=d.type=='OPTIX'
scene.render.resolution_percentage=100;scene.render.image_settings.file_format='PNG'
cam=bpy.data.objects['Preview_Camera'];scene.camera=cam;cam.data.type='ORTHO'
scene.render.film_transparent=False
background=next(n for n in scene.world.node_tree.nodes if n.type=='BACKGROUND')
background.inputs['Color'].default_value=(.12,.145,.18,1);background.inputs['Strength'].default_value=.4


def Show(level,label=None):
    for c in bpy.data.collections:
        if c.name.startswith('TRIPO_'):c.hide_render=False
    for o in scene.objects:
        if o.type=='MESH':
            wanted=(o.name.startswith('SOURCE_') if level=='High' else o.name.endswith('LOD'+str(level)))
            o.hide_render=not (wanted and (label is None or label in o.name))


def Render(name,location,target,scale,size=(1600,1200)):
    cam.location=location;cam.rotation_euler=(Vector(target)-cam.location).to_track_quat('-Z','Y').to_euler()
    cam.data.ortho_scale=scale*max(1,size[0]/size[1]);scene.render.resolution_x,scene.render.resolution_y=size
    scene.render.filepath=str(OUT/(name+'.png'))
    bpy.context.view_layer.update();bpy.ops.render.render(write_still=True)
    print('PREVIEW '+name,flush=True)


if '--back-only' in sys.argv:
    for level in ['High',0]:
        Show(level)
        Render('Back_'+str(level),(0,5,1.0),(0,0,.88),1.95)
    raise SystemExit(0)

for level in ['High',0,1,2]:
    Show(level)
    Render('Front_'+str(level),(0,-5,.88),(0,0,.88),1.95)
Show(0)
Render('Overview',(2.4,-6,2.45),(0,0,.90),2.20)
for label in ['Player','Companion','Sword']:
    Show(0,label)
    obj=bpy.data.objects[label+'_LOD0']
    points=[obj.matrix_world@Vector(c) for c in obj.bound_box]
    center=sum(points,Vector())/8
    height=max(p.z for p in points)-min(p.z for p in points)
    Render(label+'_Detail',center+Vector((height*1.3,-height*4,height*.55)),center,height*1.18,(1000,1200))

# 色だけの比較で、法線・陰影の変化と UV の崩れを区別する。
changes=[]
for mat in {m for o in scene.objects if o.type=='MESH' and (o.name.startswith('SOURCE_') or 'LOD' in o.name) for m in o.data.materials}:
    nt=mat.node_tree;shader=next(n for n in nt.nodes if n.type=='BSDF_PRINCIPLED')
    output=next(n for n in nt.nodes if n.type=='OUTPUT_MATERIAL')
    original=output.inputs['Surface'].links[0].from_socket
    node=nt.nodes.new('ShaderNodeEmission')
    nt.links.new(shader.inputs['Base Color'].links[0].from_socket,node.inputs['Color'])
    nt.links.new(node.outputs[0],output.inputs['Surface']);changes.append((nt,output,original,node))
scene.cycles.samples=1;scene.cycles.use_denoising=False;scene.render.film_transparent=True
arrays={}
for level in ['High',0,1,2]:
    Show(level);Render('ColorCheck_'+str(level),(0,-5,.88),(0,0,.88),1.95)
    image=bpy.data.images.load(scene.render.filepath,check_existing=False)
    data=np.empty(len(image.pixels),np.float32);image.pixels.foreach_get(data)
    arrays[level]=data.reshape(-1,4);bpy.data.images.remove(image)
report={}
for level in [0,1,2]:
    mask=(arrays['High'][:,3]>.99)&(arrays[level][:,3]>.99)
    difference=np.abs(arrays['High'][mask,:3]-arrays[level][mask,:3])
    silhouette=(arrays['High'][:,3]>.5)!=(arrays[level][:,3]>.5)
    entry={'rgb_mae_normalized_png':float(difference.mean()),'rgb_absolute_error_p95':float(np.quantile(difference,.95)),
           'silhouette_difference_fraction':float(silhouette.sum()/max(1,(arrays['High'][:,3]>.5).sum()))}
    report['LOD'+str(level)]=entry
    assert entry['rgb_mae_normalized_png']<.05,'UV/color deviation too large: '+str(entry)
for nt,output,original,node in changes:
    nt.nodes.remove(node);nt.links.new(original,output.inputs['Surface'])
scene.cycles.samples=24;scene.cycles.use_denoising=True;scene.render.film_transparent=False
Show(0)
for c in bpy.data.collections:
    if c.name.startswith('TRIPO_'):c.hide_render=c.name!='TRIPO_LOD0'
(OUT/'TextureValidation.json').write_text(json.dumps(report,indent=2),encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'GreenWare/Assets/_src/MiniBotC/TripoOptimized/03_Reviewed.blend'),compress=True)
print('APPEARANCE VERIFIED '+json.dumps(report),flush=True)
