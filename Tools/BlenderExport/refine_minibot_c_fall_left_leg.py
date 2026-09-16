"""空中ループの右足の軌道を左右反転し、左足だけへ時差を付けて適用する。"""

import math

import bpy
from mathutils import Matrix, Vector


def curves(action):
    for slot in action.slots:
        for layer in action.layers:
            for strip in layer.strips:
                bag = strip.channelbag(slot)
                if bag:
                    yield from bag.fcurves


def author():
    rig = bpy.data.objects['MiniBotC_ControlRig']
    scene = bpy.context.scene
    source = bpy.data.actions['MB_C_FallLoop']
    assert 'REFERENCE_REVIEW_FallLoop' not in bpy.data.actions
    rig.animation_data.action = source
    rig.animation_data.action_slot = source.slots[0]
    scene.frame_set(1)
    bpy.context.view_layer.update()
    first_right = rig.pose.bones['foot_ik.R'].matrix.copy()
    first_left = rig.pose.bones['foot_ik.L'].matrix.copy()
    boundary = rig.pose.bones['foot_ik.L'].matrix_basis.copy()
    reflection = Matrix.Diagonal(Vector((-1.,1.,1.)))
    samples = []
    for index in range(61):
        frame = 1+index*.5
        t = (frame-1)/30
        # 先頭と末尾を保った時間変形で、左脚の伸びる瞬間を約3フレーム遅らせる。
        source_t = t-.105*math.sin(math.pi*t)**2
        source_frame = 1+30*source_t
        scene.frame_set(int(source_frame),subframe=source_frame%1)
        bpy.context.view_layer.update()
        right = rig.pose.bones['foot_ik.R'].matrix.copy()
        delta = right.to_3x3()@first_right.to_3x3().inverted()
        rotation = reflection@delta@reflection@first_left.to_3x3()
        matrix = rotation.to_4x4()
        matrix.translation = first_left.translation+reflection@(right.translation-first_right.translation)
        samples.append((frame,matrix))
    action = source.copy()
    action.name = 'REFERENCE_REVIEW_FallLoop'
    action.use_fake_user = True
    rig.animation_data.action = action
    rig.animation_data.action_slot = action.slots[0]
    previous = None
    for frame,matrix in samples:
        scene.frame_set(int(frame),subframe=frame%1)
        bone = rig.pose.bones['foot_ik.L']
        if frame in (1,31):
            bone.matrix_basis = boundary
        else:
            bone.matrix = matrix
        bone.rotation_mode = 'QUATERNION'
        if previous is not None and previous.dot(bone.rotation_quaternion)<0:
            bone.rotation_quaternion.negate()
        previous = bone.rotation_quaternion.copy()
        for channel in ('location','rotation_quaternion','scale'):
            bone.keyframe_insert(channel,frame=frame,group='foot_ik.L')
    for curve in curves(action):
        if curve.data_path.startswith('pose.bones["foot_ik.L"]'):
            for key in curve.keyframe_points:
                key.interpolation = 'LINEAR'
    action['motion_revision'] = '2026-09-14-active-both-legs-fall'
    action['authoring_tool'] = 'refine_minibot_c_fall_left_leg.py'
    action['left_leg_reference'] = 'Right foot motion mirrored, with smooth delayed timing and unchanged loop boundary'
    action['left_leg_peak_delay_frames'] = 3.
    for name,frame in [('RightLegExtend',16),('LeftLegExtend',19)]:
        marker = action.pose_markers.get(name) or action.pose_markers.new(name)
        marker.frame = frame
    scene.frame_set(1)
    bpy.context.view_layer.update()
    return action
