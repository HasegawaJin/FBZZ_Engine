"""勝利の拳上げ、敗北の膝つき、体を逃がす回避を明確なシルエットで制作する。"""

import importlib.util
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector


TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('clear_result_base', TOOLS/'author_minibot_c_locomotion_result.py')
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)


class ClearResultDodge(previous.LocomotionResultMotions):
    ends = {'Dodge':19, 'Victory':121, 'DefeatIdle':121}

    def __init__(self):
        super().__init__()
        foot_ids = set(self.helpers['foot_ids']['R']) | set(self.helpers['foot_ids']['L'])
        self.non_foot_ids = [v.index for v in self.base.body.data.vertices if v.index not in foot_ids]

    def parameters(self, label, frame):
        zero = {k:np.zeros(len(v)) for k,v in self.flight.items()}
        zero['shoulder_drop'] = np.zeros(1)
        if label == 'Dodge':
            tracks = {
                'shift':{1:(0,0,0),3:(.040,.015,-.260),5:(.280,.120,-.170),7:(.340,.180,-.210),10:(.270,.130,-.130),14:(.090,.040,-.110),19:(0,0,0)},
                'hips':{1:(0,0,0),5:(0,0,-28),9:(0,0,-20),14:(0,0,-8),19:(0,0,0)},
                'chest':{1:(0,0,0),3:(25,0,-8),5:(22,0,-20),8:(12,0,-18),13:(12,0,-6),19:(0,0,0)},
                'head':{1:(0,0,0),3:(-8,0,8),5:(-10,0,18),9:(-4,0,15),19:(0,0,0)},
                'sword_shift':{1:(0,0,0),3:(0,.050,-.160),5:(.200,.180,-.120),7:(.260,.230,-.160),10:(.220,.170,-.080),14:(.060,.070,-.065),19:(0,0,0)},
                'sword_rot':{1:(0,0,0),3:(15,-10,-10),5:(32,-25,-24),8:(25,-18,-20),14:(10,-5,-5),19:(0,0,0)},
                'foot_R_shift':{1:(0,0,0),3:(0,0,0),8:(.220,.140,0),11:(.220,.140,0),15:(0,0,0),19:(0,0,0)},
                'foot_L_shift':{1:(0,0,0),2:(0,0,0),6:(.220,.140,0),15:(.220,.140,0),19:(0,0,0)},
                'foot_R_height':{1:(0,),3:(0,),5:(.140,),8:(0,),11:(0,),13:(.075,),15:(0,),19:(0,)},
                'foot_L_height':{1:(0,),2:(0,),4:(.090,),6:(0,),15:(0,),17:(.085,),19:(0,)},
                'foot_R_rot':{1:(0,0,0),3:(28,0,0),5:(30,0,5),8:(0,0,8),11:(0,0,8),15:(0,0,0),19:(0,0,0)},
                'foot_L_rot':{1:(0,0,0),2:(0,0,0),3:(14,0,12),6:(0,0,15),15:(0,0,15),19:(0,0,0)},
            }
        elif label == 'Victory':
            tracks = {
                'shift':{1:(0,0,0),14:(0,-.025,-.100),24:(-.025,0,.200),34:(0,-.020,-.100),45:(0,0,.005),66:(.025,0,.005),86:(-.020,0,0),121:(0,0,0)},
                'hips':{1:(0,0,-5),24:(0,0,-10),45:(0,0,5),66:(0,0,-4),90:(0,0,6),121:(0,0,-5)},
                'chest':{1:(-10,0,5),14:(8,0,0),24:(-16,0,-5),34:(10,0,0),45:(-13,0,8),66:(-12,0,-7),95:(-8,0,5),121:(-10,0,5)},
                'head':{1:(-8,0,3),14:(2,0,0),24:(-14,0,-8),34:(-3,0,0),45:(-10,0,12),66:(-12,0,-12),95:(-5,0,8),121:(-8,0,3)},
                'foot_R_shift':{1:(-.060,0,0),121:(-.060,0,0)},
                'foot_L_shift':{1:(.060,0,0),121:(.060,0,0)},
                'foot_R_height':{1:(0,),17:(0,),24:(.120,),32:(0,),121:(0,)},
                'foot_L_height':{1:(0,),17:(0,),24:(.160,),33:(0,),121:(0,)},
                'foot_R_rot':{1:(0,0,-8),17:(12,0,-8),24:(22,0,-8),32:(0,0,-8),121:(0,0,-8)},
                'foot_L_rot':{1:(0,0,8),17:(8,0,8),24:(25,0,8),33:(0,0,8),121:(0,0,8)},
                'shoulder_drop':{1:(-5,),24:(-10,),34:(-3,),45:(-8,),66:(-10,),95:(-5,),121:(-5,)},
            }
        elif label == 'DefeatIdle':
            phase = (frame-1)/120*math.tau
            pulse = (1-math.cos(phase))*.5
            sway = math.sin(phase)*math.sin(phase*.5)**2
            zero.update({
                'shift':np.array((-.010,-.090,-.430-.025*pulse)),
                'hips':np.array((0,0,-3+3*sway)),
                'chest':np.array((28+7*pulse,0,-3+3*sway)),
                'head':np.array((36+8*pulse,0,10*sway)),
                'foot_R_shift':np.array((.030,.480,0)),
                'foot_L_shift':np.array((-.030,.290,0)),
                'shoulder_drop':np.array((10+3*pulse,)),
            })
            return zero
        else:
            raise ValueError(label)
        return {k:self.base.sample(tracks[k],frame) if k in tracks else v for k,v in zero.items()}

    def reach(self, side):
        prefix = 'Right' if side=='R' else 'Left'
        shoulder = self.game.pose.bones[prefix+'Arm'].head
        limit = .98*(self.game.data.bones[prefix+'Arm'].length+self.game.data.bones[prefix+'ForeArm'].length)
        hand = self.control.pose.bones['hand_ik.'+side]
        matrix = hand.matrix.copy()
        delta = matrix.translation-shoulder
        if delta.length > limit:
            matrix.translation = shoulder+delta.normalized()*limit
            self.grip.set_hand(side,matrix)

    def result_hands(self, label, frame, elbows):
        self.control['two_hand_grip'] = 0.
        self.grip.update()
        if label=='Victory':
            right = self.base.sample({1:(-.320,-.180,1.200),14:(-.300,-.180,1.060),24:(-.370,-.150,1.650),34:(-.360,-.160,1.350),45:(-.380,-.160,1.540),66:(-.370,-.160,1.570),90:(-.330,-.150,1.280),121:(-.320,-.180,1.200)},frame)
            axis = self.base.sample({1:(-.30,-.1,.95),24:(-.40,-.1,.91),45:(-.60,-.05,.80),66:(-.55,-.05,.84),95:(-.30,-.1,.95),121:(-.30,-.1,.95)},frame)
            left = self.base.sample({1:(.350,-.120,1.050),14:(.340,-.180,.980),24:(.400,-.150,1.640),34:(.360,-.150,1.140),44:(.380,-.150,1.520),56:(.350,-.180,1.180),66:(.400,-.150,1.570),80:(.350,-.120,1.150),100:(.350,-.120,1.050),121:(.350,-.120,1.050)},frame)
            curl = 1.
        else:
            hips = self.game.pose.bones['Hips'].head
            right = hips+Vector((-.380,-.040,-.020))
            axis = Vector((-.98,-.1,-.17))
            forehead = self.game.pose.bones['Head'].head+Vector((.170,-.160,.100))
            rest = hips+Vector((.280,-.060,.100))
            amount = self.scalar({1:0,20:0,40:1,65:1,92:0,121:0},frame)
            left = rest.lerp(forehead,amount)
            curl = .25
        reference = Vector((0,-1,0)) if label=='Victory' else Vector((0,0,-1))
        sword = self.grip.blade_frame(axis,reference,Vector(right))
        self.grip.set_hand('R',sword@self.grip.attachment.inverted())
        self.reach('R')
        self.grip.natural_hand('L',Vector(left))
        self.reach('L')
        self.grip.natural_hand('L',self.control.pose.bones['hand_ik.L'].matrix.translation.copy())
        fixed_elbows = {q:self.game.pose.bones[prefix+'Arm'].head+Vector((sign*.350,.060,-.060))
                        for q,prefix,sign in [('R','Right',-1),('L','Left',1)]}
        self.base.aim_elbows(fixed_elbows,0.)
        self.grip.limit_right_wrist(31.5)
        self.grip.natural_hand('L',self.control.pose.bones['hand_ik.L'].matrix.translation.copy())
        self.grip.curl('L',curl)
        self.grip.update()

    def pose(self, label, frame):
        if label in self.loops and frame==self.ends[label]:frame=1.
        anchor = self.anchors['Idle']
        self.restore(anchor)
        if label=='Dodge' and frame in (1,self.ends[label]):return
        p = self.parameters(label,frame)
        torso = anchor['torso'].copy();torso.translation += Vector(p['shift'])
        self.control.pose.bones['torso'].matrix = torso;self.grip.update()
        for name in ('hips','chest','head'):self.rotate_world(name,p[name])
        for side,sign in [('R',-1),('L',1)]:
            self.rotate_world('shoulder.'+side,(0,sign*float(p['shoulder_drop'][0]),0))
        for side in ('R','L'):
            source = anchor['feet'][side]
            matrix = (self.rotation(p['foot_'+side+'_rot'])@source.to_3x3()).to_4x4()
            matrix.translation = source.translation+Vector(p['foot_'+side+'_shift'])
            self.control.pose.bones['foot_ik.'+side].matrix = matrix
        self.grip.update()
        for _ in range(5):
            verts = self.helpers['evaluate_foot_surface']()
            for side in ('R','L'):
                height = anchor['clearance'][side]+float(p['foot_'+side+'_height'][0])
                bone = self.control.pose.bones['foot_ik.'+side];matrix = bone.matrix.copy()
                matrix.translation.z += height-float(verts[self.helpers['foot_ids'][side],2].min())
                bone.matrix = matrix
            self.grip.update()
        if label=='DefeatIdle':
            for _ in range(6):
                verts = self.helpers['evaluate_foot_surface']()
                minimum = float(verts[self.non_foot_ids,2].min())
                if minimum>=.010:break
                bone=self.control.pose.bones['torso'];matrix=bone.matrix.copy()
                matrix.translation.z += (.012-minimum)*1.1
                bone.matrix=matrix;self.grip.update()
        chest = self.rotation(p['chest'])
        elbows = {q:self.game.pose.bones[prefix+'Arm'].head+chest@(anchor['elbows'][q]-anchor['shoulders'][q])
                  for q,prefix in [('R','Right'),('L','Left')]}
        if label in self.loops:
            self.result_hands(label,frame,elbows)
        else:
            sword = (self.rotation(p['sword_rot'])@anchor['sword'].to_3x3()).to_4x4()
            sword.translation = anchor['sword'].translation+Vector(p['sword_shift'])
            self.grip.set_hand('R',sword@self.grip.attachment.inverted())
            self.base.aim_elbows(elbows,.25)
            self.grip.limit_right_wrist(31.)
            roll_weight = self.scalar({1:0,3:.65,5:1,10:.60,19:0},frame)
            self.grip.solve_left_grip()
            self.control['left_grip_roll'] = anchor['roll']+(self.control['left_grip_roll']-anchor['roll'])*roll_weight
            self.grip.update()
            self.base.aim_elbows(elbows,self.scalar({1:0,3:.35,5:.70,10:.45,19:0},frame),sides=('L',))
        self.grip.update()

    def author(self, label):
        action = super().author(label)
        action['motion_revision']='2026-09-14-clear-result-dodge'
        action['two_handed']=label=='Dodge'
        action['authoring_tool']='refine_minibot_c_result_dodge.py'
        if label=='Dodge':
            action['dodge_style']='low_lateral_step'
            action['active_pose_frames']='3-10'
        for marker in list(action.pose_markers):action.pose_markers.remove(marker)
        markers = {
            'Dodge':{'Ready':1,'PushOff':3,'Evade':5,'LeftPlant':6,'RightPlant':8,'RecoverRight':15,'ReadyEnd':19},
            'Victory':{'Proud':1,'Crouch':14,'CelebrateJump':24,'Land':34,'FistPumpA':44,'FistPumpB':66,'ProudReturn':100},
            'DefeatIdle':{'Kneeling':1,'Facepalm':40,'HeadDown':65,'HandDrop':92},
        }
        for name,frame in markers[label].items():action.pose_markers.new(name).frame=frame
        return action


def create_session():
    session=ClearResultDodge()
    bpy.app.driver_namespace['clear_result_session']=session
    return session
