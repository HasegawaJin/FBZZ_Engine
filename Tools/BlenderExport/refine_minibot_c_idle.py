"""両手の構えと足の接地を保ち、待機に重心移動と周囲を見る動きを加える。"""

import importlib.util
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Euler, Matrix, Vector

TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('idle_pose_tools',TOOLS/'refine_minibot_c_run_slash.py')
pose_tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pose_tools)


class ActiveIdle:
    cycle = 144

    def __init__(self):
        self.base = pose_tools.MotionRefinement()
        self.control = self.base.control
        self.game = self.base.game
        self.grip = self.base.grip
        self.scene = self.base.scene
        self.helpers = self.base.helpers
        self.keyed = list(self.base.keyed)+['shoulder.L','shoulder.R','neck']
        self.original = bpy.data.actions['MB_C_Idle']
        self.control.animation_data.action = self.original
        self.control.animation_data.action_slot = self.original.slots[0]
        self.scene.frame_set(1)
        self.grip.update()
        self.original_pose = {b.name:b.matrix.copy() for b in self.game.pose.bones}
        self.original_pose['Sword'] = self.grip.sword.matrix_world.copy()
        self.elbows = {q:self.game.pose.bones[p+'ForeArm'].head.copy() for q,p in [('R','Right'),('L','Left')]}
        self.shoulders = {q:self.game.pose.bones[p+'Arm'].head.copy() for q,p in [('R','Right'),('L','Left')]}
        self.base.aim_elbows(self.elbows,0.)
        self.anchor = {b.name:b.matrix_basis.copy() for b in self.control.pose.bones}
        self.torso = self.control.pose.bones['torso'].matrix.copy()
        self.sword = self.grip.sword.matrix_world.copy()
        self.roll = float(self.control['left_grip_roll'])
        self.feet = {q:self.control.pose.bones['foot_ik.'+q].matrix.copy() for q in ('R','L')}
        points = self.helpers['evaluate_foot_surface']()
        self.clearance = {q:float(points[self.helpers['foot_ids'][q],2].min()) for q in ('R','L')}

    def curve(self, values, frame):
        return float(self.base.sample({f:[v] for f,v in values.items()},frame)[0])

    def restore_anchor(self):
        for name,matrix in self.anchor.items():
            self.control.pose.bones[name].matrix_basis = matrix
        self.control['two_hand_grip'] = 1.
        self.control['left_grip_roll'] = self.roll
        for side in ('L','R'):
            self.control.pose.bones['upper_arm_parent.'+side]['pole_vector'] = True
            self.control.pose.bones['upper_arm_parent.'+side]['IK_FK'] = 0.
            self.control.pose.bones['thigh_parent.'+side]['IK_FK'] = 0.
        self.grip.update()

    def rotate_world(self, name, angles):
        bone = self.control.pose.bones[name]
        current = bone.matrix.copy()
        delta = Euler(tuple(math.radians(v) for v in angles),'XYZ').to_matrix().to_4x4()
        matrix = delta@current
        matrix.translation = current.translation
        bone.matrix = matrix
        self.grip.update()

    def pose(self, frame):
        self.restore_anchor()
        f = frame-1
        if f<=0 or f>=self.cycle:
            return
        x = self.curve({0:0,20:.020,46:.029,62:.018,78:-.023,104:-.032,125:-.012,144:0},f)
        y = self.curve({0:0,28:.007,60:.012,85:-.003,114:.004,144:0},f)
        z = self.curve({0:0,20:-.010,40:.004,65:-.018,94:-.006,118:.006,144:0},f)
        shift = Vector((x,y,z))
        torso = self.torso.copy()
        torso.translation += shift
        self.control.pose.bones['torso'].matrix = torso
        self.grip.update()
        hip_yaw = self.curve({0:0,34:-2,63:0,100:2.5,122:.7,144:0},f)
        chest_yaw = self.curve({0:0,40:-3,60:-.5,105:3.5,144:0},f)
        lean = self.curve({0:0,30:1,64:2,100:-.6,125:.5,144:0},f)
        self.rotate_world('hips',(0,-x*38,hip_yaw))
        self.rotate_world('chest',(lean,x*35,chest_yaw))
        pitch = self.curve({0:0,23:-1.5,51:0,80:2,110:-1,144:0},f)
        look = self.curve({0:0,27:-10,49:-6,66:0,91:11,112:7,144:0},f)
        self.rotate_world('neck',(-lean*.35,0,-chest_yaw*.3))
        self.rotate_world('head',(pitch,-x*20,look))
        shrug = self.curve({0:0,25:1.5,49:-.6,74:1,101:-1,125:1.5,144:0},f)
        for side,sign in [('L',1),('R',-1)]:
            self.rotate_world('shoulder.'+side,(0,sign*shrug,sign*chest_yaw*.25))
        for side,matrix in self.feet.items():
            self.control.pose.bones['foot_ik.'+side].matrix = matrix
        self.grip.update()
        for _ in range(3):
            points = self.helpers['evaluate_foot_surface']()
            for side,target in self.clearance.items():
                bone = self.control.pose.bones['foot_ik.'+side]
                matrix = bone.matrix.copy()
                matrix.translation.z += target-float(points[self.helpers['foot_ids'][side],2].min())
                bone.matrix = matrix
            self.grip.update()
        lift = self.curve({0:0,28:.018,50:.008,72:-.008,98:.014,122:.021,144:0},f)
        tilt = self.curve({0:0,32:-3.5,56:-1,84:4,113:2,144:0},f)
        pitch = self.curve({0:0,25:1.5,62:-2.5,102:2,125:1,144:0},f)
        rotation = Euler(tuple(math.radians(v) for v in (pitch,tilt,chest_yaw*.7)),'XYZ').to_matrix()
        sword = (rotation@self.sword.to_3x3()).to_4x4()
        sword.translation = self.sword.translation+Vector((x*.70,y*.6,z*.65+lift))
        self.grip.set_hand('R',sword@self.grip.attachment.inverted())
        chest_rotation = Euler((math.radians(lean),math.radians(x*35),math.radians(chest_yaw)),'XYZ').to_matrix()
        elbows = {side:self.game.pose.bones[prefix+'Arm'].head+chest_rotation@(self.elbows[side]-self.shoulders[side])
                  for side,prefix in [('R','Right'),('L','Left')]}
        self.base.aim_elbows(elbows,0.)
        self.grip.limit_right_wrist(32)
        self.grip.update()

    def author(self):
        name = 'REFERENCE_REVIEW_IdleActive'
        if name in bpy.data.actions:
            raise RuntimeError('Review Action already exists: '+name)
        action = bpy.data.actions.new(name)
        action.use_fake_user = True
        self.control.animation_data.action = action
        previous = {}
        for frame in np.arange(1,self.cycle+1.001,.5):
            self.scene.frame_set(int(frame),subframe=float(frame%1))
            self.pose(float(frame))
            for name in self.keyed:
                bone = self.control.pose.bones[name]
                bone.rotation_mode = 'QUATERNION'
                if name in previous and previous[name].dot(bone.rotation_quaternion)<0:
                    bone.rotation_quaternion.negate()
                previous[name] = bone.rotation_quaternion.copy()
                for channel in ('location','rotation_quaternion','scale'):
                    bone.keyframe_insert(channel,frame=float(frame),group=name)
            for prop in ('two_hand_grip','left_grip_roll'):
                self.control.keyframe_insert(f'["{prop}"]',frame=float(frame),group='Weapon')
            for side in ('L','R'):
                for prefix in ('upper_arm_parent.','thigh_parent.'):
                    self.control.pose.bones[prefix+side].keyframe_insert('["IK_FK"]',frame=float(frame),group='IK')
                self.control.pose.bones['upper_arm_parent.'+side].keyframe_insert('["pole_vector"]',frame=float(frame),group='IK')
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if bag:
                        for curve in bag.fcurves:
                            for point in curve.keyframe_points:
                                point.interpolation = 'LINEAR'
        for key,value in dict(name='MB_C_Idle',start_frame=1,end_frame=self.cycle+1,
            duration_seconds=self.cycle/30,loop=True,reference='MB_C_Stance',layer='Base',
            root_motion='in_place',two_handed=True,reference_speed_mps=0.,
            motion_revision='2026-09-14-expressive-idle',entry_pose_preserved=True).items():
            action[key] = value
        for name,frame in [('Ready',1),('LookRight',28),('SettleLeft',47),('LookLeft',92),('Regrip',123)]:
            action.pose_markers.new(name).frame = frame
        self.scene.render.fps = 30
        self.scene.render.fps_base = 1
        self.scene.frame_start = 1
        self.scene.frame_end = self.cycle
        self.scene.frame_set(1)
        return action


def create_session():
    session = ActiveIdle()
    bpy.app.driver_namespace['active_idle_session'] = session
    return session
