"""提供された Mixamo の剣アニメーションを MiniBot の体格と両手グリップへ合わせる。"""

import importlib.util
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

TOOLS = Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\BlenderExport')


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem,path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReferenceRetarget:
    def __init__(self):
        self.base = load(TOOLS/'refine_minibot_c_run_slash.py').MotionRefinement()
        self.control = self.base.control
        self.game = self.base.game
        self.grip = self.base.grip
        self.scene = self.base.scene
        self.helpers = self.base.helpers
        self.refs = self.ensure_references()
        self.rest = {}
        self.boundary = {}
        self.weapon_axis = {}
        bpy.data.collections['Animation_References'].hide_viewport = False
        for label,item in self.refs.items():
            rig = item['rig']
            self.rest[label] = {b.name.split(':')[-1]:rig.matrix_world@b.matrix_local for b in rig.data.bones}
            initial = self.sample(label,1)
            first = initial['Hips'].translation.copy()
            last = self.sample(label,float(rig.animation_data.action.frame_range[1]))['Hips'].translation.copy()
            self.boundary[label] = (first,last)
            directions = {q:(initial[q+'HandMiddle1'].translation-initial[q+'Hand'].translation).normalized()
                          for q in ['Right','Left']}
            axis = directions['Right'].cross(directions['Left']).normalized()
            separation = initial['RightHand'].translation-initial['LeftHand'].translation
            if axis.dot(separation)<0:
                axis.negate()
            self.weapon_axis[label] = initial['RightHand'].to_quaternion().inverted()@axis
        self.keyed = list(self.base.keyed)+['shoulder.L','shoulder.R','neck']

    def ensure_references(self):
        collection = bpy.data.collections.get('Animation_References')
        if collection is None:
            collection = bpy.data.collections.new('Animation_References')
            self.scene.collection.children.link(collection)
        result = {}
        for label,filename in [('Run_F','Great Sword Run.fbx'),('Slash01','Great Sword Slash.fbx'),
                               ('HighSpinAttack','Great Sword High Spin Attack.fbx'),
                               ('JumpAttack','Sword Jump Attack.fbx'),
                               ('SlideAttack','Sword Slide Attack.fbx'),
                               ('UnderSlash','Sword UnderSlash.fbx'),
                               ('UnderSlashandUpperSlash','Sword UnderSlashandUpperSlash.fbx'),
                               ('SwordWalk','Sword Walk.fbx')]:
            path = str(Path(r'C:\Users\jinhs\Downloads\AnimationRef')/filename)
            rig = bpy.data.objects.get('REF_'+label)
            if rig is None:
                if bpy.context.object and bpy.context.object.mode!='OBJECT':
                    bpy.ops.object.mode_set(mode='OBJECT')
                before = set(bpy.data.objects)
                bpy.ops.import_scene.fbx(filepath=path,use_anim=True,ignore_leaf_bones=True,automatic_bone_orientation=False)
                imported = [o for o in bpy.data.objects if o not in before]
                rig = next(o for o in imported if o.type=='ARMATURE')
                rig.name = 'REF_'+label
                for obj in imported:
                    for owner in list(obj.users_collection):
                        owner.objects.unlink(obj)
                    collection.objects.link(obj)
                    obj.hide_render = True
                rig.animation_data.action.name = 'REF_Source_'+label
                rig.animation_data.action.use_fake_user = True
                rig.animation_data.action['source_fps'] = self.scene.render.fps/self.scene.render.fps_base
            result[label] = {'rig':rig,'filepath':path,'fps':float(rig.animation_data.action.get('source_fps',24))}
        self.scene.render.fps = 30
        bpy.app.driver_namespace['motion_references'] = result
        return result

    def sample(self,label,frame):
        self.scene.frame_set(int(frame),subframe=frame%1)
        rig = self.refs[label]['rig']
        bpy.context.view_layer.update()
        return {b.name.split(':')[-1]:rig.matrix_world@b.matrix for b in rig.pose.bones}

    def orient(self,name,rotation):
        bone = self.control.pose.bones[name]
        matrix = rotation.to_matrix().to_4x4()
        matrix.translation = bone.matrix.translation
        bone.matrix = matrix
        self.grip.update()

    def fit_grip(self):
        for _ in range(6):
            for side,prefix in [('L','Left'),('R','Right')]:
                desired = self.grip.target.matrix_world.translation if side=='L' else self.control.pose.bones['hand_ik.R'].matrix.translation
                shoulder = self.game.pose.bones[prefix+'Arm'].head
                offset = desired-shoulder
                radius = (self.game.data.bones[prefix+'Arm'].length+self.game.data.bones[prefix+'ForeArm'].length)*getattr(self,'arm_reach',.84)
                if offset.length>radius:
                    bone = self.control.pose.bones['hand_ik.R']
                    matrix = bone.matrix.copy()
                    matrix.translation += offset.normalized()*(radius-offset.length)
                    self.grip.set_hand('R',matrix)

    def weapon_direction(self,label,frame,source,axis):
        return axis

    def weapon_offset(self,label,frame,source):
        return Vector()

    def elbow_weight(self,label):
        return .65 if label=='Run_F' else .9

    def motion_origin(self,label,frame,source):
        start,last = self.boundary[label]
        origin = start.lerp(last,(frame-1)/14) if label=='Run_F' else source['Hips'].translation.copy()
        origin.x = 0 if label=='Run_F' else origin.x
        origin.z = 0
        return origin

    def pose(self,label,frame):
        source = self.sample(label,frame)
        rest = self.rest[label]
        leg_scale = .878
        origin = self.motion_origin(label,frame,source)
        hip = source['Hips'].translation.copy()
        target_hip = (hip-origin)*leg_scale
        target_hip.y += .053
        target_hip.z = hip.z*.84
        for bone in self.control.pose.bones:
            bone.matrix_basis = self.helpers['rest'][bone.name]
        self.control['two_hand_grip'] = 0.
        for side in ('R','L'):
            self.control.pose.bones['upper_arm_parent.'+side]['pole_vector'] = False
            self.control.pose.bones['upper_arm_parent.'+side]['IK_FK'] = 0.
            self.control.pose.bones['thigh_parent.'+side]['IK_FK'] = 0.
        self.grip.update()
        def delta(name):
            return source[name].to_quaternion()@rest[name].to_quaternion().inverted()
        torso = delta('Hips').to_matrix().to_4x4()
        torso.translation = target_hip+Vector((0,.0175,.0475))
        self.control.pose.bones['torso'].matrix = torso
        self.grip.update()
        for target,ref in [('chest','Spine2'),('neck','Neck'),('head','Head'),('shoulder.R','RightShoulder'),('shoulder.L','LeftShoulder')]:
            self.orient(target,delta(ref)@self.helpers['rest_world'][target].to_quaternion())
        feet = {}
        for side,prefix in [('R','Right'),('L','Left')]:
            name = prefix+'Foot'
            position = target_hip+(source[name].translation-hip)*leg_scale
            rotation = delta(name)@self.helpers['rest_world']['foot_ik.'+side].to_quaternion()
            matrix = rotation.to_matrix().to_4x4()
            matrix.translation = position
            self.control.pose.bones['foot_ik.'+side].matrix = matrix
            landmarks = []
            for landmark in [name,prefix+'ToeBase']:
                point = rest[landmark].translation.copy();point.z = 0
                landmarks.append((source[landmark]@rest[landmark].inverted()@point).z)
            clearance = getattr(self,'sole_clearance',.002)
            feet[side] = max(clearance,min(landmarks)*leg_scale+clearance)
        self.grip.update()
        for _ in range(4):
            points = self.helpers['evaluate_foot_surface']()
            for side,target in feet.items():
                height = float(np.min(points[self.helpers['foot_ids'][side],2]))
                bone = self.control.pose.bones['foot_ik.'+side]
                matrix = bone.matrix.copy();matrix.translation.z += target-height;bone.matrix = matrix
            self.grip.update()
        palms = {}
        directions = {}
        elbows = {}
        wrists = {}
        for side,prefix in [('R','Right'),('L','Left')]:
            shoulder = self.game.pose.bones[prefix+'Arm'].head.copy()
            upper = (source[prefix+'ForeArm'].translation-source[prefix+'Arm'].translation).normalized()
            lower = (source[prefix+'Hand'].translation-source[prefix+'ForeArm'].translation).normalized()
            elbows[side] = shoulder+upper*self.game.data.bones[prefix+'Arm'].length
            wrists[side] = elbows[side]+lower*self.game.data.bones[prefix+'ForeArm'].length
            hand = source[prefix+'Hand'].translation
            knuckle = source[prefix+'HandMiddle1'].translation
            palms[side] = hand.lerp(knuckle,.60)
            directions[side] = (knuckle-hand).normalized()
        axis = source['RightHand'].to_quaternion()@self.weapon_axis[label]
        axis = self.weapon_direction(label,frame,source,axis)
        sword = self.grip.blade_frame(axis,directions['R'],Vector())
        hand = sword@self.grip.attachment.inverted()
        sword.translation = wrists['R']+hand.to_3x3()@self.grip.palms['R']-axis*.055
        sword.translation += self.weapon_offset(label,frame,source)
        if label=='Slash01':
            sword.translation.y += float(self.base.sample({1:[0],14:[0],16:[.07],17.5:[.18],19:[.055],21:[0],27:[0]},frame)[0])
        self.grip.set_hand('R',sword@self.grip.attachment.inverted())
        left = self.grip.blade_frame(axis,directions['L'],sword@Vector((0,0,-.085)))
        relative = sword.inverted()@left
        self.control['left_grip_roll'] = math.atan2(relative[1][0],relative[0][0])
        self.control['two_hand_grip'] = 1.
        self.grip.update()
        for _ in range(4):
            self.fit_grip()
            self.base.aim_elbows(elbows,self.elbow_weight(label))
            self.grip.limit_right_wrist(32)
        reference_roll = self.control['left_grip_roll']
        for _ in range(3):
            self.grip.solve_left_grip(1.)
            limit = math.radians(getattr(self,'left_roll_limit',55))
            self.control['left_grip_roll'] = max(reference_roll-limit,
                min(reference_roll+limit,self.control['left_grip_roll']))
            self.grip.update()
            self.base.aim_elbows(elbows,getattr(self,'left_elbow_weight',.35),sides=('L',))
        self.grip.curl('R',1.);self.grip.curl('L',1.)
        self.grip.update()
        return feet

    def capture(self):
        return {
            'bones':{n:self.control.pose.bones[n].matrix_basis.copy() for n in self.keyed},
            'feet':{q:self.control.pose.bones['foot_ik.'+q].matrix.copy() for q in ('R','L')},
            'roll':float(self.control['left_grip_roll']),
        }

    def restore(self,pose):
        for name,matrix in pose['bones'].items():
            self.control.pose.bones[name].matrix_basis = matrix
        self.control['left_grip_roll'] = pose['roll']
        self.control['two_hand_grip'] = 1.
        for side in ('R','L'):
            self.control.pose.bones['upper_arm_parent.'+side]['pole_vector'] = True
        self.grip.update()

    @staticmethod
    def mix(first,last,weight):
        a,qa,sa = first.decompose()
        b,qb,sb = last.decompose()
        return Matrix.LocRotScale(a.lerp(b,weight),qa.slerp(qb,weight),sa.lerp(sb,weight))

    @staticmethod
    def ease(value):
        t = max(0.,min(1.,value))
        return t*t*(3-2*t)

    def recover(self,first,last,frame,first_clearance,last_clearance):
        weight = self.ease((frame-34)/18)
        pose = {'bones':{n:self.mix(last['bones'][n],first['bones'][n],weight) for n in self.keyed},
                'roll':last['roll']+(first['roll']-last['roll'])*weight}
        self.restore(pose)
        clearances = {}
        for side,start,end in [('R',34,43),('L',42,52)]:
            t = max(0.,min(1.,(frame-start)/(end-start)))
            amount = self.ease(t)
            self.control.pose.bones['foot_ik.'+side].matrix = self.mix(last['feet'][side],first['feet'][side],amount)
            clearances[side] = last_clearance[side]*(1-amount)+first_clearance[side]*amount+.065*math.sin(math.pi*t)
        self.grip.update()
        for _ in range(3):
            points = self.helpers['evaluate_foot_surface']()
            for side,target in clearances.items():
                bone = self.control.pose.bones['foot_ik.'+side]
                matrix = bone.matrix.copy()
                matrix.translation.z += target-float(np.min(points[self.helpers['foot_ids'][side],2]))
                bone.matrix = matrix
            self.grip.update()
        elbows = {q:self.game.pose.bones[p+'ForeArm'].head.copy() for q,p in [('R','Right'),('L','Left')]}
        adjustment = self.ease((frame-34)/6)*self.ease((52-frame)/6)
        reference_roll = self.control['left_grip_roll']
        for _ in range(3):
            self.fit_grip()
            self.base.aim_elbows(elbows,.35*adjustment)
            self.grip.limit_right_wrist(32)
            self.grip.solve_left_grip(1.)
            self.control['left_grip_roll'] = reference_roll+(self.control['left_grip_roll']-reference_roll)*adjustment
            self.grip.update()
        self.base.aim_elbows(elbows,.35*adjustment)
        return clearances

    def author(self,label):
        for window in bpy.context.window_manager.windows:
            if window.screen.is_animation_playing:
                with bpy.context.temp_override(window=window):
                    bpy.ops.screen.animation_cancel(restore_frame=False)
        name = 'REFERENCE_REVIEW_'+label
        if name in bpy.data.actions:
            raise RuntimeError('Review Action already exists: '+name)
        self.control.animation_data.action = None
        first_clearance = self.pose(label,1)
        first = self.capture()
        if label=='Slash01':
            last_clearance = self.pose(label,27)
            last = self.capture()
        action = bpy.data.actions.new(name)
        action.use_fake_user = True
        self.control.animation_data.action = action
        end = 19 if label=='Run_F' else 52
        previous = {}
        previous_roll = None
        report = {'wrist_max':{'R':0.,'L':0.},'palm_max_m':0.,'floor_min_m':100.,'foot_error_m':0.}
        for frame in np.arange(1,end+.001,.5):
            if frame==end:
                feet = first_clearance
                self.restore(first)
            elif label=='Run_F':
                feet = self.pose(label,1+(frame-1)*14/18)
            elif frame<=34:
                feet = self.pose(label,1+(frame-1)*26/33)
            else:
                feet = self.recover(first,last,frame,first_clearance,last_clearance)
            snapshot = self.capture()
            self.scene.frame_set(int(frame),subframe=frame%1)
            self.restore(snapshot)
            roll = self.control['left_grip_roll']
            if previous_roll is not None:
                roll += round((previous_roll-roll)/math.tau)*math.tau
            previous_roll = roll
            self.control['left_grip_roll'] = roll
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
                for control in ('upper_arm_parent.','thigh_parent.'):
                    self.control.pose.bones[control+side].keyframe_insert('["IK_FK"]',frame=frame,group='IK')
                self.control.pose.bones['upper_arm_parent.'+side].keyframe_insert('["pole_vector"]',frame=frame,group='IK')
            points = self.helpers['evaluate_foot_surface']()
            for side,target in feet.items():
                report['foot_error_m'] = max(report['foot_error_m'],abs(float(np.min(points[self.helpers['foot_ids'][side],2]))-target))
            for side,angle in self.grip.wrist_angles().items():
                report['wrist_max'][side] = max(report['wrist_max'][side],angle)
                hand = self.game.pose.bones[('Right' if side=='R' else 'Left')+'Hand']
                target = self.grip.sword.matrix_world@Vector((0,0,.055 if side=='R' else -.085))
                report['palm_max_m'] = max(report['palm_max_m'],(hand.matrix@self.grip.palms[side]-target).length)
            report['floor_min_m'] = min(report['floor_min_m'],min((self.grip.sword.matrix_world@v.co).z for v in bpy.data.objects['Sword_LOD0'].data.vertices))
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
            reference_speed_mps=abs(self.boundary[label][1].y-self.boundary[label][0].y)*.878/.6 if label=='Run_F' else 0.,
            source_reference=self.refs[label]['filepath'],source_fps=24,
            source_start=1,source_end=15 if label=='Run_F' else 27,two_handed=True,
            motion_revision='2026-09-14-reference-retarget').items():
            action[key] = value
        markers = {'CycleStart':1,'OppositeStep':10} if label=='Run_F' else {
            'Windup':8,'WindupPeak':20,'Hit':25,'FollowThrough':34,'Recover':43}
        for name,frame in markers.items():
            action.pose_markers.new(name).frame = frame
        action['refinement_report'] = json.dumps(report)
        self.scene.frame_start = 1
        self.scene.frame_end = end-1 if label=='Run_F' else end
        self.scene.frame_set(1)
        print('REFERENCE_MOTION '+label+' '+json.dumps(report),flush=True)
        return action


def create_session():
    session = ReferenceRetarget()
    bpy.app.driver_namespace['reference_retarget'] = session
    return session
