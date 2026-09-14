"""Playerの基準モーションを接地が見えるステージで描画する。"""
import bpy
import sys
from pathlib import Path
from mathutils import Vector

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'Docs/Art/MiniBotC/Foundation'
scene=bpy.context.scene
control=bpy.data.objects['MiniBotC_ControlRig']
scene.render.engine='CYCLES'
scene.cycles.device='GPU'
scene.cycles.samples=12
scene.cycles.use_denoising=True
scene.render.use_persistent_data=True
prefs=bpy.context.preferences.addons['cycles'].preferences
prefs.compute_device_type='OPTIX'
prefs.refresh_devices()
for device in prefs.devices:
    device.use=device.type=='OPTIX'
if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
floor=bpy.data.objects.get('MotionReview_Floor')
if not floor:
    bpy.ops.mesh.primitive_plane_add(size=200,location=(0,0,-.006))
    floor=bpy.context.object
    floor.name='MotionReview_Floor'
    mat=bpy.data.materials.new('MotionReview_Floor');mat.use_nodes=True
    nodes=mat.node_tree.nodes
    bsdf=next(n for n in nodes if n.type=='BSDF_PRINCIPLED');bsdf.inputs['Roughness'].default_value=.82
    checker=nodes.new('ShaderNodeTexChecker');checker.inputs['Color1'].default_value=(.11,.125,.14,1);checker.inputs['Color2'].default_value=(.17,.19,.21,1);checker.inputs['Scale'].default_value=1
    coords=nodes.new('ShaderNodeTexCoord');mat.node_tree.links.new(coords.outputs['Object'],checker.inputs['Vector']);mat.node_tree.links.new(checker.outputs['Color'],bsdf.inputs['Base Color'])
    floor.data.materials.append(mat)
for o in scene.objects:
    if o.name.startswith('Companion'):o.hide_render=True
cam=scene.camera;cam.data.type='ORTHO'
cam.location=(3,-6,2.8);cam.rotation_euler=(Vector((0,-.05,1.04))-cam.location).to_track_quat('-Z','Y').to_euler();cam.data.ortho_scale=2.9
scene.render.resolution_x=900;scene.render.resolution_y=900;scene.render.resolution_percentage=100
scene.render.image_settings.file_format='PNG';scene.render.image_settings.color_mode='RGB'
args=sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else []
if args and args[0]=='video':
    label=args[1]
    action=bpy.data.actions['MB_C_'+label];control.animation_data.action=action
    start,end=[int(f) for f in action.frame_range]
    scene.cycles.samples=6
    scene.render.resolution_x=640;scene.render.resolution_y=640
    for slot in action.slots:
        for layer in action.layers:
            for strip in layer.strips:
                bag=strip.channelbag(slot)
                if bag:
                    for curve in bag.fcurves:curve.modifiers.new('CYCLES')
    repeats=1 if label=='Idle' else 4 if label=='Run_F' else 3
    scene.frame_start=start;scene.frame_end=(end-start)*repeats
    if label=='Run_F':
        floor.location.y=0;floor.keyframe_insert('location',frame=1)
        floor.location.y=(scene.frame_end-1)/30*6;floor.keyframe_insert('location',frame=scene.frame_end)
        for layer in floor.animation_data.action.layers:
            for strip in layer.strips:
                for curve in strip.channelbag(floor.animation_data.action_slot).fcurves:
                    for key in curve.keyframe_points:key.interpolation='LINEAR'
    scene.render.image_settings.media_type='VIDEO'
    scene.render.image_settings.file_format='FFMPEG'
    scene.render.ffmpeg.format='MPEG4';scene.render.ffmpeg.codec='H264'
    scene.render.ffmpeg.constant_rate_factor='HIGH';scene.render.ffmpeg.ffmpeg_preset='GOOD'
    scene.render.filepath=str(OUT/(label+'.mp4'))
    def progress(current,*unused):
        if current.frame_current%10==0:print('VIDEO FRAME '+label+' '+str(current.frame_current),flush=True)
    bpy.app.handlers.render_post.append(progress)
    bpy.ops.render.render(animation=True)
    print('VIDEO COMPLETE '+label,flush=True)
else:
    shots=[('Stance',1),('Run_F',1),('Run_F',4),('Run_F',7),('Run_F',10),
           ('Slash01',7),('Slash01',9),('Slash01',11),('Parry',3),('Parry',12)]
    for label,frame in shots:
        control.animation_data.action=bpy.data.actions['MB_C_'+label]
        scene.frame_set(frame);bpy.context.view_layer.update()
        scene.render.filepath=str(OUT/f'{label}_{frame:02d}.png')
        bpy.ops.render.render(write_still=True)
        print('PREVIEW '+label+' '+str(frame),flush=True)
