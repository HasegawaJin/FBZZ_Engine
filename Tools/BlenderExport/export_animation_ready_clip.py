"""アクティブな制作 Action を54ボーンへ評価・ベイクし、単一テイクの FBX にする。"""
import bpy,math,re
from pathlib import Path
from mathutils import Matrix

def export_clip(filepath,action=None,start=None,end=None,include_mesh=True):
    scene=bpy.context.scene;control=bpy.data.objects['MiniBotC_ControlRig'];source=bpy.data.objects['MiniBotC_Humanoid']
    action=action or (control.animation_data.action if control.animation_data else None)
    if not action:raise RuntimeError('Select a Player control-rig Action before export')
    start=int(math.floor(action.frame_range[0])) if start is None else start
    end=int(math.ceil(action.frame_range[1])) if end is None else end
    old_frame=scene.frame_current;old_action=control.animation_data.action;old_range=(scene.frame_start,scene.frame_end)
    old_mode=bpy.context.object.mode if bpy.context.object else 'OBJECT';old_active=bpy.context.view_layer.objects.active
    selected=list(bpy.context.selected_objects);source_pose=source.data.pose_position
    if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
    dest=source.copy();dest.data=source.data.copy();scene.collection.objects.link(dest);dest.name='EXPORT_Humanoid'
    dest.animation_data_clear();dest.hide_set(False);dest.data.pose_position='POSE'
    for p in dest.pose.bones:
        for c in list(p.constraints):p.constraints.remove(c)
        p.rotation_mode='QUATERNION'
    meshes=[]
    try:
        control.animation_data.action=action;source.data.pose_position='POSE'
        for frame in range(start,end+1):
            scene.frame_set(frame);control.update_tag();source.update_tag();bpy.context.view_layer.update()
            for p in dest.pose.bones:
                ref=source.pose.bones[p.name]
                if ref.parent:
                    basis=ref.bone.convert_local_to_pose(ref.matrix,ref.bone.matrix_local,parent_matrix=ref.parent.matrix,parent_matrix_local=ref.parent.bone.matrix_local,invert=True)
                else:basis=ref.bone.convert_local_to_pose(ref.matrix,ref.bone.matrix_local,invert=True)
                p.matrix_basis=basis
                p.keyframe_insert('location',frame=frame,group=p.name);p.keyframe_insert('rotation_quaternion',frame=frame,group=p.name);p.keyframe_insert('scale',frame=frame,group=p.name)
        dest.animation_data.action.name=action.name+'_Baked'
        for check_frame in [min(end,start+15),min(end,start+30),min(end,start+60)]:
            scene.frame_set(check_frame);control.update_tag();source.update_tag();bpy.context.view_layer.update()
            errors=sorted([(max(abs(float(a-b)) for ra,rb in zip(p.matrix,source.pose.bones[p.name].matrix) for a,b in zip(ra,rb)),p.name) for p in dest.pose.bones],reverse=True)
            assert errors[0][0]<1e-4,f'Pose bake differs at {check_frame}: {errors[:3]}'
        if include_mesh:
            for name in ['Player_LOD0','Player_Fingers_LOD0']:
                mesh=bpy.data.objects[name].copy();mesh.data=mesh.data.copy();scene.collection.objects.link(mesh);mesh.name='EXPORT_'+name;mesh.parent=dest;mesh.matrix_world=Matrix.Identity(4);mesh.hide_set(False);meshes.append(mesh)
                for m in mesh.modifiers:
                    if m.type=='ARMATURE':m.object=dest
        bpy.ops.object.select_all(action='DESELECT');dest.select_set(True);bpy.context.view_layer.objects.active=dest
        for mesh in meshes:mesh.select_set(True)
        scene.frame_start=start;scene.frame_end=end;scene.frame_set(start)
        Path(filepath).parent.mkdir(parents=True,exist_ok=True)
        bpy.ops.export_scene.fbx(filepath=str(filepath),use_selection=True,object_types={'MESH','ARMATURE'},
            global_scale=1,apply_unit_scale=True,apply_scale_options='FBX_SCALE_ALL',use_space_transform=True,
            axis_forward='-Z',axis_up='Y',add_leaf_bones=False,use_armature_deform_only=False,
            mesh_smooth_type='OFF',use_tspace=True,path_mode='COPY',embed_textures=False,
            bake_anim=True,bake_anim_use_all_actions=False,bake_anim_use_nla_strips=False,
            bake_anim_step=1.0,bake_anim_simplify_factor=0.0,bake_anim_force_startend_keying=True)
        return {'file':str(filepath),'action':action.name,'frames':[start,end],'fps':scene.render.fps,'bones':len(dest.data.bones)}
    finally:
        baked=dest.animation_data.action if dest.animation_data else None
        for mesh in meshes:
            data=mesh.data;bpy.data.objects.remove(mesh,do_unlink=True);bpy.data.meshes.remove(data)
        data=dest.data;bpy.data.objects.remove(dest,do_unlink=True);bpy.data.armatures.remove(data)
        if baked and baked.users==0:bpy.data.actions.remove(baked)
        source.data.pose_position=source_pose;control.animation_data.action=old_action
        scene.frame_start,scene.frame_end=old_range;scene.frame_set(old_frame)
        bpy.ops.object.select_all(action='DESELECT')
        for o in selected:
            if o.name in scene.objects and o.visible_get():o.select_set(True)
        bpy.context.view_layer.objects.active=old_active
        if old_active and old_mode!='OBJECT':bpy.ops.object.mode_set(mode=old_mode)

if __name__=='__main__':
    active=bpy.data.objects['MiniBotC_ControlRig'].animation_data.action
    label=re.sub(r'[^A-Za-z0-9_-]','_',active.name) if active else 'Clip'
    destination=Path(bpy.path.abspath('//../../Models/MiniBotC_AnimationReady/Animations'))/(label+'.fbx')
    print(export_clip(destination))
