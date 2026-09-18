"""校正済みの握りを保ち、走行と両手斬撃を独立した確認Actionへ制作する。"""

import importlib.util
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector


TOOLS = Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\BlenderExport')


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MotionRefinement:
    def __init__(self):
        self.grip = load_module('minibot_grip', TOOLS/'correct_minibot_c_grip.py').GripCorrection()
        self.control = self.grip.control
        self.game = self.grip.game
        self.scene = self.grip.scene
        self.body = bpy.data.objects['Player_LOD0']
        self.control['two_hand_grip'] = 0.0
        for bone in self.control.pose.bones:
            bone.matrix_basis = Matrix.Identity(4)
        for side in ('R','L'):
            self.control.pose.bones['upper_arm_parent.'+side]['pole_vector'] = False
            self.control.pose.bones['upper_arm_parent.'+side]['IK_FK'] = 0.
            self.control.pose.bones['thigh_parent.'+side]['IK_FK'] = 0.
        self.grip.update()
        self.helpers = load_module('minibot_walk', TOOLS/'author_minibot_c_walk.py').make_pose_tools(
            self.control,self.game,self.body,self.grip.sword,self.scene)
        self.sample = self.helpers['sample']
        self.keyed = ['root','torso','hips','chest','head','hand_ik.L','hand_ik.R',
                      'foot_ik.L','foot_ik.R','toe_ik.L','toe_ik.R',
                      'upper_arm_ik_target.L','upper_arm_ik_target.R']
        self.keyed += [f'{stem}.{joint:02d}.{side}'
            for stem in ['f_index','f_middle','f_ring','f_pinky','thumb']
            for joint in range(1,4) for side in ('R','L')]

    def body_pose(self, shift, lean, hips, chest, head, feet):
        for bone in self.control.pose.bones:
            bone.matrix_basis = self.helpers['rest'][bone.name]
        self.control['two_hand_grip'] = 0.0
        self.control['left_grip_roll'] = 0.0
        for side in ('R','L'):
            self.control.pose.bones['upper_arm_parent.'+side]['pole_vector'] = False
        torso = self.helpers['rest_world']['torso'].copy()
        rotation = self.helpers['rotation']((lean,0,0)).to_matrix().to_4x4()
        matrix = rotation @ torso
        matrix.translation = torso.translation + Vector(shift)
        self.control.pose.bones['torso'].matrix = matrix
        for name, angles in [('hips',hips),('chest',chest),('head',head)]:
            self.helpers['local_rotation'](name,angles)
        self.grip.update()
        for side,(position,rotation,clearance) in feet.items():
            self.helpers['world_pose']('foot_ik.'+side,position,rotation)
        self.grip.update()
        for _ in range(3):
            points = self.helpers['evaluate_foot_surface']()
            for side,(_,_,clearance) in feet.items():
                height = float(np.min(points[self.helpers['foot_ids'][side],2]))
                bone = self.control.pose.bones['foot_ik.'+side]
                matrix = bone.matrix.copy()
                matrix.translation.z += clearance-height
                bone.matrix = matrix
            self.grip.update()

    def run_pose(self, frame):
        phase = (frame-1) % 20
        angle = phase/20*math.tau
        bounce = float(self.sample({0:[-.045],2:[-.063],5:[-.034],8:[-.016],
            10:[-.045],12:[-.063],15:[-.034],18:[-.016],20:[-.045]},phase)[0])
        feet = {}
        for side,offset,x in [('R',0,-.155),('L',10,.155)]:
            t = (phase+offset) % 20
            if t <= 6:
                y = -.28+.10*t
                clearance = .003
            else:
                u = (t-6)/14
                # 支持脚と遊脚の境界で前後速度を揃え、足が接地直前に止まるのを防ぐ。
                y = ((2*u**3-3*u*u+1)*.32+(u**3-2*u*u+u)*1.4
                     +(-2*u**3+3*u*u)*(-.28)+(u**3-u*u)*1.4)
                clearance = float(self.sample({6:[.003],8:[.075],11:[.180],
                    14:[.145],17:[.045],19:[.008],20:[.003]},t)[0])
            pitch = float(self.sample({0:[-3],2:[0],4:[5],6:[27],8:[33],
                11:[18],14:[2],17:[-6],20:[-3]},t)[0])
            feet[side] = ((x,float(y),.132),(pitch,0,0),clearance)
        twist = -4*math.cos(angle)
        self.body_pose((-.012*math.sin(angle),-.018,bounce-.018),10,
                       (1,0,twist),(2,0,-.8*twist),(-7,0,.25*twist),feet)
        shoulder_r = self.game.pose.bones['RightArm'].head.copy()
        shoulder_l = self.game.pose.bones['LeftArm'].head.copy()
        right = shoulder_r + Vector((-.055,-.16+.045*math.cos(angle),-.17+.012*math.cos(angle)))
        left = shoulder_l + Vector((.055,-.075-.155*math.cos(angle),-.18+.035*math.cos(angle)))
        self.grip.natural_hand('R',right,Vector((-.35,.25,.9)))
        self.grip.natural_hand('L',left)
        self.grip.curl('R',1.0)
        self.grip.curl('L',.40)
        self.grip.update()
        return feet

    def aim_elbows(self, original, weight, sides=('R','L')):
        for side,label in [('R','Right'),('L','Left')]:
            if side not in sides:
                continue
            shoulder = self.game.pose.bones[label+'Arm'].head.copy()
            wrist = self.game.pose.bones[label+'Hand'].head.copy()
            axis = (wrist-shoulder).normalized()
            ideal = wrist-self.game.pose.bones[label+'Hand'].matrix.to_3x3().col[1]*self.game.data.bones[label+'ForeArm'].length
            radial = ideal-shoulder-axis*(ideal-shoulder).dot(axis)
            initial = original[side]-shoulder
            initial = (initial-axis*initial.dot(axis)).normalized()
            radial = initial.lerp(radial.normalized(),weight).normalized()
            self.control.pose.bones['upper_arm_parent.'+side]['pole_vector'] = True
            pole = self.control.pose.bones['upper_arm_ik_target.'+side]
            matrix = pole.matrix.copy()
            matrix.translation = shoulder+axis*.20+radial*.6
            pole.matrix = matrix
            self.grip.update()
            # Rigify の pole_angle とボーンのロールを、評価済みの肘位置から補正する。
            for _ in range(2):
                actual = self.game.pose.bones[label+'ForeArm'].head-shoulder
                actual = (actual-axis*actual.dot(axis)).normalized()
                angle = math.atan2(axis.dot(actual.cross(radial)),actual.dot(radial))
                matrix = pole.matrix.copy()
                matrix.translation = shoulder+Quaternion(axis,angle)@(pole.matrix.translation-shoulder)
                pole.matrix = matrix
                self.grip.update()

    def slash_pose(self, frame):
        frame = frame if frame <= 19 else 19+(frame-19)*9/17
        shift = self.sample({1:(-.008,-.018,-.038),7:(-.020,.012,-.065),
            10:(-.012,-.012,-.073),13:(.015,-.065,-.071),17:(.022,-.060,-.063),
            22:(.005,-.030,-.045),28:(-.008,-.018,-.038)},frame)
        hips = self.sample({1:(0,0,-3),8:(0,0,-10),12:(4,0,5),
            16:(2,0,10),21:(0,0,5),28:(0,0,-3)},frame)
        chest = self.sample({1:(3,0,5),7:(-2,0,-13),10:(1,0,-8),
            13:(8,0,14),17:(6,0,18),22:(3,0,8),28:(3,0,5)},frame)
        head = self.sample({1:(-2,0,-3),8:(1,0,7),13:(-4,0,-8),
            18:(-3,0,-9),28:(-2,0,-3)},frame)
        feet = {'R':((-.23,-.065,.132),(0,0,-4),.002),
                'L':((.23,.155,.132),(0,0,5),.002)}
        self.body_pose(shift,0,hips,chest,head,feet)
        position = self.sample({1:(-.025,-.28,.930),4:(-.075,-.25,1.005),
            8:(-.175,-.215,1.205),10:(-.165,-.215,1.260),
            12:(-.080,-.220,1.270),14:(-.040,-.320,1.130),
            17:(.020,-.310,1.070),19:(.030,-.310,1.050),
            23:(-.025,-.170,1.050),26:(-.025,-.250,1.060),28:(-.025,-.28,.930)},frame)
        axis = self.sample({1:(-.05,-.2,.978),4:(-.25,.02,.96),
            8:(-.50,.30,.812),10:(-.48,.10,.87),12:(-.25,-.92,.30),
            14:(0,-.90,-.436),17:(.20,-.80,-.566),19:(.28,-.76,-.578),
            23:(-.10,-.75,.65),26:(-.05,-.30,.95),
            28:(-.05,-.2,.978)},frame)
        self.grip.sword_pose(Vector(position),Vector(axis))
        self.grip.solve_left_grip(1.0)
        original = {side:self.game.pose.bones[label+'ForeArm'].head.copy()
                    for side,label in [('R','Right'),('L','Left')]}
        weight = float(self.sample({1:[0],6:[1],22:[1],28:[0]},frame)[0])
        for _ in range(3):
            self.aim_elbows(original,weight)
            self.grip.limit_right_wrist(28)
            self.grip.solve_left_grip(1.0)
        self.aim_elbows(original,weight)
        self.grip.curl('R',1.0)
        self.grip.curl('L',1.0)
        self.grip.update()
        return feet

    def author(self, label):
        name = 'REVIEW_'+label
        if name in bpy.data.actions:
            raise RuntimeError('Review Action already exists: '+name)
        action = bpy.data.actions.new(name)
        action.use_fake_user = True
        self.control.animation_data.action = action
        end = 21 if label == 'Run_F' else 36
        previous = {}
        previous_roll = None
        report = {'name':name,'wrist_degrees':{'R':0.,'L':0.},'palm_error_m':0.,
                  'foot_error_m':0.,'contact_error_m':0.,'min_sword_z_m':100.}
        for frame in range(1,end+1):
            self.scene.frame_set(frame)
            feet = self.run_pose(frame) if label == 'Run_F' else self.slash_pose(frame)
            roll = self.control['left_grip_roll']
            if previous_roll is not None:
                roll += round((previous_roll-roll)/math.tau)*math.tau
            self.control['left_grip_roll'] = roll
            previous_roll = roll
            self.grip.update()
            for name in self.keyed:
                bone = self.control.pose.bones[name]
                bone.rotation_mode = 'QUATERNION'
                if name in previous and previous[name].dot(bone.rotation_quaternion)<0:
                    bone.rotation_quaternion.negate()
                previous[name] = bone.rotation_quaternion.copy()
                for channel in ('location','rotation_quaternion','scale'):
                    bone.keyframe_insert(channel,frame=frame,group=name)
            for prop in ('two_hand_grip','left_grip_roll'):
                self.control.keyframe_insert(f'["{prop}"]',frame=frame,group='Weapon')
            for side in ('R','L'):
                self.control.pose.bones['upper_arm_parent.'+side].keyframe_insert(
                    '["pole_vector"]',frame=frame,group='Elbows')
            points = self.helpers['evaluate_foot_surface']()
            for side,(_,_,target) in feet.items():
                height = float(np.min(points[self.helpers['foot_ids'][side],2]))
                report['foot_error_m'] = max(report['foot_error_m'],abs(height-target))
                if target <= .0031:
                    report['contact_error_m'] = max(report['contact_error_m'],abs(height-target))
            for side,angle in self.grip.wrist_angles().items():
                report['wrist_degrees'][side] = max(report['wrist_degrees'][side],angle)
                if side=='R' or label=='Slash01':
                    bone = self.game.pose.bones[('Right' if side=='R' else 'Left')+'Hand']
                    point = bone.matrix @ self.grip.palms[side]
                    target = self.grip.sword.matrix_world @ Vector((0,0,.055 if side=='R' else -.085))
                    report['palm_error_m'] = max(report['palm_error_m'],(point-target).length)
            report['min_sword_z_m'] = min(report['min_sword_z_m'],min((self.grip.sword.matrix_world@v.co).z
                    for v in bpy.data.objects['Sword_LOD0'].data.vertices))
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if bag:
                        for curve in bag.fcurves:
                            for point in curve.keyframe_points:
                                point.interpolation = 'LINEAR'
        for key,value in dict(name='MB_C_'+label,start_frame=1,end_frame=end,
            duration_seconds=(end-1)/30,loop=label=='Run_F',reference='MB_C_Stance',
            layer='Base' if label=='Run_F' else 'Attack',root_motion='in_place',
            reference_speed_mps=3.0 if label=='Run_F' else 0.,
            motion_revision='2026-09-14-run-twohand').items():
            action[key] = value
        markers = {'FootContact_R':1,'ToeOff_R':7,'FootContact_L':11,'ToeOff_L':17} if label=='Run_F' else {
            'Windup':4,'WindupPeak':10,'Hit':14,'FollowThrough':19,'Recover':27}
        for name,frame in markers.items():
            action.pose_markers.new(name).frame = frame
        action['refinement_report'] = json.dumps(report)
        self.scene.frame_start = 1
        self.scene.frame_end = end-1 if label=='Run_F' else end
        self.scene.frame_set(1)
        print('MOTION_REVIEW '+json.dumps(report),flush=True)
        return action


def create_session():
    session = MotionRefinement()
    bpy.app.driver_namespace['run_slash_refinement'] = session
    return session
