"""Tripo 最適化版を原点付きの FBX/GLB と軽い Blender ファイルへ保存する。"""

import bpy
import hashlib
import json
from pathlib import Path
from mathutils import Matrix, Vector

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'GreenWare/Assets/Models/MiniBotC_Tripo'
OUT.mkdir(parents=True,exist_ok=True)
scene=bpy.data.scenes['MiniBotC_TripoOptimized'];bpy.context.window.scene=scene
if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
report=json.loads((ROOT/'Docs/Art/MiniBotC/TripoOptimized/OptimizationReport.json').read_text(encoding='utf8'))
assert Path(report['source_archive']).is_file(),'Original source archive missing'
for source in list(bpy.data.objects):
    if source.name.startswith('SOURCE_'):
        mesh=source.data;bpy.data.objects.remove(source,do_unlink=True)
        if mesh.users==0:bpy.data.meshes.remove(mesh)
collection=bpy.data.collections.get('TRIPO_SOURCE')
if collection:bpy.data.collections.remove(collection)
for other in list(bpy.data.scenes):
    if other!=scene:bpy.data.scenes.remove(other)

assets=[]
for label in ['Player','Sword','Companion']:
    original=report['models'][label]['base_color']
    assert hashlib.sha256(Path(original['file']).read_bytes()).hexdigest()==original['sha256'],'Original Tripo color changed'
    for level in range(3):
        obj=bpy.data.objects[label+'_LOD'+str(level)]
        before=len(obj.data.polygons)
        obj.data.validate(verbose=False,clean_customdata=False)
        cleaned=before-len(obj.data.polygons)
        assets.append(obj)
        obj.data.uv_layers.active.name='UVMap'
        for mat in obj.data.materials:
            for node in mat.node_tree.nodes:
                if node.type in ['UVMAP','NORMAL_MAP']:node.uv_map='UVMap'
        bpy.ops.object.select_all(action='DESELECT')
        obj.hide_set(False);obj.select_set(True);bpy.context.view_layer.objects.active=obj
        world=obj.matrix_world.copy();obj.matrix_world=Matrix.Identity(4)
        settings=dict(use_selection=True,object_types={'MESH'},global_scale=1,apply_unit_scale=True,
                      apply_scale_options='FBX_SCALE_ALL',use_space_transform=True,axis_forward='-Z',axis_up='Y',
                      add_leaf_bones=False,use_mesh_modifiers=True,mesh_smooth_type='OFF',use_tspace=True,
                      bake_anim=False,bake_anim_step=1.0,path_mode='COPY',embed_textures=False)
        fbx=OUT/(obj.name+'.fbx')
        bpy.ops.export_scene.fbx(filepath=str(fbx),**settings)
        hint={'sourceDcc':'blender','sourceFile':'../../_src/MiniBotC/MiniBotC_TripoOptimized.blend',
              'settings':{k:sorted(v) if isinstance(v,set) else v for k,v in settings.items()}}
        Path(str(fbx)+'.fzhint').write_text(json.dumps(hint,indent=2),encoding='utf8')
        glb=OUT/(obj.name+'.glb')
        bpy.ops.export_scene.gltf(filepath=str(glb),export_format='GLB',use_selection=True,export_animations=False,
                                  export_skins=False,export_materials='EXPORT',export_yup=True)
        obj.matrix_world=world
        obj.hide_set(level!=0);obj.hide_render=level!=0
        entry=report['models'][label]['lods'][level]
        entry['triangles']=sum(len(p.vertices)-2 for p in obj.data.polygons)
        entry['duplicate_faces_removed']=cleaned
        images={n.image for mat in obj.data.materials for n in mat.node_tree.nodes if n.type=='TEX_IMAGE' and n.image}
        entry.update(fbx=str(fbx),glb=str(glb),fbx_bytes=fbx.stat().st_size,glb_bytes=glb.stat().st_size,
                     runtime_textures=[{'name':i.name,'path':i.filepath,'resolution':list(i.size)} for i in images])
        if label in ['Companion','Sword']:entry['base_color_reprojected_from_tripo']=True
        print('EXPORTED '+obj.name,flush=True)

# 旧リグは再利用用に残す。今回の交換データへは含めない。
for obj in scene.objects:
    if obj.type=='ARMATURE' or obj.name=='LeftHand_GripTarget':obj.hide_set(True);obj.hide_render=True
for material in list(bpy.data.materials):
    if material.users==0:bpy.data.materials.remove(material)
for image in list(bpy.data.images):
    if image.users==0 and image.name!='Render Result':bpy.data.images.remove(image)
    elif image.source=='FILE' and image.filepath:image.pack()
for level in range(3):bpy.data.collections['TRIPO_LOD'+str(level)].hide_render=level!=0
scene['source_archive']=report['source_archive']
scene['optimization_report']='../../../../Docs/Art/MiniBotC/TripoOptimized/OptimizationReport.json'
scene['texture_policy']='Original Tripo images retained byte-for-byte; Companion and Sword colors reprojected to repair UV distortion'
scene['animation_status']='New meshes are not skinned; legacy rig is hidden and excluded from export'
bpy.ops.object.select_all(action='DESELECT')
obj=bpy.data.objects['Player_LOD0'];obj.select_set(True);bpy.context.view_layer.objects.active=obj
for area in bpy.context.screen.areas:
    if area.type=='VIEW_3D':
        space=area.spaces.active;space.shading.type='MATERIAL';space.show_region_ui=False
        space.overlay.show_extras=False;space.overlay.show_relationship_lines=False
        space.overlay.show_axis_x=False;space.overlay.show_axis_y=False
        space.region_3d.view_location=Vector((0,0,.9));space.region_3d.view_distance=3.1
        space.region_3d.view_rotation=Vector((1.2,-6,1.2)).to_track_quat('Z','Y')
    elif area.type=='PROPERTIES':area.spaces.active.context='MATERIAL'
blend=ROOT/'GreenWare/Assets/_src/MiniBotC/MiniBotC_TripoOptimized.blend'
bpy.ops.wm.save_as_mainfile(filepath=str(blend),compress=True)
bpy.ops.file.make_paths_relative();bpy.ops.wm.save_as_mainfile(filepath=str(blend),compress=True)
report['optimized_blend']=str(blend);report['optimized_blend_bytes']=blend.stat().st_size
report['total_source_triangles']=sum(m['source_triangles'] for m in report['models'].values())
report['total_triangles_by_lod']=[sum(m['lods'][i]['triangles'] for m in report['models'].values()) for i in range(3)]
report['lod0_reduction_percent']=100*(1-report['total_triangles_by_lod'][0]/report['total_source_triangles'])
(OUT/'AssetReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')
(ROOT/'Docs/Art/MiniBotC/TripoOptimized/OptimizationReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print('EXPORT COMPLETE '+json.dumps({'triangles':report['total_triangles_by_lod'],'reduction':report['lod0_reduction_percent'],'blend_bytes':report['optimized_blend_bytes']}),flush=True)
