"""通常ジャンプの3区間と、ガード姿勢へ戻る受け止めの反動を制作する。"""

import importlib.util
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Euler, Vector

TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('jump_pose_tools',TOOLS/'refine_minibot_c_run_slash.py')
pose_tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pose_tools)


class JumpGuardMotions:
    ends = {'JumpStart':19,'FallLoop':31,'Land':22,'GuardHit':16}
    flight = {
        'shift':(0,-.035,.150),'hips':(0,0,-7),'chest':(12,0,4),'head':(-7,0,-3),
        'sword_shift':(-.060,-.090,.200),'sword_rot':(8,-12,-5),
        'foot_R_shift':(.025,.130,0),'foot_L_shift':(-.025,.090,0),
        'foot_R_rot':(30,0,-4),'foot_L_rot':(32,0,5),
        'foot_R_height':(.430,),'foot_L_height':(.450,),
    }

    def __init__(self):
        self.base = pose_tools.MotionRefinement()
        self.control = self.base.control
        self.game = self.base.game
        self.grip = self.base.grip
        self.scene = self.base.scene
        self.helpers = self.base.helpers
        self.keyed = list(self.base.keyed)+['shoulder.L','shoulder.R','neck']
        self.anchors = {name:self.capture_anchor('MB_C_'+name) for name in ('Idle','Blocking')}

    def capture_anchor(self, action_name):
        for name,matrix in self.helpers['rest'].items():
            self.control.pose.bones[name].matrix_basis = matrix
        action = bpy.data.actions[action_name]
        self.control.animation_data.action = action
        self.control.animation_data.action_slot = action.slots[0]
        self.scene.frame_set(1)
        self.grip.update()
        elbows = {q:self.game.pose.bones[p+'ForeArm'].head.copy() for q,p in [('R','Right'),('L','Left')]}
        self.base.aim_elbows(elbows,0.)
        points = self.helpers['evaluate_foot_surface']()
        return {'bones':{b.name:b.matrix_basis.copy() for b in self.control.pose.bones},
                'torso':self.control.pose.bones['torso'].matrix.copy(),
                'sword':self.grip.sword.matrix_world.copy(),'roll':float(self.control['left_grip_roll']),
                'feet':{q:self.control.pose.bones['foot_ik.'+q].matrix.copy() for q in ('R','L')},
                'clearance':{q:float(points[self.helpers['foot_ids'][q],2].min()) for q in ('R','L')},
                'elbows':elbows,
                'shoulders':{q:self.game.pose.bones[p+'Arm'].head.copy() for q,p in [('R','Right'),('L','Left')]}}

    def restore(self, anchor):
        for name,matrix in anchor['bones'].items():
            self.control.pose.bones[name].matrix_basis = matrix
        self.control['two_hand_grip'] = 1.
        self.control['left_grip_roll'] = anchor['roll']
        for side in ('L','R'):
            self.control.pose.bones['upper_arm_parent.'+side]['pole_vector'] = True
            self.control.pose.bones['upper_arm_parent.'+side]['IK_FK'] = 0.
            self.control.pose.bones['thigh_parent.'+side]['IK_FK'] = 0.
        self.grip.update()

    @staticmethod
    def rotation(angles):
        return Euler(tuple(math.radians(float(v)) for v in angles),'XYZ').to_matrix()

    def rotate_world(self, name, angles):
        bone = self.control.pose.bones[name]
        current = bone.matrix.copy()
        matrix = (self.rotation(angles)@current.to_3x3()).to_4x4()
        matrix.translation = current.translation
        bone.matrix = matrix
        self.grip.update()

    def parameters(self, label, frame):
        zero = {key:np.zeros(len(value)) for key,value in self.flight.items()}
        flight = {key:np.array(value,dtype=float) for key,value in self.flight.items()}
        if label=='FallLoop':
            t = (frame-1)/30
            pulse = (1-math.cos(math.tau*t))*.5
            sway = math.sin(math.tau*t)*math.sin(math.pi*t)**2
            flight['shift'] += (.010*sway,0,.030*pulse)
            flight['chest'] += (2*pulse,0,2*sway)
            flight['head'] += (-pulse,0,-2*sway)
            flight['sword_shift'] += (0,-.015*pulse,.025*pulse)
            flight['foot_R_shift'] += (-.025*pulse,-.180*pulse,0)
            flight['foot_R_rot'] += (-18*pulse,0,3*pulse)
            flight['foot_R_height'] -= .135*pulse
            flight['foot_L_height'] -= .035*pulse
            return flight
        if label=='JumpStart':
            tracks = {
                'shift':{1:(0,0,0),7:(0,-.045,-.230),10:(0,-.045,-.145),12:(0,-.035,.090),19:flight['shift']},
                'hips':{1:(0,0,0),7:(0,0,-4),12:(0,0,-7),19:flight['hips']},
                'chest':{1:(0,0,0),7:(20,0,0),12:(3,0,4),19:flight['chest']},
                'head':{1:(0,0,0),7:(-9,0,0),12:(-2,0,-3),19:flight['head']},
                'sword_shift':{1:(0,0,0),7:(-.025,-.120,-.120),12:(-.045,-.080,.140),19:flight['sword_shift']},
                'sword_rot':{1:(0,0,0),7:(12,-5,0),12:(4,-10,-5),19:flight['sword_rot']},
                'foot_R_shift':{1:(0,0,0),11:(0,0,0),16:(.025,.170,0),19:flight['foot_R_shift']},
                'foot_L_shift':{1:(0,0,0),11:(0,0,0),19:flight['foot_L_shift']},
                'foot_R_rot':{1:(0,0,0),9:(0,0,0),12:(24,0,0),16:(34,0,-4),19:flight['foot_R_rot']},
                'foot_L_rot':{1:(0,0,0),9:(0,0,0),12:(24,0,0),19:flight['foot_L_rot']},
                'foot_R_height':{1:(0,),11:(0,),14:(.285,),16:(.470,),19:flight['foot_R_height']},
                'foot_L_height':{1:(0,),11:(0,),14:(.260,),19:flight['foot_L_height']},
            }
        elif label=='Land':
            tracks = {
                'shift':{1:flight['shift'],4:(0,-.040,-.010),8:(0,-.070,-.250),14:(0,-.020,.025),18:(0,-.010,-.015),22:(0,0,0)},
                'hips':{1:flight['hips'],8:(0,0,4),14:(0,0,-2),22:(0,0,0)},
                'chest':{1:flight['chest'],8:(22,0,-3),14:(3,0,1),18:(1,0,0),22:(0,0,0)},
                'head':{1:flight['head'],8:(-10,0,3),14:(2,0,-1),22:(0,0,0)},
                'sword_shift':{1:flight['sword_shift'],8:(-.040,-.140,-.140),14:(-.010,-.025,.035),18:(0,-.010,-.010),22:(0,0,0)},
                'sword_rot':{1:flight['sword_rot'],8:(14,-9,3),14:(-2,-2,-1),22:(0,0,0)},
                'foot_R_shift':{1:flight['foot_R_shift'],2.5:(.010,-.040,0),4:(0,0,0),22:(0,0,0)},
                'foot_L_shift':{1:flight['foot_L_shift'],4:(0,0,0),22:(0,0,0)},
                'foot_R_rot':{1:flight['foot_R_rot'],2.5:(8,0,-1),4:(0,0,0),22:(0,0,0)},
                'foot_L_rot':{1:flight['foot_L_rot'],4:(0,0,0),22:(0,0,0)},
                'foot_R_height':{1:flight['foot_R_height'],2.5:(.220,),4:(0,),22:(0,)},
                'foot_L_height':{1:flight['foot_L_height'],4:(0,),22:(0,)},
            }
        else:
            tracks = {
                'shift':{1:(0,0,0),3:(.005,.035,-.018),6:(.008,.045,-.045),10:(.003,.020,-.020),16:(0,0,0)},
                'hips':{1:(0,0,0),6:(0,0,-2),16:(0,0,0)},
                'chest':{1:(0,0,0),3:(-5,0,-3),6:(-3,0,-2),10:(1,0,1),16:(0,0,0)},
                'head':{1:(0,0,0),4:(-3,0,2),9:(1,0,0),16:(0,0,0)},
                'sword_shift':{1:(0,0,0),3:(.005,.025,-.015),6:(.008,.030,-.025),10:(0,.010,-.010),16:(0,0,0)},
                'sword_rot':{1:(0,0,0),3:(-3,0,-4),6:(-2,0,-3),10:(1,0,1),16:(0,0,0)},
            }
        return {key:self.base.sample(tracks[key],frame) if key in tracks else value for key,value in zero.items()}

    def pose(self, label, frame):
        anchor = self.anchors['Blocking' if label=='GuardHit' else 'Idle']
        self.restore(anchor)
        if (label=='JumpStart' and frame==1) or (label=='Land' and frame==22) or (label=='GuardHit' and frame in (1,16)):
            return
        p = self.parameters(label,frame)
        torso = anchor['torso'].copy()
        torso.translation += Vector(p['shift'])
        self.control.pose.bones['torso'].matrix = torso
        self.grip.update()
        for name in ('hips','chest','head'):
            self.rotate_world(name,p[name])
        targets = {}
        for side in ('R','L'):
            source = anchor['feet'][side]
            matrix = (self.rotation(p['foot_'+side+'_rot'])@source.to_3x3()).to_4x4()
            matrix.translation = source.translation+Vector(p['foot_'+side+'_shift'])
            self.control.pose.bones['foot_ik.'+side].matrix = matrix
            targets[side] = anchor['clearance'][side]+float(p['foot_'+side+'_height'][0])
        self.grip.update()
        for _ in range(4):
            points = self.helpers['evaluate_foot_surface']()
            for side,target in targets.items():
                bone = self.control.pose.bones['foot_ik.'+side]
                matrix = bone.matrix.copy()
                matrix.translation.z += target-float(points[self.helpers['foot_ids'][side],2].min())
                bone.matrix = matrix
            self.grip.update()
        sword = (self.rotation(p['sword_rot'])@anchor['sword'].to_3x3()).to_4x4()
        sword.translation = anchor['sword'].translation+Vector(p['sword_shift'])
        self.grip.set_hand('R',sword@self.grip.attachment.inverted())
        chest = self.rotation(p['chest'])
        elbows = {side:self.game.pose.bones[prefix+'Arm'].head+chest@(anchor['elbows'][side]-anchor['shoulders'][side])
                  for side,prefix in [('R','Right'),('L','Left')]}
        self.base.aim_elbows(elbows,0.)
        self.grip.limit_right_wrist(32 if label=='GuardHit' else 31.5)
        self.grip.update()

    def author(self, label):
        assert label in self.ends
        name = 'REFERENCE_REVIEW_'+label
        if name in bpy.data.actions:
            raise RuntimeError('Action already exists: '+label)
        action = bpy.data.actions.new(name)
        action.use_fake_user = True
        self.control.animation_data.action = action
        previous = {}
        end = self.ends[label]
        for frame in np.arange(1,end+.001,.5):
            self.scene.frame_set(int(frame),subframe=float(frame%1))
            self.pose(label,float(frame))
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
        for key,value in dict(name='MB_C_'+label,start_frame=1,end_frame=end,duration_seconds=(end-1)/30,
            loop=label=='FallLoop',reference='MB_C_Blocking' if label=='GuardHit' else 'MB_C_Idle',
            layer='FullBody' if label=='GuardHit' else 'Base',root_motion='in_place',two_handed=True,
            source_type='authored_from_existing_poses',
            motion_revision='2026-09-14-jump-guard-hit' if label=='GuardHit' else '2026-09-14-active-right-leg-jump',
            vertical_motion='pose_only_game_controls_height' if label!='GuardHit' else 'body_weight_shift').items():
            action[key] = value
        markers = {
            'JumpStart':{'Ready':1,'Anticipation':7,'PushOff':10,'Takeoff':12,'AirPose':19},
            'FallLoop':{'AirLoop':1},
            'Land':{'AirPose':1,'Contact':4,'Compress':8,'Rebound':14,'Settle':18,'Ready':22},
            'GuardHit':{'Guard':1,'Impact':3,'Absorb':6,'Recover':10,'GuardReady':16},
        }
        for name,frame in markers[label].items():
            action.pose_markers.new(name).frame = frame
        self.scene.render.fps = 30
        self.scene.render.fps_base = 1
        self.scene.frame_start = 1
        self.scene.frame_end = end-1 if label=='FallLoop' else end
        self.scene.frame_set(1)
        return action


def create_session():
    session = JumpGuardMotions()
    bpy.app.driver_namespace['jump_guard_session'] = session
    return session
