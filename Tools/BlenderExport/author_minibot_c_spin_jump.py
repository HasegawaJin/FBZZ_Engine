"""回転斬りとジャンプ斬りを、参照の全区間から制作する。"""

import importlib.util
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

TOOLS = Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\BlenderExport')
spec = importlib.util.spec_from_file_location('reference_motions',TOOLS/'retarget_minibot_c_references.py')
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


class ExtendedAttacks(reference.ReferenceRetarget):
    supported_labels = ('HighSpinAttack','JumpAttack')
    revision = '2026-09-14-spin-jump'
    sample_prefix = 'spin_jump_samples_'
    source_markers = {
        'HighSpinAttack': {'Windup':5,'SpinStart':8,'SpinPeak':20,'Hit':28,'Recover':40},
        'JumpAttack': {'Windup':13,'Takeoff':17,'Apex':20,'Land':24,'Hit':28,'Recover':45},
    }

    def __init__(self):
        super().__init__()
        self.arm_reach = .94
        self.left_elbow_weight = .42
        self.left_roll_limit = 75
        self.previous_left = None
        self.previous_sample = None

    def weapon_direction(self,label,frame,source,axis):
        if label=='HighSpinAttack':
            weight = float(self.base.sample({1:[0],15:[0],17:[1],18:[1],23:[1],27:[0],46:[0]},frame)[0])
            if axis.z<.65:
                horizontal = Vector((axis.x,axis.y,0)).normalized()
                target = horizontal*math.sqrt(1-.65**2)+Vector((0,0,.65))
                axis = axis.lerp(target,weight).normalized()
        return axis

    def weapon_offset(self,label,frame,source):
        if label!='HighSpinAttack':
            return Vector()
        delta = source['Spine2'].to_quaternion()@self.rest[label]['Spine2'].to_quaternion().inverted()
        outward = delta@Vector((-1,0,0))
        side = float(self.base.sample({1:[0],2:[.04],3:[.14],4:[.15],6:[0],46:[0]},frame)[0])
        up = float(self.base.sample({1:[0],15:[0],17:[.07],20:[.05],24:[0],46:[0]},frame)[0])
        left = float(self.base.sample({1:[0],15:[0],17:[-.13],18:[-.10],20:[0],46:[0]},frame)[0])
        back = float(self.base.sample({1:[0],20:[0],22:[.13],23:[.13],25:[0],46:[0]},frame)[0])
        return outward*side+Vector((left,back,up))

    def solve_left_elbow(self):
        reference_elbow = self.game.pose.bones['LeftForeArm'].head.copy()
        anchor = self.previous_left if self.previous_left is not None else reference_elbow
        for _ in range(5):
            shoulder = self.game.pose.bones['LeftArm'].head.copy()
            wrist = self.grip.target.matrix_world.translation.copy()
            offset = wrist-shoulder
            distance = offset.length
            axis = offset.normalized()
            upper = self.game.data.bones['LeftArm'].length
            lower = self.game.data.bones['LeftForeArm'].length
            along = (upper*upper-lower*lower+distance*distance)/(2*distance)
            radius = math.sqrt(max(1e-8,upper*upper-along*along))
            center = shoulder+axis*along
            blade = self.grip.sword.matrix_world.to_3x3().col[2].normalized()
            projected = blade-axis*blade.dot(axis)
            if projected.length<1e-5:
                break
            u = projected.normalized()
            v = axis.cross(u).normalized()
            hand_dot = self.grip.left_attachment.inverted().to_3x3().col[1].z
            cosine = ((wrist-center).dot(blade)-lower*hand_dot)/(radius*projected.length)
            cosine = max(-1.,min(1.,cosine))
            sine = math.sqrt(max(0.,1-cosine*cosine))
            choices = [center+radius*(u*cosine+v*sine),center+radius*(u*cosine-v*sine)]
            elbow = min(choices,key=lambda p:(p-anchor).length_squared)
            self.base.aim_elbows({'L':elbow},0.,sides=('L',))
            self.grip.solve_left_grip(1.)
        self.previous_left = self.game.pose.bones['LeftForeArm'].head.copy()

    def pose(self,label,frame):
        if self.previous_sample is None or self.previous_sample[0]!=label or frame<=self.previous_sample[1]:
            self.previous_left = None
        feet = super().pose(label,frame)
        self.solve_left_elbow()
        self.previous_sample = (label,frame)
        return feet

    def author(self,label):
        assert label in self.supported_labels
        for window in bpy.context.window_manager.windows:
            if window.screen.is_animation_playing:
                with bpy.context.temp_override(window=window):
                    bpy.ops.screen.animation_cancel(restore_frame=False)
        name = 'REFERENCE_REVIEW_'+label
        if name in bpy.data.actions:
            raise RuntimeError('Review Action already exists: '+name)
        source_end = float(self.refs[label]['rig'].animation_data.action.frame_range[1])
        end = round((source_end-1)*30/24)+1
        action = bpy.data.actions.new(name)
        action.use_fake_user = True
        self.control.animation_data.action = action
        self.previous_sample = None
        previous = {}
        previous_roll = None
        samples = []
        report = {'wrist_max':{'R':0.,'L':0.},'palm_max_m':0.,'floor_min_m':100.,
                  'foot_error_m':0.,'feet_min_m':100.,'arm_joint_gap_m':0.}
        for frame in np.arange(1,end+.001,.5):
            source_frame = 1+(frame-1)*(source_end-1)/(end-1)
            feet = self.pose(label,source_frame)
            snapshot = self.capture()
            self.scene.frame_set(int(frame),subframe=float(frame%1))
            self.restore(snapshot)
            roll = self.control['left_grip_roll']
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
                    bone.keyframe_insert(channel,frame=frame,group=bone_name)
            for prop in ('two_hand_grip','left_grip_roll'):
                self.control.keyframe_insert(f'["{prop}"]',frame=frame,group='Weapon')
            for side in ('R','L'):
                for prefix in ('upper_arm_parent.','thigh_parent.'):
                    self.control.pose.bones[prefix+side].keyframe_insert('["IK_FK"]',frame=frame,group='IK')
                self.control.pose.bones['upper_arm_parent.'+side].keyframe_insert('["pole_vector"]',frame=frame,group='IK')
            points = self.helpers['evaluate_foot_surface']()
            heights = {}
            for side,target in feet.items():
                height = float(np.min(points[self.helpers['foot_ids'][side],2]))
                heights[side] = height
                report['feet_min_m'] = min(report['feet_min_m'],height)
                report['foot_error_m'] = max(report['foot_error_m'],abs(height-target))
            for side,angle in self.grip.wrist_angles().items():
                prefix = 'Right' if side=='R' else 'Left'
                report['wrist_max'][side] = max(report['wrist_max'][side],angle)
                hand = self.game.pose.bones[prefix+'Hand']
                target = self.grip.sword.matrix_world@Vector((0,0,.055 if side=='R' else -.085))
                report['palm_max_m'] = max(report['palm_max_m'],(hand.matrix@self.grip.palms[side]-target).length)
                report['arm_joint_gap_m'] = max(report['arm_joint_gap_m'],(self.game.pose.bones[prefix+'ForeArm'].tail-hand.head).length)
            report['floor_min_m'] = min(report['floor_min_m'],min((self.grip.sword.matrix_world@v.co).z for v in bpy.data.objects['Sword_LOD0'].data.vertices))
            samples.append({'frame':float(frame),'source_frame':source_frame,'hip_z':self.game.pose.bones['Hips'].head.z,'feet':heights})
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if bag:
                        for curve in bag.fcurves:
                            for point in curve.keyframe_points:
                                point.interpolation = 'LINEAR'
        for key,value in dict(name='MB_C_'+label,start_frame=1,end_frame=end,
            duration_seconds=(end-1)/30,loop=False,reference='MB_C_Stance',layer='FullBody',
            root_motion='in_place',two_handed=True,source_reference=self.refs[label]['filepath'],
            source_fps=24,source_start=1,source_end=source_end,
            vertical_motion='hips_and_feet' if label=='JumpAttack' else 'body_weight_shift',
            motion_revision=self.revision).items():
            action[key] = value
        def output_frame(source_frame):
            return round(1+(source_frame-1)*(end-1)/(source_end-1))
        markers = self.source_markers[label]
        for marker,frame in markers.items():
            action.pose_markers.new(marker).frame = output_frame(frame)
        if label=='JumpAttack':
            apex = max(samples,key=lambda item:item['hip_z'])
            index = samples.index(apex)
            start = index
            while start>0 and min(samples[start]['feet'].values())>.025:
                start -= 1
            landing = index
            while landing<len(samples)-1 and min(samples[landing]['feet'].values())>.015:
                landing += 1
            action.pose_markers['Takeoff'].frame = math.ceil(samples[start+1]['frame'])
            action.pose_markers['Apex'].frame = round(apex['frame'])
            action.pose_markers['Land'].frame = math.ceil(samples[landing]['frame'])
            report['airborne_interval'] = [samples[start+1]['frame'],samples[landing]['frame']]
            report['peak_both_feet_clearance_m'] = max(min(item['feet'].values()) for item in samples)
        action['refinement_report'] = json.dumps(report)
        self.scene.frame_start = 1
        self.scene.frame_end = end
        self.scene.frame_set(1)
        bpy.app.driver_namespace[self.sample_prefix+label] = samples
        print('EXTENDED_ATTACK '+label+' '+json.dumps(report),flush=True)
        return action


def create_session():
    session = ExtendedAttacks()
    bpy.app.driver_namespace['spin_jump_session'] = session
    return session
