"""操作リグを含む原本と、選択したゲーム骨格・LOD の交換データを保存する。"""

import bpy
import bmesh
import json
import numpy as np
from pathlib import Path
from mathutils import Matrix, Vector

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
SOURCE=ROOT/'GreenWare/Assets/_src/MiniBotC'
OUT=ROOT/'GreenWare/Assets/Models/MiniBotC_GameReady'
OUT.mkdir(parents=True,exist_ok=True)
scene=bpy.data.scenes['MiniBotC_GameReady'];bpy.context.window.scene=scene
rig=bpy.data.objects['MiniBotC_Humanoid']
control=bpy.data.objects['MiniBotC_ControlRig']
channel_images={}
from io_scene_gltf2.blender.com.material_helpers import create_settings_group
gltf_settings=create_settings_group('glTF Material Output')
for mat in {m for o in scene.objects if o.type=='MESH' and 'LOD' in o.name for m in o.data.materials}:
    nt=mat.node_tree
    sep=next(n for n in nt.nodes if n.type=='SEPARATE_COLOR')
    node=nt.nodes.new('ShaderNodeGroup');node.node_tree=gltf_settings
    node.label='glTF AO';node.location=(120,-650)
    nt.links.new(sep.outputs[0],node.inputs['Occlusion'])


def FbxChannels(materials):
    changes=[]
    for mat in materials:
        nt=mat.node_tree
        shader=next(n for n in nt.nodes if n.type=='BSDF_PRINCIPLED')
        for socket_name,index in [('Roughness',1),('Metallic',2)]:
            socket=shader.inputs[socket_name]
            original=socket.links[0].from_socket
            sep=original.node
            orm=sep.inputs[0].links[0].from_node.image
            key=(orm.filepath,socket_name)
            if key not in channel_images:
                data=np.empty(len(orm.pixels),np.float32);orm.pixels.foreach_get(data);data=data.reshape(-1,4)
                result=np.ones_like(data);result[:,:3]=data[:,index:index+1]
                image=bpy.data.images.new(Path(orm.filepath).stem.replace('_ORM','_'+socket_name),width=orm.size[0],height=orm.size[1],alpha=False)
                image.colorspace_settings.name='Non-Color';image.pixels.foreach_set(result.reshape(-1))
                image.filepath_raw=str(OUT/(image.name+'.png'));image.file_format='PNG';image.save()
                channel_images[key]=image
            node=nt.nodes.new('ShaderNodeTexImage');node.image=channel_images[key]
            nt.links.new(node.outputs['Color'],socket)
            changes.append((nt,socket,original,node))
    return changes


def Select(objects):
    if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:obj.hide_set(False);obj.select_set(True)
    bpy.context.view_layer.objects.active=objects[0]


def Triangles(obj):
    return sum(len(p.vertices)-2 for p in obj.data.polygons)


def Export(label,objects):
    Select(objects)
    path=OUT/(label+'.fbx')
    settings=dict(use_selection=True,object_types={'MESH','ARMATURE'},global_scale=1,
                  apply_unit_scale=True,apply_scale_options='FBX_SCALE_ALL',use_space_transform=True,
                  axis_forward='-Z',axis_up='Y',add_leaf_bones=False,use_armature_deform_only=False,
                  use_mesh_modifiers=True,mesh_smooth_type='OFF',use_tspace=True,
                  bake_anim=False,bake_anim_step=1.0,path_mode='COPY',embed_textures=False)
    materials={m for o in objects if o.type=='MESH' for m in o.data.materials}
    changes=FbxChannels(materials)
    bpy.ops.export_scene.fbx(filepath=str(path),**settings)
    for nt,socket,original,node in changes:
        nt.nodes.remove(node);nt.links.new(original,socket)
    hint={'sourceDcc':'blender','sourceFile':'../../_src/MiniBotC/MiniBotC_GameReady.blend',
          'settings':{k:sorted(v) if isinstance(v,set) else v for k,v in settings.items()}}
    Path(str(path)+'.fzhint').write_text(json.dumps(hint,indent=2),encoding='utf8')
    bpy.ops.export_scene.gltf(filepath=str(OUT/(label+'.glb')),export_format='GLB',
                              use_selection=True,export_animations=False,export_apply=False,
                              export_skins=True,export_all_influences=False,
                              export_def_bones=False,export_materials='EXPORT',export_yup=True)
    return {'triangles':sum(Triangles(o) for o in objects if o.type=='MESH'),
            'bones':sum(len(o.data.bones) for o in objects if o.type=='ARMATURE'),
            'fbx_bytes':path.stat().st_size,'glb_bytes':(OUT/(label+'.glb')).stat().st_size}


# 高密度の原本は専用アーカイブに保持し、毎回開く操作ファイルから外す。
for old in list(bpy.data.scenes):
    if old!=scene:bpy.data.scenes.remove(old)
for obj in list(bpy.data.objects):
    if obj.name.startswith(('SOURCE_','tripo_node_')):bpy.data.objects.remove(obj,do_unlink=True)
if 'SOURCE_HighPoly' in bpy.data.collections:bpy.data.collections.remove(bpy.data.collections['SOURCE_HighPoly'])
for mesh in list(bpy.data.meshes):
    if mesh.users==0:bpy.data.meshes.remove(mesh)
for obj in scene.objects:
    if obj.type=='MESH' and 'LOD' in obj.name:
        bm=bmesh.new();bm.from_mesh(obj.data)
        bmesh.ops.triangulate(bm,faces=[f for f in bm.faces if len(f.verts)>3])
        bm.to_mesh(obj.data);bm.free();obj.data.update()
        if obj.vertex_groups:
            Select([obj])
            bpy.ops.object.vertex_group_limit_total(group_select_mode='ALL',limit=4)
            bpy.ops.object.vertex_group_normalize_all(group_select_mode='ALL',lock_active=False)
        for attr in list(obj.data.color_attributes):obj.data.color_attributes.remove(attr)
for image in list(bpy.data.images):
    if image.users==0 and image.name!='Render Result':bpy.data.images.remove(image)
    elif image.source=='FILE' and image.filepath:
        image.filepath=bpy.path.abspath(image.filepath)
        image.pack()

rig.data.pose_position='REST'
report={}
for level in [0,1]:
    meshes=[bpy.data.objects[f'Player_LOD{level}'],bpy.data.objects[f'Player_Fingers_LOD{level}']]
    report[f'MiniBotC_Player_LOD{level}']=Export(f'MiniBotC_Player_LOD{level}',[rig]+meshes)
    for mesh in meshes:mesh.hide_set(level==1)
    sword=bpy.data.objects[f'Sword_LOD{level}']
    parent,parent_type,parent_bone=sword.parent,sword.parent_type,sword.parent_bone
    matrix=sword.matrix_world.copy()
    sword.parent=None;sword.matrix_world=Matrix.Identity(4)
    report[f'MiniBotC_Sword_LOD{level}']=Export(f'MiniBotC_Sword_LOD{level}',[sword])
    sword.parent=parent;sword.parent_type=parent_type;sword.parent_bone=parent_bone;sword.matrix_world=matrix
    sword.hide_set(level==1)
rig.data.pose_position='POSE'
rig.hide_set(True)
rig['source_archive']='Archive/Tripo_HighPoly_20260913.blend'

report['texture_layout']={'BaseColor':'sRGB','Emission':'sRGB, strength 2.5',
                          'ORM':'Non-Color: R=AO, G=Roughness, B=Metallic','Normal':'Tangent, OpenGL +Y, strength 0.7'}
report['source_archive']='../../_src/MiniBotC/Archive/Tripo_HighPoly_20260913.blend'
report['engine_validation']='FBX/GLB round-trip validation in Blender; FBZZ runtime not tested'
(OUT/'AssetReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')

scene['pipeline_stage']='game_ready'
scene['source_archive']='Archive/Tripo_HighPoly_20260913.blend'
scene['asset_report']='../../Models/MiniBotC_GameReady/AssetReport.json'
scene['animation_status']='New motions not authored; rig pose checks only'
Select([control])
bpy.ops.object.mode_set(mode='POSE')
bpy.ops.pose.select_all(action='DESELECT')
control.data.bones.active=control.data.bones['hand_ik.R']
control.pose.bones['hand_ik.R'].select=True
control.pose.bones['root'].custom_shape_scale_xyz=(.65,.65,.65)
for collection in control.data.collections_all:
    collection.is_visible=collection.name not in ['ORG','MCH','DEF'] and '(FK)' not in collection.name and '(Tweak)' not in collection.name
for area in bpy.context.screen.areas:
    if area.type=='VIEW_3D':
        area.spaces.active.shading.type='MATERIAL'
        area.spaces.active.show_region_ui=True
        area.spaces.active.overlay.show_relationship_lines=False
        area.spaces.active.overlay.show_extras=False
        area.spaces.active.overlay.show_axis_x=False
        area.spaces.active.overlay.show_axis_y=False
        area.spaces.active.region_3d.view_distance=2.75
        area.spaces.active.region_3d.view_location=Vector((-.10,0,.88))
        area.spaces.active.region_3d.view_rotation=Vector((2.5,-5,1.3)).to_track_quat('Z','Y')
    elif area.type=='DOPESHEET_EDITOR':area.spaces.active.dopesheet.show_only_selected=True
bpy.ops.wm.save_as_mainfile(filepath=str(SOURCE/'MiniBotC_GameReady.blend'),compress=True)
bpy.ops.file.make_paths_relative()
bpy.ops.wm.save_as_mainfile(filepath=str(SOURCE/'MiniBotC_GameReady.blend'),compress=True)
print('EXPORT COMPLETE '+json.dumps(report),flush=True)
