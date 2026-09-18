"""剣を両手で構える参照歩行を、原地移動のループとして制作する。"""

import importlib.util
import json
import math
from pathlib import Path

import bpy
import numpy as np

TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('walk_pose_tools',TOOLS/'author_minibot_c_spin_jump.py')
extended = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extended)


class SwordWalk(extended.ExtendedAttacks):
    sole_clearance = .005

    def motion_origin(self,label,frame,source):
        if label!='SwordWalk':
            return super().motion_origin(label,frame,source)
        first,last = self.boundary[label]
        end = float(self.refs[label]['rig'].animation_data.action.frame_range[1])
        # 一定速度の前進だけを除き、支持脚へ体重を移す腰の揺れを残す。
        origin = first.lerp(last,(frame-1)/(end-1))
        origin.z = 0
        return origin

    def elbow_weight(self,label):
        return .65 if label=='SwordWalk' else super().elbow_weight(label)

    def pose(self,label,frame):
        feet = super().pose(label,frame)
        if label=='SwordWalk':
            for side,direction in [('L',1),('R',-1)]:
                bone = self.control.pose.bones['foot_ik.'+side]
                matrix = bone.matrix.copy()
                matrix.translation.x += direction*.045
                bone.matrix = matrix
            self.grip.update()
            for _ in range(3):
                points = self.helpers['evaluate_foot_surface']()
                for side,target in feet.items():
                    bone = self.control.pose.bones['foot_ik.'+side]
                    matrix = bone.matrix.copy()
                    matrix.translation.z += target-float(np.min(points[self.helpers['foot_ids'][side],2]))
                    bone.matrix = matrix
                self.grip.update()
        return feet

    @staticmethod
    def smooth_foot_seam(action):
        end = float(action['end_frame'])
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if not bag:
                        continue
                    for curve in bag.fcurves:
                        if curve.data_path not in ('pose.bones["foot_ik.L"].location','pose.bones["foot_ik.R"].location'):
                            continue
                        value = curve.evaluate(1)
                        tangent = ((value-curve.evaluate(end-.125))+(curve.evaluate(1.125)-value))/.25
                        segments = []
                        for start,finish in [(end-2,end),(1,3)]:
                            first = curve.evaluate(start)
                            last = curve.evaluate(finish)
                            before = tangent if start==1 else (curve.evaluate(start+.125)-curve.evaluate(start-.125))/.25
                            after = tangent if finish==end else (curve.evaluate(finish+.125)-curve.evaluate(finish-.125))/.25
                            segments.append((start,finish,first,last,before,after))
                        for start,finish,first,last,before,after in segments:
                            length = finish-start
                            for frame in np.arange(start,finish+.001,.25):
                                t = (frame-start)/length
                                v = (2*t**3-3*t*t+1)*first+(t**3-2*t*t+t)*length*before
                                v += (-2*t**3+3*t*t)*last+(t**3-t*t)*length*after
                                point = curve.keyframe_points.insert(float(frame),float(v),options={'FAST'})
                                point.interpolation = 'LINEAR'
                        curve.update()
        action['loop_seam_blend_frames'] = 2

    def author(self):
        for window in bpy.context.window_manager.windows:
            if window.screen.is_animation_playing:
                with bpy.context.temp_override(window=window):
                    bpy.ops.screen.animation_cancel(restore_frame=False)
        name = 'REFERENCE_REVIEW_SwordWalk'
        if name in bpy.data.actions:
            raise RuntimeError('Review Action already exists: '+name)
        source = self.refs['SwordWalk']
        source_end = float(source['rig'].animation_data.action.frame_range[1])
        fps = source['fps']
        end = round((source_end-1)*30/fps)+1
        self.control.animation_data.action = None
        self.previous_sample = None
        action = bpy.data.actions.new(name)
        action.use_fake_user = True
        self.control.animation_data.action = action
        previous = {}
        previous_roll = None
        initial = None
        samples = []
        for frame in np.arange(1,end+.001,.5):
            if frame==end:
                self.restore(initial)
                feet = initial_feet
            else:
                source_frame = 1+(frame-1)*(source_end-1)/(end-1)
                feet = self.pose('SwordWalk',source_frame)
                if initial is None:
                    initial = self.capture()
                    initial_feet = dict(feet)
            snapshot = self.capture()
            self.scene.frame_set(int(frame),subframe=float(frame%1))
            self.restore(snapshot)
            roll = float(self.control['left_grip_roll'])
            if previous_roll is not None:
                roll += round((previous_roll-roll)/math.tau)*math.tau
            previous_roll = roll
            self.control['left_grip_roll'] = roll
            self.grip.update()
            for bone_name in self.keyed:
                bone = self.control.pose.bones[bone_name]
                bone.rotation_mode = 'QUATERNION'
                if bone_name in previous and previous[bone_name].dot(bone.rotation_quaternion)<0:
                    bone.rotation_quaternion.negate()
                previous[bone_name] = bone.rotation_quaternion.copy()
                for channel in ('location','rotation_quaternion','scale'):
                    bone.keyframe_insert(channel,frame=float(frame),group=bone_name)
            for prop in ('two_hand_grip','left_grip_roll'):
                self.control.keyframe_insert(f'["{prop}"]',frame=float(frame),group='Weapon')
            for side in ('R','L'):
                for prefix in ('upper_arm_parent.','thigh_parent.'):
                    self.control.pose.bones[prefix+side].keyframe_insert('["IK_FK"]',frame=float(frame),group='IK')
                self.control.pose.bones['upper_arm_parent.'+side].keyframe_insert('["pole_vector"]',frame=float(frame),group='IK')
            samples.append({'frame':float(frame),'feet':feet,'wrists':self.grip.wrist_angles()})
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if bag:
                        for curve in bag.fcurves:
                            for point in curve.keyframe_points:
                                point.interpolation = 'LINEAR'
        delta = (self.boundary['SwordWalk'][1]-self.boundary['SwordWalk'][0])*.878
        duration = (end-1)/30
        for key,value in dict(name='MB_C_SwordWalk',start_frame=1,end_frame=end,
            duration_seconds=duration,loop=True,reference='MB_C_Stance',layer='Base',
            root_motion='in_place',two_handed=True,reference_speed_mps=abs(delta.y)/duration,
            source_reference=source['filepath'],source_fps=fps,source_start=1,source_end=source_end,
            sole_clearance_m=self.sole_clearance,reference_horizontal_travel_m=[delta.x,delta.y,0.],
            motion_revision='2026-09-14-sword-walk').items():
            action[key] = value
        for label,frame in [('CycleStart',1),('LeftPlant',12),('OppositeStep',21),('RightPlant',33)]:
            action.pose_markers.new(label).frame = round(1+(frame-1)*(end-1)/(source_end-1))
        bpy.app.driver_namespace['sword_walk_samples'] = samples
        self.smooth_foot_seam(action)
        self.scene.render.fps = 30
        self.scene.render.fps_base = 1
        self.scene.frame_start = 1
        self.scene.frame_end = end-1
        self.scene.frame_set(1)
        print('SWORD_WALK '+json.dumps({'range':[1,end],'duration':duration,'speed_mps':action['reference_speed_mps']}),flush=True)
        return action


def create_session():
    session = SwordWalk()
    bpy.app.driver_namespace['sword_walk_session'] = session
    return session
