"""遠距離メッシュ自身の接線基底で高密度形状の法線をベイクする。"""

import bpy
import importlib.util
from pathlib import Path
from mathutils import Matrix

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
spec=importlib.util.spec_from_file_location('texture_pipeline',ROOT/'GreenWare/Tools/BlenderExport/texture_tripo_minibot.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
scene=bpy.context.scene
scene.cycles.device='GPU'
pref=bpy.context.preferences.addons['cycles'].preferences
pref.compute_device_type='OPTIX';pref.refresh_devices()
for device in pref.devices:device.use=device.type=='OPTIX'
visibility={o.name:(o.hide_render,o.hide_get()) for o in scene.objects}
bpy.data.collections['GAME_LOD1'].hide_render=False
for label in ['Player','Sword']:
    obj=bpy.data.objects[label+'_LOD1']
    for o in scene.objects:o.hide_render=True
    obj.hide_render=False;obj.hide_set(False)
    mat=obj.data.materials[0].copy();mat.name=f'MiniBotC_{label}_LOD1_PBR'
    obj.data.materials[0]=mat
    maps={n.label:n.image for n in mat.node_tree.nodes if n.type=='TEX_IMAGE' and n.image}
    matrix=obj.matrix_world.copy()
    if label=='Sword':obj.matrix_world=Matrix.Translation((0,0,.230))
    bpy.context.view_layer.update()
    maps['Normal']=p.BakeNormal(obj,bpy.data.objects['SOURCE_'+label],mat,f'MiniBotC_{label}_LOD1',2048)
    p.FinalMaterial(mat,maps)
    obj.matrix_world=matrix
for name,(render,viewport) in visibility.items():
    bpy.data.objects[name].hide_render=render;bpy.data.objects[name].hide_set(viewport)
bpy.data.collections['GAME_LOD1'].hide_render=True
bpy.context.view_layer.update()
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'GreenWare/Assets/_src/MiniBotC/GameReady/04_Reviewed.blend'),compress=True)
print('LOD NORMALS BAKED',flush=True)
