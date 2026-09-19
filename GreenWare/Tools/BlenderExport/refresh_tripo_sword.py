"""刀身の発光を面全体へ整え、既存 UV と法線を維持して再ベイクする。"""

import bpy
import importlib.util
from pathlib import Path

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
spec=importlib.util.spec_from_file_location('texture_pipeline',ROOT/'GreenWare/Tools/BlenderExport/texture_tripo_minibot.py')
pipeline=importlib.util.module_from_spec(spec);spec.loader.exec_module(pipeline)
scene=bpy.context.scene
scene.cycles.device='GPU'
pref=bpy.context.preferences.addons['cycles'].preferences
pref.compute_device_type='OPTIX';pref.refresh_devices()
for device in pref.devices:device.use=device.type=='OPTIX'
obj=bpy.data.objects['Sword_LOD0']
for o in scene.objects:o.hide_render=True
obj.hide_render=False
for attr in list(obj.data.color_attributes):obj.data.color_attributes.remove(attr)
pipeline.ColorAttributes(obj,'Sword')
mat=obj.data.materials[0]
normal=bpy.data.images['MiniBotC_Sword_Normal']
maps={channel:pipeline.BakeMap(obj,mat,'MiniBotC_Sword',channel,2048) for channel in ['BaseColor','ORM','Emission']}
maps['Normal']=normal
pipeline.FinalMaterial(mat,maps)
for o in scene.objects:o.hide_render=o.name.startswith('SOURCE_')
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'GreenWare/Assets/_src/MiniBotC/GameReady/02_Textured.blend'),compress=True)
