"""スライド斬り・切り上げ・連続斬りを両手の参照モーションから制作する。"""

import importlib.util
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Quaternion, Vector

TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('extended_attacks',TOOLS/'author_minibot_c_spin_jump.py')
extended = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extended)


class AdditionalAttacks(extended.ExtendedAttacks):
    supported_labels = ('SlideAttack','UnderSlash','UnderSlashandUpperSlash')
    revision = '2026-09-14-slide-under'
    sample_prefix = 'slide_under_samples_'
    sole_clearance = .005
    source_markers = {
        'SlideAttack': {'Entry':1,'Slide':9,'Lowest':16,'Rise':27,'Slash':40,'Recover':53},
        'UnderSlash': {'Windup':10,'WindupPeak':22,'Slash':27,'FollowThrough':35,'Recover':47},
        'UnderSlashandUpperSlash': {'Windup':5,'Slash1':21,'Windup2':30,'Slash2':38,'Recover':47},
    }

    def curve(self,values,frame):
        return float(self.base.sample({f:[v] for f,v in values.items()},frame)[0])

    def elbow_weight(self,label):
        if label=='UnderSlashandUpperSlash':
            frame = self.scene.frame_current+self.scene.frame_subframe
            return self.curve({1:.35,24:.35,26:.9,35:.9,37:.35,55:.35},frame)
        return .9

    def weapon_direction(self,label,frame,source,axis):
        if label=='SlideAttack':
            weight = self.curve({1:0,3:.5,5:1,15:1,19:0,65:0},frame)
            minimum = .85
        elif label=='UnderSlash':
            weight = self.curve({1:0,9:0,12:1,23:1,26:0,56:0},frame)
            target = Vector((.25,.08,.965)).normalized()
            return axis.lerp(target,weight).normalized()
        else:
            if frame<=22:
                weight = self.curve({1:0,8:0,11:1,18:1,22:0},frame)
                minimum = .995
            else:
                if frame<35:
                    weight = self.curve({22:0,26:0,28:1,31:1,34:0,35:0},frame)
                    minimum = .995
                else:
                    weight = self.curve({35:0,36:0,39:1,42:1,46:0,55:0},frame)
                    minimum = .80
        if axis.z<minimum:
            horizontal = Vector((axis.x,axis.y,0)).normalized()
            target = horizontal*math.sqrt(1-minimum*minimum)+Vector((0,0,minimum))
            axis = axis.lerp(target,weight).normalized()
        return axis

    def weapon_offset(self,label,frame,source):
        if label=='SlideAttack':
            x = self.curve({1:0,3:0,5:-.08,9:-.08,15:-.09,19:0,65:0},frame)
            return Vector((x,0,0))
        if label=='UnderSlashandUpperSlash':
            y = self.curve({1:0,8:0,11:.10,18:.10,22:0,55:0},frame)
            x = self.curve({1:0,26:0,28:-.12,32:-.12,35:0,36:0,39:-.13,42:-.13,46:0,55:0},frame)
            return Vector((x,y,0))
        return Vector()

    def support_error(self,step):
        blade = self.grip.sword.matrix_world.to_3x3().col[2].normalized()
        errors = {}
        for side,prefix in [('L','Left'),('R','Right')]:
            hand = self.game.pose.bones[prefix+'Hand']
            wrist = self.grip.target.matrix_world.translation if side=='L' else hand.head
            offset = wrist+blade*step-self.game.pose.bones[prefix+'Arm'].head
            distance = offset.length
            axis = offset.normalized()
            upper = self.game.data.bones[prefix+'Arm'].length
            lower = self.game.data.bones[prefix+'ForeArm'].length
            along = (upper*upper-lower*lower+distance*distance)/(2*distance)
            radius = math.sqrt(max(0.,upper*upper-along*along))
            direction = blade if side=='L' else hand.matrix.to_3x3().col[1].normalized()
            parallel = (offset-axis*along).dot(direction)/lower
            spread = radius*math.sqrt(max(0.,1-axis.dot(direction)**2))/lower
            desired = self.grip.left_attachment.inverted().to_3x3().col[1].z if side=='L' else 1.
            dot = max(parallel-spread,min(parallel+spread,desired))
            angle = abs(math.degrees(math.acos(max(-1.,min(1.,dot)))-math.acos(desired)))
            errors[side] = angle
            errors[side+'_reach'] = max(0.,distance-(upper+lower)*.97)
        return errors

    def refine_support_and_floor(self,action,previous_clearance=.005):
        self.control.animation_data.action = action
        self.control.animation_data.action_slot = action.slots[0]
        frames = list(np.arange(1,float(action['end_frame'])+.001,.5))
        offsets = []
        for frame in frames:
            self.scene.frame_set(int(frame),subframe=float(frame%1))
            self.grip.update()
            choices = []
            for step in np.arange(-.12,.12001,.0025):
                errors = self.support_error(float(step))
                score = max(0.,errors['L']-42)**2+2*max(0.,errors['R']-32)**2
                score += 100000*(errors['L_reach']**2+errors['R_reach']**2)+abs(step)*.05
                choices.append((score,float(step)))
            offsets.append(min(choices)[1])
        # 柄方向の補正を平滑化し、肘の可動域境界で速度が折れないようにする。
        padded = np.pad(offsets,(2,2),mode='edge')
        offsets = np.convolve(padded,np.array([1,4,6,4,1])/16,mode='valid')
        previous = {}
        previous_roll = None
        self.previous_left = None
        for frame,step in zip(frames,offsets):
            self.scene.frame_set(int(frame),subframe=float(frame%1))
            self.grip.update()
            elbow = self.game.pose.bones['RightForeArm'].head.copy()
            matrix = self.control.pose.bones['hand_ik.R'].matrix.copy()
            matrix.translation += self.grip.sword.matrix_world.to_3x3().col[2].normalized()*float(step)
            self.grip.set_hand('R',matrix)
            if self.grip.wrist_angles()['R']>32:
                self.base.aim_elbows({'R':elbow},.9,sides=('R',))
                self.grip.limit_right_wrist(32)
            self.solve_left_elbow()
            for side in ('R','L'):
                bone = self.control.pose.bones['foot_ik.'+side]
                matrix = bone.matrix.copy()
                matrix.translation.z += self.sole_clearance-previous_clearance
                bone.matrix = matrix
            roll = self.control['left_grip_roll']
            if previous_roll is not None:
                roll += round((previous_roll-roll)/math.tau)*math.tau
            self.control['left_grip_roll'] = roll
            previous_roll = roll
            self.grip.update()
            for name in self.keyed:
                bone = self.control.pose.bones[name]
                if name in previous and previous[name].dot(bone.rotation_quaternion)<0:
                    bone.rotation_quaternion.negate()
                previous[name] = bone.rotation_quaternion.copy()
                for channel in ('location','rotation_quaternion','scale'):
                    bone.keyframe_insert(channel,frame=float(frame),group=name)
            self.control.keyframe_insert('["left_grip_roll"]',frame=float(frame),group='Weapon')
        action['sole_clearance_m'] = self.sole_clearance
        action['support_grip_max_offset_m'] = float(max(abs(v) for v in offsets))
        delta = (self.boundary[action['name'][5:]][1]-self.boundary[action['name'][5:]][0])*.878
        action['reference_horizontal_travel_m'] = [delta.x,delta.y,0.]
        self.scene.frame_set(1)
        print('SUPPORT_REFINEMENT '+action.name+' '+str(action['support_grip_max_offset_m']),flush=True)

    @staticmethod
    def align_quaternion_keys(action):
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if not bag:
                        continue
                    groups = {}
                    for curve in bag.fcurves:
                        if curve.data_path.endswith('rotation_quaternion'):
                            groups.setdefault(curve.data_path,{})[curve.array_index] = curve
                    for curves in groups.values():
                        if set(curves)!={0,1,2,3}:
                            continue
                        points = {i:{float(k.co.x):k for k in c.keyframe_points} for i,c in curves.items()}
                        previous = None
                        for frame in sorted(points[0]):
                            q = Quaternion(tuple(points[i][frame].co.y for i in range(4))).normalized()
                            if previous is not None and previous.dot(q)<0:
                                q.negate()
                            for i in range(4):
                                points[i][frame].co.y = q[i]
                            previous = q.copy()
                        for curve in curves.values():
                            curve.update()

    def repair_overhead_transition(self,action):
        # 手首制限が反転する区間は、前後の安定した姿勢を外側の弧でつなぐ。
        s = self
        dest = action
        base = action.copy()
        base.use_fake_user = False
        s.control.animation_data.action=base
        s.control.animation_data.action_slot=base.slots[0]
        poses={}
        for f in (36,42):
            s.scene.frame_set(f);s.grip.update()
            poses[f]={'sword':s.grip.sword.matrix_world.copy(),'bones':{n:s.control.pose.bones[n].matrix_basis.copy() for n in s.keyed},'roll':float(s.control['left_grip_roll']),'elbow':s.game.pose.bones['RightForeArm'].head.copy()}
        s.previous_left=None
        roll_prev=poses[36]['roll']
        quats={}
        for i in range(1,24):
            f=36+i*.25;t=(f-36)/6;u=s.ease(t)
            s.control.animation_data.action=base
            s.control.animation_data.action_slot=base.slots[0]
            s.scene.frame_set(int(f),subframe=f%1);s.grip.update()
            for n in ('upper_arm_ik_target.R','upper_arm_ik_target.L'):
                s.control.pose.bones[n].matrix_basis=s.mix(poses[36]['bones'][n],poses[42]['bones'][n],u)
            sword=s.mix(poses[36]['sword'],poses[42]['sword'],u)
            axis=sword.to_3x3().col[2].normalized()
            axis.x-=.55*math.sin(math.pi*t)**2
            axis.normalize()
            pos=sword.translation.copy();pos.x-=.06*math.sin(math.pi*t)**2
            sword=s.grip.blade_frame(axis,(sword@s.grip.attachment.inverted()).to_3x3().col[1],pos)
            s.grip.set_hand('R',sword@s.grip.attachment.inverted())
            s.fit_grip()
            elbow=poses[36]['elbow'].lerp(poses[42]['elbow'],u)
            s.base.aim_elbows({'R':elbow},.85*math.sin(math.pi*t)**2,sides=('R',))
            s.solve_left_elbow()
            snapshot=s.capture()
            s.control.animation_data.action=dest
            s.control.animation_data.action_slot=dest.slots[0]
            s.scene.frame_set(int(f),subframe=f%1)
            s.restore(snapshot)
            roll=s.control['left_grip_roll'];roll+=round((roll_prev-roll)/math.tau)*math.tau
            roll_prev=roll;s.control['left_grip_roll']=roll
            for n in s.keyed:
                b=s.control.pose.bones[n]
                if n in quats and quats[n].dot(b.rotation_quaternion)<0:b.rotation_quaternion.negate()
                quats[n]=b.rotation_quaternion.copy()
                for ch in ('location','rotation_quaternion','scale'):b.keyframe_insert(ch,frame=f,group=n)
            s.control.keyframe_insert('["left_grip_roll"]',frame=f,group='Weapon')
        
        s.control.animation_data.action=dest
        for i in range(int((float(dest['end_frame'])-42)*4)+1):
            f=42+i*.25
            s.scene.frame_set(int(f),subframe=f%1);s.grip.update()
            s.solve_left_elbow()
            roll=s.control['left_grip_roll'];roll+=round((roll_prev-roll)/math.tau)*math.tau
            roll_prev=roll;s.control['left_grip_roll']=roll
            s.grip.update()
            for n in ('upper_arm_ik_target.L','hand_ik.L'):
                b=s.control.pose.bones[n]
                if n in quats and quats[n].dot(b.rotation_quaternion)<0:b.rotation_quaternion.negate()
                quats[n]=b.rotation_quaternion.copy()
                for ch in ('location','rotation_quaternion','scale'):b.keyframe_insert(ch,frame=f,group=n)
            s.control.keyframe_insert('["left_grip_roll"]',frame=f,group='Weapon')
        
        for slot in dest.slots:
         for layer in dest.layers:
          for strip in layer.strips:
           bag=strip.channelbag(slot)
           if bag:
            for c in bag.fcurves:
             for k in c.keyframe_points:k.interpolation='LINEAR'
        
        self.control.animation_data.action = dest
        self.control.animation_data.action_slot = dest.slots[0]
        self.scene.frame_set(1)
        bpy.data.actions.remove(base)
        action['overhead_refinement_window'] = [36,42]
        self.align_quaternion_keys(action)


    def match_reference_timing(self,action):
        label = action['name'][5:]
        fps = self.refs[label]['fps']
        end = round((float(action['source_end'])-float(action['source_start']))*30/fps)+1
        factor = (end-1)/(float(action['end_frame'])-1)
        # 補正区間は共通ツールの作業用フレームで定義し、最後にFBXの実時間へ合わせる。
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if bag:
                        for curve in bag.fcurves:
                            for key in curve.keyframe_points:
                                key.co.x = 1+(key.co.x-1)*factor
                                key.handle_left.x = 1+(key.handle_left.x-1)*factor
                                key.handle_right.x = 1+(key.handle_right.x-1)*factor
                            curve.update()
        for marker in action.pose_markers:
            marker.frame = round(1+(marker.frame-1)*factor)
        if 'overhead_refinement_window' in action:
            action['overhead_refinement_window'] = [1+(f-1)*factor for f in action['overhead_refinement_window']]
        for sample in bpy.app.driver_namespace.get(self.sample_prefix+label,[]):
            sample['frame'] = 1+(sample['frame']-1)*factor
        action['source_fps'] = fps
        action['end_frame'] = end
        action['duration_seconds'] = (end-1)/30
        action['reference_timing_matched'] = True
        self.scene.frame_end = end
        self.scene.frame_set(1)

    def author(self,label):
        action = super().author(label)
        self.refine_support_and_floor(action)
        if label=='UnderSlashandUpperSlash':
            self.repair_overhead_transition(action)
        self.match_reference_timing(action)
        return action


def create_session():
    session = AdditionalAttacks()
    bpy.app.driver_namespace['additional_attack_session'] = session
    return session
