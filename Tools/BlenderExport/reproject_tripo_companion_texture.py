"""軽量化で歪んだおともの目の UV を、Tripo 原本の色投影で補正する。"""

import bpy
import sys
from pathlib import Path

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
scene=bpy.data.scenes['MiniBotC_TripoOptimized'];bpy.context.window.scene=scene
scene.render.engine='CYCLES';scene.cycles.device='GPU';scene.cycles.samples=1
pref=bpy.context.preferences.addons['cycles'].preferences
pref.compute_device_type='OPTIX';pref.refresh_devices()
for d in pref.devices:d.use=d.type=='OPTIX'
label='Sword' if '--sword' in sys.argv else 'Companion'
height=1.39 if label=='Sword' else .55
high=bpy.data.objects['SOURCE_'+label+'_Tripo']
nt=high.data.materials[0].node_tree
shader=next(n for n in nt.nodes if n.type=='BSDF_PRINCIPLED')
output=next(n for n in nt.nodes if n.type=='OUTPUT_MATERIAL')
emit=nt.nodes.new('ShaderNodeEmission')
nt.links.new(shader.inputs['Base Color'].links[0].from_socket,emit.inputs['Color'])
nt.links.new(emit.outputs[0],output.inputs['Surface'])
for level,resolution in enumerate([2048,1024,512]):
    if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
    for o in scene.objects:
        if o.type=='MESH':o.hide_render=True
    low=bpy.data.objects[label+'_LOD'+str(level)]
    for c in [bpy.data.collections['TRIPO_SOURCE'],bpy.data.collections['TRIPO_LOD'+str(level)]]:c.hide_render=False
    bpy.ops.object.select_all(action='DESELECT')
    for o in [low,high]:o.hide_render=False;o.hide_set(False);o.select_set(True)
    bpy.context.view_layer.objects.active=low
    nodes=low.data.materials[0].node_tree.nodes
    image=bpy.data.images.new(low.name+'_BaseColor',width=resolution,height=resolution,alpha=False)
    image.colorspace_settings.name='sRGB'
    target=nodes.new('ShaderNodeTexImage');target.image=image;nodes.active=target
    scene.render.bake.use_selected_to_active=True;scene.render.bake.cage_extrusion=height*.012
    scene.render.bake.max_ray_distance=height*.035;scene.render.bake.margin=8
    bpy.ops.object.bake(type='EMIT')
    image.filepath_raw=str(ROOT/'GreenWare/Assets/Textures/MiniBotC_Tripo'/(low.name+'_BaseColor.png'))
    image.file_format='PNG';image.save()
    bsdf=next(n for n in nodes if n.type=='BSDF_PRINCIPLED')
    bsdf.inputs['Base Color'].links[0].from_node.image=image
    nodes.remove(target)
    print('COLOR REPROJECTED '+low.name,flush=True)
nt.nodes.remove(emit);nt.links.new(shader.outputs[0],output.inputs['Surface'])
scene.render.bake.use_selected_to_active=False
for o in scene.objects:
    if o.type=='MESH':
        o.hide_render=not o.name.endswith('LOD0')
        if o.name in bpy.context.view_layer.objects:o.hide_set(not o.name.endswith('LOD0'))
for c in bpy.data.collections:
    if c.name.startswith('TRIPO_'):c.hide_render=c.name!='TRIPO_LOD0'
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'GreenWare/Assets/_src/MiniBotC/TripoOptimized/02_Baked.blend'),compress=True)
