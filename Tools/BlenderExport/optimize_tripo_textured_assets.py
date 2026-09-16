"""Tripo の配色と UV を維持し、Player・剣・おともをゲーム用 LOD に変換する。

Author: Hasegawa Jin
Date: 2026-09-13
"""

import bpy
import bmesh
import hashlib
import json
import math
import numpy as np
import time
from pathlib import Path
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
WORK=ROOT/'GreenWare/Assets/_src/MiniBotC/TripoOptimized'
TEX=ROOT/'GreenWare/Assets/Textures/MiniBotC_Tripo'
REVIEW=ROOT/'Docs/Art/MiniBotC/TripoOptimized'
for path in [WORK,TEX,REVIEW]:path.mkdir(parents=True,exist_ok=True)
START=time.time()
SOURCE_ARCHIVE=bpy.data.filepath
scene=bpy.data.scenes['MiniBotC_GameReady'];bpy.context.window.scene=scene
scene.name='MiniBotC_TripoOptimized'
scene.unit_settings.system='METRIC';scene.unit_settings.scale_length=1;scene.render.fps=30
SPECS=[
    ('Player','tripo_node_5d38e5db-9194-478e-b509-334ea703edf0',1.70,[60000,30000,12000],[2048,1024,512],Vector((0,0,0))),
    ('Sword','tripo_node_e0e7a2df-0349-4932-aeae-fcc26e162871',1.39,[6000,3000,2200],[1024,512,256],Vector((.77,0,.230))),
    ('Companion','tripo_node_ee17b988-b9d3-4bff-93a3-93675fa51b1f',.55,[20000,10000,4000],[1024,512,256],Vector((-.85,0,1.10))),
]


def Log(message):print(f'TRIPO [{time.time()-START:.1f}s] {message}',flush=True)


def Activate(obj):
    if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='DESELECT')
    obj.hide_set(False);obj.select_set(True);bpy.context.view_layer.objects.active=obj


def Triangles(obj):return sum(len(f.vertices)-2 for f in obj.data.polygons)


def Material(src,label):
    original=src.data.materials[0]
    image=next(n.image for n in original.node_tree.nodes if n.type=='TEX_IMAGE' and n.image)
    assert image.packed_file,'Source texture must be packed before optimizing'
    raw=bytes(image.packed_file.data)
    output=TEX/(label+'_BaseColor.jpg')
    output.write_bytes(raw)
    assert hashlib.sha256(output.read_bytes()).hexdigest()==hashlib.sha256(raw).hexdigest()
    # FBX の UV Mapping にある無限大の Z 倍率を取り除く。画像と UV の XY は変更しない。
    mat=original.copy();mat.name=label+'_Tripo_PBR'
    shader=next(n for n in mat.node_tree.nodes if n.type=='BSDF_PRINCIPLED')
    tex=next(n for n in mat.node_tree.nodes if n.type=='TEX_IMAGE' and n.image)
    for node in list(mat.node_tree.nodes):
        if node.type in ['MAPPING','UVMAP']:mat.node_tree.nodes.remove(node)
    uv=mat.node_tree.nodes.new('ShaderNodeUVMap');uv.uv_map=src.data.uv_layers.active.name
    mat.node_tree.links.new(uv.outputs['UV'],tex.inputs['Vector'])
    image.filepath=str(output)
    shader.inputs['Alpha'].default_value=1
    src.data.materials.clear();src.data.materials.append(mat)
    return mat,{'file':str(output),'sha256':hashlib.sha256(raw).hexdigest(),'resolution':list(image.size),'bytes':len(raw),'unchanged_source_bytes':True}


def Quality(high,low,height):
    tree=BVHTree.FromPolygons([v.co for v in low.data.vertices],[p.vertices[:] for p in low.data.polygons],all_triangles=True)
    stride=max(1,len(high.data.vertices)//6000)
    distances=np.array([tree.find_nearest(high.data.vertices[i].co)[3] for i in range(0,len(high.data.vertices),stride)])
    assert np.isfinite(distances).all(),'Invalid surface positions'
    return {'samples':len(distances),'surface_error_p95_m':float(np.quantile(distances,.95)),
            'surface_error_max_m':float(distances.max()),'relative_p95':float(np.quantile(distances,.95)/height)}


def BakeNormal(low,high,label,resolution,height):
    for o in scene.objects:
        if o.type=='MESH':o.hide_render=True
    low.hide_render=False;high.hide_render=False
    source_collection.hide_render=False
    lod_collections[int(low.name[-1])].hide_render=False
    mat=low.data.materials[0].copy();mat.name=label+'_PBR';low.data.materials[0]=mat
    nt=mat.node_tree
    for node in list(nt.nodes):
        if node.type=='NORMAL_MAP' or node.label=='Baked Normal':nt.nodes.remove(node)
    image=bpy.data.images.new(label+'_Normal',width=resolution,height=resolution,alpha=False)
    image.colorspace_settings.name='Non-Color'
    tex=nt.nodes.new('ShaderNodeTexImage');tex.image=image;tex.label='Baked Normal';tex.location=(-400,-300)
    nt.nodes.active=tex
    Activate(low);high.hide_set(False);high.select_set(True)
    scene.render.bake.use_selected_to_active=True
    scene.render.bake.cage_extrusion=height*.012
    scene.render.bake.max_ray_distance=height*.035
    scene.render.bake.normal_space='TANGENT';scene.render.bake.margin=8
    scene.cycles.samples=1
    bpy.context.view_layer.update()
    Log('Bake '+label+' normal '+str(resolution))
    bpy.ops.object.bake(type='NORMAL')
    image.filepath_raw=str(TEX/(label+'_Normal.png'));image.file_format='PNG';image.save()
    shader=next(n for n in nt.nodes if n.type=='BSDF_PRINCIPLED')
    normal=nt.nodes.new('ShaderNodeNormalMap');normal.uv_map=low.data.uv_layers.active.name
    normal.inputs['Strength'].default_value=1
    nt.links.new(tex.outputs['Color'],normal.inputs['Color']);nt.links.new(normal.outputs[0],shader.inputs['Normal'])
    high.hide_render=True;high.hide_set(True);high.select_set(False)
    source_collection.hide_render=True
    scene.render.bake.use_selected_to_active=False
    return image.filepath


source_collection=bpy.data.collections.new('TRIPO_SOURCE');scene.collection.children.link(source_collection)
lod_collections=[]
for level in range(3):
    collection=bpy.data.collections.new('TRIPO_LOD'+str(level));scene.collection.children.link(collection);lod_collections.append(collection)
scene.render.engine='CYCLES';scene.cycles.device='GPU'
pref=bpy.context.preferences.addons['cycles'].preferences
pref.compute_device_type='OPTIX';pref.refresh_devices()
for device in pref.devices:device.use=device.type=='OPTIX'
report={'source_archive':SOURCE_ARCHIVE,'models':{},'textures':'Original Tripo BaseColor bytes preserved; tangent normals baked for each LOD'}
for label,name,height,budgets,resolutions,location in SPECS:
    high=bpy.data.objects[name]
    assert high.type=='MESH' and not high.vertex_groups and len(high.data.materials)==1
    count=Triangles(high)
    for collection in list(high.users_collection):collection.objects.unlink(high)
    source_collection.objects.link(high)
    high.name='SOURCE_'+label+'_Tripo'
    corners=[high.matrix_world@Vector(c) for c in high.bound_box]
    lo=Vector([min(c[k] for c in corners) for k in range(3)]);hi=Vector([max(c[k] for c in corners) for k in range(3)])
    origin=Vector(((lo.x+hi.x)/2,(lo.y+hi.y)/2,lo.z))
    factor=height/(hi.z-lo.z)
    pivot=Vector((0,0,height*.5 if label=='Companion' else .230 if label=='Sword' else 0))
    high.data.transform(Matrix.Translation(-pivot)@Matrix.Scale(factor,4)@Matrix.Translation(-origin)@high.matrix_world)
    high.matrix_world=Matrix.Translation(location);high.data.update()
    for face in high.data.polygons:face.use_smooth=True
    mat,texture=Material(high,label)
    entry={'source_triangles':count,'height_m':height,'pivot':'center' if label=='Companion' else 'grip' if label=='Sword' else 'feet',
           'base_color':texture,'lods':[]}
    previous=high
    for level,budget in enumerate(budgets):
        low=previous.copy();low.data=previous.data.copy();low.name=label+'_LOD'+str(level)
        lod_collections[level].objects.link(low)
        low.data.name=low.name+'_Mesh'
        low['tripo_source']=name;low['lod_level']=level
        Activate(low)
        decimate=low.modifiers.new('Game triangle budget','DECIMATE')
        decimate.ratio=budget/Triangles(previous);decimate.use_collapse_triangulate=True
        decimate.use_symmetry=False
        Log(f'Decimate {low.name}: {Triangles(previous)} -> {budget}')
        bpy.ops.object.modifier_apply(modifier=decimate.name)
        bm=bmesh.new();bm.from_mesh(low.data)
        degenerate=[f for f in bm.faces if f.calc_area()<1e-13]
        if degenerate:bmesh.ops.delete(bm,geom=degenerate,context='FACES')
        loose=[v for v in bm.verts if not v.link_faces]
        if loose:bmesh.ops.delete(bm,geom=loose,context='VERTS')
        bmesh.ops.triangulate(bm,faces=[f for f in bm.faces if len(f.verts)>3])
        bm.to_mesh(low.data);bm.free();low.data.update()
        low.data.validate(verbose=False,clean_customdata=False)
        quality=Quality(high,low,height)
        assert quality['relative_p95']<.01,'Reduction loses too much surface detail: '+low.name+' '+json.dumps(quality)
        entry['lods'].append({'object':low.name,'triangles':Triangles(low),'vertices':len(low.data.vertices),
                              'uv_layers':len(low.data.uv_layers),'materials':len(low.data.materials),'quality':quality})
        Log(low.name+' '+json.dumps(entry['lods'][-1]))
        previous=low
    high.hide_set(True);high.hide_render=True
    report['models'][label]=entry
source_collection.hide_render=True
(REVIEW/'OptimizationReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(WORK/'01_Reduced.blend'),compress=True)

for label,name,height,budgets,resolutions,location in SPECS:
    high=bpy.data.objects['SOURCE_'+label+'_Tripo']
    for level,resolution in enumerate(resolutions):
        low=bpy.data.objects[label+'_LOD'+str(level)]
        path=BakeNormal(low,high,low.name,resolution,height)
        report['models'][label]['lods'][level]['normal_map']=path
        report['models'][label]['lods'][level]['normal_resolution']=resolution
for level,collection in enumerate(lod_collections):
    collection.hide_render=level!=0
    for obj in collection.objects:obj.hide_render=level!=0;obj.hide_set(level!=0)
for obj in source_collection.objects:obj.hide_render=True;obj.hide_set(True)
source_collection.hide_render=True
scene['pipeline_stage']='textured_tripo_optimized'
scene['animation_status']='Meshes optimized; new-model skin binding has not been performed'
scene['optimization_report']='../../../../../Docs/Art/MiniBotC/TripoOptimized/OptimizationReport.json'
(REVIEW/'OptimizationReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(WORK/'02_Baked.blend'),compress=True)
Log('OPTIMIZATION COMPLETE')
