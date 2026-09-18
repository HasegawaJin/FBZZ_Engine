"""両手の回避・走り停止と、リザルト画面の勝利・敗北ループを制作する。"""

import importlib.util
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector


TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('locomotion_result_base', TOOLS/'author_minibot_c_jump_guard.py')
pose_tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pose_tools)


class LocomotionResultMotions(pose_tools.JumpGuardMotions):
    ends = {'Dodge':12, 'RunStop':28, 'Victory':121, 'DefeatIdle':121}
    loops = {'Victory', 'DefeatIdle'}

    def __init__(self):
        super().__init__()
        self.anchors['Run'] = self.capture_anchor('MB_C_Run_F')

    def scalar(self, keys, frame):
        return float(self.base.sample({f:(v,) for f,v in keys.items()}, frame)[0])

    def parameters(self, label, frame):
        zero = {k:np.zeros(len(v)) for k,v in self.flight.items()}
        zero['shoulder_drop'] = np.zeros(1)
        if label == 'Dodge':
            tracks = {
                'shift':{1:(0,0,0),3:(.015,.040,-.140),5:(.030,.100,-.055),8:(.015,.045,-.125),10:(.005,.010,-.040),12:(0,0,0)},
                'hips':{1:(0,0,0),4:(0,0,-9),8:(0,0,-4),12:(0,0,0)},
                'chest':{1:(0,0,0),3:(10,0,4),5:(-3,0,7),8:(8,0,3),12:(0,0,0)},
                'head':{1:(0,0,0),4:(-4,0,-5),8:(-2,0,-2),12:(0,0,0)},
                'sword_shift':{1:(0,0,0),3:(-.020,-.010,-.070),5:(-.035,.095,-.010),8:(-.020,.020,-.060),12:(0,0,0)},
                'sword_rot':{1:(0,0,0),4:(8,-8,-5),8:(5,-4,-2),12:(0,0,0)},
                'foot_R_shift':{1:(0,0,0),3:(0,0,0),5:(.025,.100,0),9:(0,0,0),12:(0,0,0)},
                'foot_L_shift':{1:(0,0,0),3:(0,0,0),6:(-.015,.080,0),10:(0,0,0),12:(0,0,0)},
                'foot_R_height':{1:(0,),3:(0,),5:(.120,),9:(0,),12:(0,)},
                'foot_L_height':{1:(0,),3:(0,),6:(.080,),10:(0,),12:(0,)},
                'foot_R_rot':{1:(0,0,0),3:(12,0,0),5:(24,0,-4),9:(0,0,0),12:(0,0,0)},
                'foot_L_rot':{1:(0,0,0),3:(8,0,0),6:(18,0,3),10:(0,0,0),12:(0,0,0)},
            }
        elif label == 'RunStop':
            tracks = {
                'shift':{1:(0,0,0),5:(.015,.065,-.095),8:(.015,.080,-.140),14:(0,.030,-.055),24:(0,0,0),28:(0,0,0)},
                'hips':{1:(0,0,0),6:(0,0,-6),14:(0,0,3),24:(0,0,0),28:(0,0,0)},
                'chest':{1:(0,0,0),6:(-12,0,5),14:(3,0,-2),24:(0,0,0),28:(0,0,0)},
                'head':{1:(0,0,0),6:(4,0,-3),14:(-2,0,1),24:(0,0,0),28:(0,0,0)},
                'sword_shift':{1:(0,0,0),6:(-.015,-.025,-.030),14:(-.010,-.010,-.015),24:(0,0,0),28:(0,0,0)},
                'sword_rot':{1:(0,0,0),6:(-5,-4,-3),14:(3,-2,1),24:(0,0,0),28:(0,0,0)},
            }
        elif label == 'Victory':
            tracks = {
                'shift':{1:(.005,0,.015),22:(.015,-.005,.045),42:(.020,-.005,.035),70:(-.015,0,.025),92:(-.005,0,.010),121:(.005,0,.015)},
                'hips':{1:(0,0,-2),22:(0,0,-6),60:(0,0,5),92:(0,0,0),121:(0,0,-2)},
                'chest':{1:(-5,0,2),22:(-10,0,7),42:(-7,0,5),70:(-5,0,-4),92:(-4,0,0),121:(-5,0,2)},
                'head':{1:(-4,0,0),22:(-8,0,-8),55:(-2,0,8),85:(0,0,-6),121:(-4,0,0)},
                'sword_shift':{1:(-.045,-.035,.090),22:(-.005,.040,.300),40:(.005,.045,.340),60:(-.025,.010,.230),90:(-.045,-.035,.090),121:(-.045,-.035,.090)},
                'sword_rot':{1:(-3,-10,-3),22:(-5,-30,-6),40:(-4,-32,-8),60:(-3,-25,-4),90:(-3,-10,-3),121:(-3,-10,-3)},
                'shoulder_drop':{1:(-2,),25:(-5,),60:(-3,),90:(-2,),121:(-2,)},
            }
        elif label == 'DefeatIdle':
            phase = (frame-1)/120*math.tau
            pulse = (1-math.cos(phase))*.5
            sway = math.sin(phase)*math.sin(phase*.5)**2
            zero.update({
                'shift':np.array((-.005+.008*sway,.005,-.075-.025*pulse)),
                'hips':np.array((0,0,-3+2*sway)),
                'chest':np.array((16+4*pulse,0,-2+sway)),
                'head':np.array((18+5*pulse,0,3+4*sway)),
                'sword_shift':np.array((-.005,-.160,-.180-.015*pulse)),
                'sword_rot':np.array((120+3*pulse,-5,0)),
                'shoulder_drop':np.array((6+2*pulse,)),
            })
            return zero
        else:
            raise ValueError(label)
        return {k:self.base.sample(tracks[k],frame) if k in tracks else v for k,v in zero.items()}

    def stop_anchor(self, frame):
        run, idle = self.anchors['Run'], self.anchors['Idle']
        blend = self.scalar({1:0,8:.30,16:.80,24:1,28:1},frame)
        result = {'bones':{name:matrix.lerp(idle['bones'][name],blend) for name,matrix in run['bones'].items()},
                  'roll':run['roll']*(1-blend)+idle['roll']*blend}
        for name in ('torso','sword'):
            result[name] = run[name].lerp(idle[name],blend)
        for name in ('feet','elbows','shoulders'):
            result[name] = {q:run[name][q].lerp(idle[name][q],blend) for q in ('R','L')}
        result['clearance'] = dict(idle['clearance'])
        return result

    def stop_feet(self, frame):
        run, idle = self.anchors['Run'], self.anchors['Idle']
        brake = idle['feet']['L'].copy()
        brake.translation.y = -.210
        right_phase = self.scalar({1:0,8:0,14:1,28:1},frame)
        right = run['feet']['R'].lerp(idle['feet']['R'],right_phase)
        flatten = self.scalar({1:0,4:1,28:1},frame)
        rotation = run['feet']['R'].to_quaternion().slerp(idle['feet']['R'].to_quaternion(),flatten)
        position = right.translation.copy()
        right = rotation.to_matrix().to_4x4();right.translation = position
        if frame <= 5:
            left = run['feet']['L'].lerp(brake,self.scalar({1:0,5:1},frame))
        elif frame <= 16:
            left = brake
        else:
            left = brake.lerp(idle['feet']['L'],self.scalar({16:0,24:1,28:1},frame))
        right_height = idle['clearance']['R']+self.scalar({1:0,8:0,11:.120,14:0,28:0},frame)
        left_height = self.scalar({1:run['clearance']['L'],3:.240,5:idle['clearance']['L'],16:idle['clearance']['L'],20:.112,24:idle['clearance']['L'],28:idle['clearance']['L']},frame)
        return {'R':(right,right_height),'L':(left,left_height)}

    def pose(self, label, frame):
        if label in self.loops and frame == self.ends[label]:
            frame = 1.
        if label == 'RunStop' and frame == 1:
            self.restore(self.anchors['Run']);return
        if (label=='Dodge' and frame in (1,12)) or (label=='RunStop' and frame==28):
            self.restore(self.anchors['Idle']);return
        anchor = self.stop_anchor(frame) if label=='RunStop' else self.anchors['Idle']
        self.restore(anchor)
        p = self.parameters(label,frame)
        torso = anchor['torso'].copy();torso.translation += Vector(p['shift'])
        self.control.pose.bones['torso'].matrix = torso;self.grip.update()
        for name in ('hips','chest','head'):
            self.rotate_world(name,p[name])
        for side,sign in [('R',-1),('L',1)]:
            self.rotate_world('shoulder.'+side,(0,sign*float(p['shoulder_drop'][0]),0))
        feet = self.stop_feet(frame) if label=='RunStop' else {}
        if not feet:
            for side in ('R','L'):
                source = anchor['feet'][side]
                matrix = (self.rotation(p['foot_'+side+'_rot'])@source.to_3x3()).to_4x4()
                matrix.translation = source.translation+Vector(p['foot_'+side+'_shift'])
                feet[side] = (matrix,anchor['clearance'][side]+float(p['foot_'+side+'_height'][0]))
        for side,(matrix,_) in feet.items():
            self.control.pose.bones['foot_ik.'+side].matrix = matrix
        self.grip.update()
        for _ in range(4):
            verts = self.helpers['evaluate_foot_surface']()
            for side,(_,height) in feet.items():
                bone = self.control.pose.bones['foot_ik.'+side];matrix = bone.matrix.copy()
                matrix.translation.z += height-float(verts[self.helpers['foot_ids'][side],2].min())
                bone.matrix = matrix
            self.grip.update()
        sword = (self.rotation(p['sword_rot'])@anchor['sword'].to_3x3()).to_4x4()
        sword.translation = anchor['sword'].translation+Vector(p['sword_shift'])
        self.grip.set_hand('R',sword@self.grip.attachment.inverted())
        chest = self.rotation(p['chest'])
        elbows = {q:self.game.pose.bones[prefix+'Arm'].head+chest@(anchor['elbows'][q]-anchor['shoulders'][q])
                  for q,prefix in [('R','Right'),('L','Left')]}
        elbow_weight = 1. if label=='DefeatIdle' else (.25 if label=='Victory' else 0.)
        self.base.aim_elbows(elbows,elbow_weight)
        self.grip.limit_right_wrist(31.)
        if label in self.loops:
            self.grip.solve_left_grip()
        if label=='DefeatIdle':
            self.base.aim_elbows(elbows,1.,sides=('L',))
        self.grip.update()

    def author(self, label):
        assert label in self.ends
        name = 'REFERENCE_REVIEW_'+label
        if name in bpy.data.actions:
            raise RuntimeError('Action already exists: '+label)
        action = bpy.data.actions.new(name);action.use_fake_user = True
        self.control.animation_data.action = action
        previous = {}
        end = self.ends[label]
        for frame in np.arange(1,end+.001,.5):
            self.scene.frame_set(int(frame),subframe=float(frame%1));self.pose(label,float(frame))
            for name in self.keyed:
                bone = self.control.pose.bones[name];bone.rotation_mode = 'QUATERNION'
                if name in previous and previous[name].dot(bone.rotation_quaternion)<0:
                    bone.rotation_quaternion.negate()
                previous[name] = bone.rotation_quaternion.copy()
                for channel in ('location','rotation_quaternion','scale'):
                    bone.keyframe_insert(channel,frame=float(frame),group=name)
            for prop in ('two_hand_grip','left_grip_roll'):
                self.control.keyframe_insert(f'["{prop}"]',frame=float(frame),group='Weapon')
            for side in ('R','L'):
                for prefix in ('upper_arm_parent.','thigh_parent.'):
                    self.control.pose.bones[prefix+side].keyframe_insert('["IK_FK"]',frame=float(frame),group='IK')
                self.control.pose.bones['upper_arm_parent.'+side].keyframe_insert('["pole_vector"]',frame=float(frame),group='IK')
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if bag:
                        for curve in bag.fcurves:
                            for point in curve.keyframe_points:point.interpolation = 'LINEAR'
        metadata = dict(name='MB_C_'+label,start_frame=1,end_frame=end,duration_seconds=(end-1)/30,
                        loop=label in self.loops,reference='MB_C_Run_F' if label=='RunStop' else 'MB_C_Idle',
                        layer='FullBody' if label in self.loops else 'Base',root_motion='in_place',two_handed=True,
                        source_type='authored_from_existing_poses',motion_revision='2026-09-14-locomotion-result',
                        game_connection='not_connected',presentation='result_loop' if label in self.loops else 'gameplay_clip')
        if label=='Dodge':
            metadata.update(dodge_style='step',game_controls_displacement=True,nominal_game_duration_seconds=.36)
        elif label=='RunStop':
            metadata.update(entry_action='MB_C_Run_F',entry_frame=1,exit_action='MB_C_Idle',game_controls_deceleration=True)
        for key,value in metadata.items():action[key] = value
        markers = {
            'Dodge':{'Ready':1,'PushOff':3,'EvadePose':5,'RightContact':9,'LeftContact':10,'ReadyEnd':12},
            'RunStop':{'RunContact':1,'BrakePlant':5,'Brake':8,'RightPlant':14,'LeftRecover':24,'Ready':28},
            'Victory':{'ProudPose':1,'RaiseSword':22,'Celebrate':40,'LookAround':60,'Settle':90},
            'DefeatIdle':{'DejectedPose':1,'Exhale':61},
        }
        for name,frame in markers[label].items():action.pose_markers.new(name).frame = frame
        self.scene.render.fps = 30;self.scene.render.fps_base = 1
        self.scene.frame_start = 1;self.scene.frame_end = end-1 if label in self.loops else end
        self.scene.frame_set(1)
        return action


def create_session():
    session = LocomotionResultMotions()
    bpy.app.driver_namespace['locomotion_result_session'] = session
    return session
