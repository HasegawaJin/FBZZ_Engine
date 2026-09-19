"""現在の Player に歩行 Action を追加する。既存 Action の変更・書き出しはしない。"""

import ast
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Euler, Matrix, Vector


SOURCE = Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\BlenderExport')
ACTION_NAME = 'MB_C_Walk_F'
CYCLE = 32
STANCE = 20
SPEED = 1.0
FPS = 30


def make_pose_tools(control, rig, body, sword, scene):
    reference = bpy.data.actions.get('Player_NewAction')
    if reference is None:
        source_blend = SOURCE.parent.parent / 'GreenWare/Assets/_src/MiniBotC/MiniBotC_AnimationReady.blend'
        with bpy.data.libraries.load(str(source_blend), link=False) as (available, loaded):
            assert 'Player_NewAction' in available.actions
            loaded.actions = ['Player_NewAction']
        reference = loaded.actions[0]
    control.animation_data.action = reference
    scene.frame_set(1)
    bpy.context.view_layer.update()
    rest = {p.name: p.matrix_basis.copy() for p in control.pose.bones}
    rest_world = {p.name: p.matrix.copy() for p in control.pose.bones}
    foot_ids = {}
    for side, label in [('R', 'Right'), ('L', 'Left')]:
        groups = {g.index for g in body.vertex_groups
                  if g.name in (label + 'Foot', label + 'ToeBase')}
        foot_ids[side] = [v.index for v in body.data.vertices
                          if sum(g.weight for g in v.groups if g.group in groups) > .55]
    namespace = dict(bpy=bpy, np=np, math=math, Euler=Euler, Matrix=Matrix, Vector=Vector,
                     control=control, rig=rig, body=body, sword=sword, rest=rest,
                     rest_world=rest_world, foot_ids=foot_ids,
                     hand_to_sword=rest_world['hand_ik.R'].inverted() @ sword.matrix_world)
    # 基準4本と同じ接地・握りを使うが、バッチ生成側の保存や既存Action処理は実行しない。
    source = SOURCE / 'author_minibot_c_foundation.py'
    parsed = ast.parse(source.read_text(encoding='utf-8'), filename=str(source))
    names = {'update', 'sample', 'rotation', 'local_rotation', 'world_pose', 'sword_pose',
             'fingers', 'base_pose', 'evaluate_foot_surface', 'apply_pose'}
    functions = [node for node in parsed.body
                 if isinstance(node, ast.FunctionDef) and node.name in names]
    assert {node.name for node in functions} == names
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(source), 'exec'), namespace)
    return namespace


def walk_pose(frame, helpers):
    phase = (frame - 1) % CYCLE
    sample = helpers['sample']
    pose = helpers['base_pose']()
    sway = float(sample({0:[0], 8:[-.032], 16:[0], 24:[.032], 32:[0]}, phase)[0])
    height = float(sample({0:[-.045], 4:[-.062], 8:[-.025], 12:[-.015],
                           16:[-.045], 20:[-.062], 24:[-.025], 28:[-.015],
                           32:[-.045]}, phase)[0])
    twist = float(sample({0:[-5], 8:[0], 16:[5], 24:[0], 32:[-5]}, phase)[0])
    pose['torso'] = (sway, -.012, height)
    pose['hips'] = (3, 0, twist)
    pose['chest'] = (2, 0, -twist * .7)
    pose['head'] = (-3, 0, twist * .3)
    pose['grip'] = sample({0:(-.285,-.135,.955), 8:(-.280,-.18,.965),
                           16:(-.280,-.22,.955), 24:(-.290,-.18,.96),
                           32:(-.285,-.135,.955)}, phase)
    pose['blade'] = sample({0:(-18,10,0), 16:(-12,8,0), 32:(-18,10,0)}, phase)
    pose['left'] = sample({0:(.285,-.225,.895), 8:(.300,-.055,.830),
                           16:(.280,.135,.855), 24:(.290,-.025,.845),
                           32:(.285,-.225,.895)}, phase)
    pose['left_rot'] = sample({0:(-8,0,-5), 16:(20,0,-5), 32:(-8,0,-5)}, phase)
    pose['left_closed'] = .22
    support = []
    for side, offset, x, suffix in [('R',0,-.18,'r'), ('L',16,.18,'l')]:
        t = (phase + offset) % CYCLE
        front = .05 - STANCE * SPEED / FPS / 2
        back = .05 + STANCE * SPEED / FPS / 2
        if t <= STANCE:
            y = front + SPEED / FPS * t
            z = .132
            pitch = float(sample({0:[-8], 2:[0], 15:[0], 18:[10], 20:[20]}, t)[0])
            support.append(side)
        else:
            y, z, pitch = sample({20:(back,.160,20), 22:(.37,.250,25),
                                  25:(.10,.295,9), 28:(-.22,.245,-5),
                                  30:(front-.023,.175,-10),
                                  32:(front,.132,-8)}, t)
        pose['foot_' + suffix] = (x, float(y), float(z))
        pose['foot_rot_' + suffix] = (float(pitch), 0, 0)
    pose['support'] = support
    return pose


def main():
    if ACTION_NAME in bpy.data.actions:
        raise RuntimeError('Existing walk Action is preserved: ' + ACTION_NAME)
    scene = bpy.context.scene
    assert scene.render.fps == FPS and scene.render.fps_base == 1
    control = bpy.data.objects['MiniBotC_ControlRig']
    rig = bpy.data.objects['MiniBotC_Humanoid']
    body = bpy.data.objects['Player_LOD0']
    sword = bpy.data.objects['Sword_Control']
    helpers = make_pose_tools(control, rig, body, sword, scene)
    action = bpy.data.actions.new(ACTION_NAME)
    action.use_fake_user = True
    control.animation_data.action = action
    keyed = ['root', 'torso', 'hips', 'chest', 'head', 'hand_ik.L', 'hand_ik.R',
             'foot_ik.L', 'foot_ik.R', 'toe_ik.L', 'toe_ik.R']
    keyed += [f'{stem}.{joint:02d}.{side}'
              for stem in ['f_index', 'f_middle', 'f_ring', 'f_pinky', 'thumb']
              for joint in range(1, 4) for side in ['L', 'R']]
    ground_error = 0.0
    sword_height = 1000.0
    min_clearance = 1000.0
    for frame in range(1, CYCLE + 2):
        scene.frame_set(frame)
        pose = walk_pose(frame, helpers)
        helpers['apply_pose'](pose)
        points = helpers['evaluate_foot_surface']()
        # 回転した足のIK原点ではなく、変形後の足裏から遊脚のクリアランスを決める。
        for side, offset in [('R', 0), ('L', 16)]:
            t = (frame - 1 + offset) % CYCLE
            if side not in pose['support']:
                clearance = float(helpers['sample'](
                    {20:[.002], 22:[.045], 25:[.11], 28:[.08], 30:[.035], 32:[.002]}, t)[0])
                height = float(np.min(points[helpers['foot_ids'][side], 2]))
                bone = control.pose.bones['foot_ik.' + side]
                matrix = bone.matrix.copy()
                matrix.translation.z += clearance - height
                bone.matrix = matrix
        helpers['update']()
        points = helpers['evaluate_foot_surface']()
        for side in pose['support']:
            height = float(np.min(points[helpers['foot_ids'][side], 2]))
            ground_error = max(ground_error, abs(height - .002))
        for side in ('R', 'L'):
            min_clearance = min(min_clearance, float(np.min(points[helpers['foot_ids'][side], 2])))
        sword_height = min(sword_height, min((sword.matrix_world @ v.co).z
                          for v in bpy.data.objects['Sword_LOD0'].data.vertices))
        for name in keyed:
            bone = control.pose.bones[name]
            bone.rotation_mode = 'QUATERNION'
            for channel in ('location', 'rotation_quaternion', 'scale'):
                bone.keyframe_insert(channel, frame=frame, group=name)
        control.keyframe_insert('["two_hand_grip"]', frame=frame, group='Weapon')
    for slot in action.slots:
        for layer in action.layers:
            for strip in layer.strips:
                bag = strip.channelbag(slot)
                if bag:
                    for curve in bag.fcurves:
                        for key in curve.keyframe_points:
                            key.interpolation = 'LINEAR'
    metadata = dict(name=ACTION_NAME, start_frame=1, end_frame=CYCLE + 1,
                    duration_seconds=CYCLE / FPS, loop=True, reference='MB_C_Stance',
                    layer='Base', root_motion='in_place', reference_speed_mps=SPEED,
                    stride_length_m=SPEED * CYCLE / FPS)
    for key, value in metadata.items():
        action[key] = value
    for name, frame in {'FootContact_R':1, 'FootFlat_R':3, 'ToeOff_L':6,
                        'Passing_L':10, 'FootContact_L':17, 'FootFlat_L':19,
                        'ToeOff_R':22, 'Passing_R':26, 'CycleEnd':33}.items():
        action.pose_markers.new(name).frame = frame
    scene.frame_start = 1
    scene.frame_end = CYCLE
    scene.use_preview_range = False
    scene.frame_set(1)
    bpy.context.view_layer.update()
    first = {p.name: p.matrix.copy() for p in rig.pose.bones}
    scene.frame_set(CYCLE + 1)
    bpy.context.view_layer.update()
    seam = max(abs(first[p.name][row][col] - p.matrix[row][col])
               for p in rig.pose.bones for row in range(4) for col in range(4))
    scene.frame_set(1)
    report = dict(action=ACTION_NAME, frames=[1, CYCLE + 1], preview_frames=[1, CYCLE],
                  ground_error_m=ground_error, minimum_foot_z_m=min_clearance,
                  minimum_sword_z_m=sword_height, loop_seam_error=seam)
    print('WALK_REVIEW ' + json.dumps(report), flush=True)
    assert ground_error < .012, report
    assert min_clearance > -.008, report
    assert sword_height > .02, report
    assert seam < .0001, report
    action['authoring_checks'] = json.dumps(report)


if __name__ == '__main__':
    main()
