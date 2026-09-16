"""制作開始用 Blender、スキン付き全 LOD、可動確認クリップを出力する。"""
import bpy,json,sys,numpy as np
from pathlib import Path
from mathutils import Vector,Matrix
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'GreenWare/Assets/Models/MiniBotC_AnimationReady';OUT.mkdir(parents=True,exist_ok=True)
ART=ROOT/'Docs/Art/MiniBotC/AnimationReady'
scene=bpy.context.scene;rig=bpy.data.objects['MiniBotC_Humanoid'];control=bpy.data.objects['MiniBotC_ControlRig'];comp=bpy.data.objects['Companion_Rig']
if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
report={'models':{},'fps':30,'player_bones':54,'companion_bones':4,'runtime_tested':False}
for model in ['Player','Sword','Companion']:
    for level in range(3):
        meshes=[bpy.data.objects[f'{model}_LOD{level}']]
        if model=='Player':meshes.append(bpy.data.objects[f'Player_Fingers_LOD{level}'])
        arm=rig if model=='Player' else comp if model=='Companion' else None
        objects=([arm] if arm else [])+meshes
        hidden={o:o.hide_get() for o in objects};basis={o:o.matrix_basis.copy() for o in objects}
        parents={o:(o.parent,o.parent_type,o.parent_bone,o.matrix_parent_inverse.copy()) for o in meshes}
        if arm:arm.data.pose_position='REST';arm.matrix_world=Matrix.Identity(4)
        for mesh in meshes:
            if not arm:mesh.parent=None
            mesh.matrix_world=Matrix.Identity(4)
            influences=[sum(g.weight>0 for g in v.groups) for v in mesh.data.vertices]
            if arm:
                assert min(influences)>0 and max(influences)<=4
                assert max(abs(sum(g.weight for g in v.groups)-1) for v in mesh.data.vertices)<1e-5
        bpy.ops.object.select_all(action='DESELECT')
        for o in objects:o.hide_set(False);o.select_set(True)
        bpy.context.view_layer.objects.active=objects[0];bpy.context.view_layer.update()
        label=f'{model}_LOD{level}';fbx=OUT/(label+'.fbx')
        settings=dict(use_selection=True,object_types={'MESH','ARMATURE'},global_scale=1,
            apply_unit_scale=True,apply_scale_options='FBX_SCALE_ALL',use_space_transform=True,axis_forward='-Z',axis_up='Y',
            add_leaf_bones=False,use_armature_deform_only=False,use_mesh_modifiers=True,mesh_smooth_type='OFF',use_tspace=True,
            bake_anim=False,bake_anim_step=1.0,path_mode='COPY',embed_textures=False)
        bpy.ops.export_scene.fbx(filepath=str(fbx),**settings)
        hint={'sourceDcc':'blender','sourceFile':'../../_src/MiniBotC/MiniBotC_AnimationReady.blend','settings':{k:sorted(v) if isinstance(v,set) else v for k,v in settings.items()}}
        Path(str(fbx)+'.fzhint').write_text(json.dumps(hint,indent=2),encoding='utf8')
        bpy.ops.export_scene.gltf(filepath=str(OUT/(label+'.glb')),export_format='GLB',use_selection=True,
            export_animations=False,export_apply=False,export_skins=arm is not None,export_all_influences=False,
            export_def_bones=False,export_materials='EXPORT',export_yup=True)
        report['models'][label]={'triangles':sum(len(p.vertices)-2 for mesh in meshes for p in mesh.data.polygons),
            'meshes':[m.name for m in meshes],'bones':len(arm.data.bones) if arm else 0,'materials':len({m for mesh in meshes for m in mesh.data.materials})}
        if arm:arm.matrix_basis=basis[arm];arm.data.pose_position='POSE'
        for mesh in meshes:
            mesh.parent,mesh.parent_type,mesh.parent_bone,mesh.matrix_parent_inverse=parents[mesh];mesh.matrix_basis=basis[mesh]
        for o in objects:o.hide_set(hidden[o])
        bpy.context.view_layer.update()
        print('EXPORTED '+label,flush=True)

for label in ['Player','Player_Fingers','Sword','Companion']:
    expected=bpy.data.objects[f'{label}_LOD0'].matrix_world
    for level in [1,2]:
        actual=bpy.data.objects[f'{label}_LOD{level}'].matrix_world
        assert max(abs(actual[r][c]-expected[r][c]) for r in range(4) for c in range(4))<1e-6,(label,level)

old_action=control.animation_data.action
control.animation_data.action=bpy.data.actions['DEMO_RigCheck']
samples={}
for frame in [1,16,31,46,61,76,91]:
    scene.frame_set(frame);bpy.context.view_layer.update()
    for name in ['Player_LOD0','Player_Fingers_LOD0']:
        obj=bpy.data.objects[name];evaluated=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=evaluated.to_mesh()
        samples[f'{frame}_{name}']=np.array([evaluated.matrix_world@v.co for v in mesh.vertices]);evaluated.to_mesh_clear()
np.savez_compressed(ROOT/'GreenWare/Assets/_src/MiniBotC/AnimationReady/ClipReference.npz',**samples)
control.animation_data.action=old_action;scene.frame_set(1)
sys.path.insert(0,str(ROOT/'Tools/BlenderExport'))
from export_animation_ready_clip import export_clip
report['demo_clip']=export_clip(OUT/'Animations/DEMO_RigCheck.fbx',action=bpy.data.actions['DEMO_RigCheck'],start=1,end=91)

for label in ['Player','Player_Fingers','Sword','Companion']:
    for level in range(3):
        o=bpy.data.objects[f'{label}_LOD{level}'];o.hide_set(level!=0);o.hide_render=level!=0
rig.hide_set(True);rig.hide_render=True;bpy.data.objects['MiniBotC_Metarig'].hide_set(True)
control.hide_set(False);comp.hide_set(False)
for o in scene.objects:
    if o.type in {'CAMERA','LIGHT'}:o.hide_set(True)
finger_collection=control.data.collections.get('Fingers') or control.data.collections.new('Fingers')
for p in control.pose.bones:
    if p.name.startswith(('f_index.','f_middle.','f_ring.','f_pinky.','thumb.')):
        finger_collection.assign(p.bone);p.bone.color.palette='THEME04'
for collection in control.data.collections_all:
    collection.is_visible=collection.name not in ['ORG','MCH','DEF'] and '(FK)' not in collection.name and '(Tweak)' not in collection.name
finger_collection.is_visible=True
for p in comp.pose.bones:p.bone.color.palette='THEME03'
for image in bpy.data.images:
    if image.users and image.source=='FILE':image.pack()
helper=ROOT/'Tools/BlenderExport/export_animation_ready_clip.py'
text=bpy.data.texts.get('Export_Active_Action.py') or bpy.data.texts.new('Export_Active_Action.py');text.clear();text.write(helper.read_text(encoding='utf8'))
guide=bpy.data.texts.get('START_HERE.txt') or bpy.data.texts.new('START_HERE.txt');guide.clear()
guide.write('MiniBotC Animation Ready\n\nPose Mode: MiniBotC_ControlRig\nHands: hand_ik.L/R  Feet: foot_ik.L/R\nBody: torso, chest, hips, head; root moves the character.\nFingers: f_index/f_middle/f_ring/f_pinky/thumb; local X bends joints.\nObject custom property two_hand_grip: 0 = free left hand, 1 = left hand follows sword.\nSword_Control changes the grip offset.\nCompanion_Rig: Root = travel, Body = hover/tilt, Pod.L/R = pods.\n\nPlayer_NewAction has a rest key on frame 1. Duplicate/rename in Action Editor.\nDEMO_RigCheck contains diagnostic poses on frames 1,16,31,46,61,76,91.\nText Editor > Export_Active_Action.py > Run Script writes the active Action as a baked FBX.\nExport includes the game skeleton, body and fingers, never Rigify control bones.\n')
scene['pipeline_stage']='animation_ready';scene['animation_status']='Skinned LODs; Rigify controls; blank starter and diagnostic Action; baked FBX export'
scene['asset_report']='../../Models/MiniBotC_AnimationReady/AssetReport.json'
scene['texture_policy']='Tripo textures retained; articulated finger surfaces sample original Tripo colors'
for marker in list(scene.timeline_markers):scene.timeline_markers.remove(marker)
bpy.ops.object.select_all(action='DESELECT');control.select_set(True);bpy.context.view_layer.objects.active=control;bpy.ops.object.mode_set(mode='POSE')
bpy.ops.pose.select_all(action='DESELECT');control.data.bones.active=control.data.bones['hand_ik.R'];control.pose.bones['hand_ik.R'].select=True
for area in bpy.context.screen.areas:
    if area.type=='VIEW_3D':
        space=area.spaces.active;space.shading.type='MATERIAL';space.show_region_ui=True
        space.overlay.show_extras=True;space.overlay.show_relationship_lines=False
        space.region_3d.view_location=Vector((-.12,-.15,.86));space.region_3d.view_distance=3.5
        space.region_3d.view_rotation=Vector((2,-5,1.2)).to_track_quat('Z','Y')
    elif area.type=='DOPESHEET_EDITOR':
        area.spaces.active.mode='ACTION';area.spaces.active.dopesheet.show_only_selected=False;area.spaces.active.show_region_channels=True
        area.spaces.active.dopesheet.show_expanded_summary=True
    elif area.type=='PROPERTIES':area.spaces.active.context='DATA'
blend=ROOT/'GreenWare/Assets/_src/MiniBotC/MiniBotC_AnimationReady.blend'
bpy.ops.wm.save_as_mainfile(filepath=str(blend),compress=True);bpy.ops.file.make_paths_relative();bpy.ops.wm.save_as_mainfile(filepath=str(blend),compress=True)
report['blend']=str(blend);report['blend_bytes']=blend.stat().st_size
report['skin_influences_max']=4;report['source']='MiniBotC_TripoOptimized.blend'
(OUT/'AssetReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print('ANIMATION READY EXPORT COMPLETE '+json.dumps(report),flush=True)
