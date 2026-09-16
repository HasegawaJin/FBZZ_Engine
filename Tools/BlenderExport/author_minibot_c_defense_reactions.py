"""剣の防御・被弾・死亡の参照素材を、Player の比率へ合わせる。"""

import importlib.util
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('defense_pose_tools', TOOLS/'author_minibot_c_spin_jump.py')
extended = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extended)


class DefenseReactions(extended.ExtendedAttacks):
    supported_labels = ('Death', 'Hit', 'Blocking', 'ToBlocking', 'BlockingToIdle')
    sole_clearance = .005

    def __init__(self):
        super().__init__()
        # 共通の構えで較正し、ガード切り替え時に剣の取り付け角が変わらないようにする。
        axis = self.weapon_axis['ToBlocking'].copy()
        self.weapon_axis = {label:axis.copy() for label in self.refs}
        self.anchors = {}

    def ensure_references(self):
        collection = bpy.data.collections.get('Animation_References')
        if collection is None:
            collection = bpy.data.collections.new('Animation_References')
            self.scene.collection.children.link(collection)
        result = {}
        downloads = Path('C:/Users/jinhs/Downloads')
        for label in self.supported_labels:
            filename = 'Sword '+('Blocking ToIdle' if label=='BlockingToIdle' else label)+'.fbx'
            path = next((p for p in (downloads/filename, downloads/'AnimationRef'/filename) if p.is_file()), None)
            if path is None:
                raise FileNotFoundError(filename)
            rig = bpy.data.objects.get('REF_'+label)
            if rig is None:
                if bpy.context.object and bpy.context.object.mode!='OBJECT':
                    bpy.ops.object.mode_set(mode='OBJECT')
                before = set(bpy.data.objects)
                bpy.ops.import_scene.fbx(filepath=str(path), use_anim=True, ignore_leaf_bones=True,
                                         automatic_bone_orientation=False)
                imported = [o for o in bpy.data.objects if o not in before]
                rig = next(o for o in imported if o.type=='ARMATURE')
                rig.name = 'REF_'+label
                for obj in imported:
                    for owner in list(obj.users_collection):
                        owner.objects.unlink(obj)
                    collection.objects.link(obj)
                    obj.hide_render = True
                action = rig.animation_data.action
                action.name = 'REF_Source_'+label
                action.use_fake_user = True
                action['source_fps'] = self.scene.render.fps/self.scene.render.fps_base
            result[label] = {'rig':rig, 'filepath':str(path),
                             'fps':float(rig.animation_data.action['source_fps'])}
        self.scene.render.fps = 30
        self.scene.render.fps_base = 1
        return result

    def motion_origin(self, label, frame, source):
        origin = self.boundary[label][0].copy() if label=='Death' else source['Hips'].translation.copy()
        origin.z = 0
        return origin

    def weapon_direction(self, label, frame, source, axis):
        if label=='Death':
            weight = self.ease((frame-8)/7)
            fallen = self.ease((frame-40)/12)
            target = Vector((-.82,-.22,.53)).lerp(Vector((-.94,-.32,.08)),fallen).normalized()
            return axis.lerp(target,weight).normalized()
        return axis

    def weapon_offset(self, label, frame, source):
        if label=='Death':
            fall = self.ease((frame-34)/5)*(1.-self.ease((frame-44)/5))
            return Vector((-.085,0,.035))*self.ease((frame-8)/7)+Vector((-.10,.025,-.03))*fall
        return Vector()

    def capture(self):
        pose = super().capture()
        pose['grip'] = float(self.control['two_hand_grip'])
        return pose

    def restore(self, pose):
        super().restore(pose)
        self.control['two_hand_grip'] = pose.get('grip', 1.)
        for side in ('L', 'R'):
            self.control.pose.bones['upper_arm_parent.'+side]['IK_FK'] = 0.
            self.control.pose.bones['thigh_parent.'+side]['IK_FK'] = 0.
        self.grip.update()

    def blend(self, first, last, weight):
        roll = last['roll']+round((first['roll']-last['roll'])/math.tau)*math.tau
        self.restore({'bones':{n:self.mix(first['bones'][n], last['bones'][n], weight) for n in self.keyed},
                      'roll':first['roll']+(roll-first['roll'])*weight,
                      'grip':first['grip']+(last['grip']-first['grip'])*weight})

    def free_left_hand(self, label, frame):
        if label=='Hit':
            weight = 1.-self.ease((frame-2)/4)*(1.-self.ease((frame-26)/8))
        elif label=='Death':
            weight = 1.-self.ease((frame-39)/9)
        else:
            return
        if weight>=1.-1e-8:
            return
        rig = self.refs[label]['rig']
        source = {b.name.split(':')[-1]:rig.matrix_world@b.matrix for b in rig.pose.bones}
        held_elbow = self.game.pose.bones['LeftForeArm'].head.copy()
        shoulder = self.game.pose.bones['LeftArm'].head.copy()
        upper = (source['LeftForeArm'].translation-source['LeftArm'].translation).normalized()
        lower = (source['LeftHand'].translation-source['LeftForeArm'].translation).normalized()
        elbow = shoulder+upper*self.game.data.bones['LeftArm'].length
        wrist = elbow+lower*self.game.data.bones['LeftForeArm'].length
        reach = (self.game.data.bones['LeftArm'].length+self.game.data.bones['LeftForeArm'].length)*.93
        if (wrist-shoulder).length>reach:
            wrist = shoulder+(wrist-shoulder).normalized()*reach
        if label=='Hit':
            # 参照の肘が完全に伸びる瞬間は、曲げ平面が不定になるため外下向きへ寄せる。
            axis = (wrist-shoulder).normalized()
            radial = elbow-shoulder-axis*(elbow-shoulder).dot(axis)
            preferred = Vector((1,1,-1.3))
            preferred = (preferred-axis*preferred.dot(axis)).normalized()
            direction = preferred.lerp(radial.normalized(),self.ease(radial.length/.06)).normalized()
            elbow = shoulder+axis*.15+direction*.20
        self.control['two_hand_grip'] = 0.
        self.grip.update()
        matrix = self.control.pose.bones['hand_ik.L'].matrix.copy()
        matrix.translation = wrist
        self.grip.set_hand('L', matrix)
        self.base.aim_elbows({'L':elbow}, 0., sides=('L',))
        self.grip.natural_hand('L', wrist)
        self.control['two_hand_grip'] = weight
        self.grip.update()
        self.base.aim_elbows({'L':elbow.lerp(held_elbow,weight)}, 0., sides=('L',))
        self.grip.curl('L', .2+.8*weight)
        self.grip.update()

    def lift_body(self, distance):
        if distance<=0:
            return
        names = ['torso','foot_ik.L','foot_ik.R','hand_ik.L','hand_ik.R',
                 'upper_arm_ik_target.L','upper_arm_ik_target.R']
        matrices = {n:self.control.pose.bones[n].matrix.copy() for n in names}
        for name in names:
            matrix = matrices[name]
            matrix.translation.z += distance
            self.control.pose.bones[name].matrix = matrix
            self.grip.update()

    def correct_floor(self, label):
        if label=='Death':
            points = self.helpers['evaluate_foot_surface']()
            frame = self.scene.frame_current+self.scene.frame_subframe
            clearance = .002+.006*self.ease((frame-1)/5)
            self.lift_body(max(0., clearance-float(points[:,2].min())))

    def pose(self, label, frame):
        feet = super().pose(label, frame)
        self.free_left_hand(label, frame)
        if label=='Death':
            # 長い頭部フィンを接地点にすると胴体が浮くため、倒れ込みに合わせて顔を起こす。
            settle = self.ease((frame-43)/15)
            head = self.control.pose.bones['head']
            matrix = head.matrix.copy()
            head.matrix = matrix@Matrix.Rotation(math.radians(45)*settle,4,'X')@Matrix.Rotation(math.radians(-10)*settle,4,'Y')
            self.grip.update()
            weight = self.ease((frame-46)/3)
            if weight>0:
                sword = self.grip.sword.matrix_world.copy()
                axis = sword.to_3x3().col[2].normalized().lerp(Vector((-.94,.15,.08)).normalized(),weight).normalized()
                elbow = self.game.pose.bones['RightForeArm'].head.copy()
                matrix = self.grip.blade_frame(axis,self.grip.forearm('R'),sword.translation)
                self.grip.set_hand('R',matrix@self.grip.attachment.inverted())
                self.base.aim_elbows({'R':elbow},.9*weight,sides=('R',))
                self.grip.limit_right_wrist(32)
        self.correct_floor(label)
        return feet

    def prepare_anchors(self):
        action = bpy.data.actions['MB_C_Idle']
        self.control.animation_data.action = action
        self.control.animation_data.action_slot = action.slots[0]
        self.scene.frame_set(1)
        self.grip.update()
        elbows = {side:self.game.pose.bones[prefix+'ForeArm'].head.copy()
                  for side,prefix in [('L','Left'),('R','Right')]}
        self.base.aim_elbows(elbows, 0.)
        self.anchors['Idle'] = self.capture()
        self.control.animation_data.action = None
        bpy.data.collections['Animation_References'].hide_viewport = False
        self.previous_sample = None
        self.pose('Blocking', 1)
        self.anchors['Blocking'] = self.capture()

    def key_pose(self, action, frame, previous, previous_roll):
        roll = float(self.control['left_grip_roll'])
        if previous_roll is not None:
            roll += round((previous_roll-roll)/math.tau)*math.tau
        self.control['left_grip_roll'] = roll
        self.grip.update()
        for name in self.keyed:
            bone = self.control.pose.bones[name]
            bone.rotation_mode = 'QUATERNION'
            if name in previous and previous[name].dot(bone.rotation_quaternion)<0:
                bone.rotation_quaternion.negate()
            previous[name] = bone.rotation_quaternion.copy()
            for channel in ('location','rotation_quaternion','scale'):
                bone.keyframe_insert(channel, frame=float(frame), group=name)
        for prop in ('two_hand_grip','left_grip_roll'):
            self.control.keyframe_insert(f'["{prop}"]', frame=float(frame), group='Weapon')
        for side in ('R','L'):
            for prefix in ('upper_arm_parent.','thigh_parent.'):
                self.control.pose.bones[prefix+side].keyframe_insert('["IK_FK"]', frame=float(frame), group='IK')
            self.control.pose.bones['upper_arm_parent.'+side].keyframe_insert('["pole_vector"]', frame=float(frame), group='IK')
        return roll

    def author(self, label):
        assert label in self.supported_labels
        if not self.anchors:
            self.prepare_anchors()
        name = 'REFERENCE_REVIEW_'+label
        if name in bpy.data.actions:
            raise RuntimeError('Review Action already exists: '+name)
        source = self.refs[label]
        source_end = float(source['rig'].animation_data.action.frame_range[1])
        end = round((source_end-1)*30/source['fps'])+1
        action = bpy.data.actions.new(name)
        action.use_fake_user = True
        self.control.animation_data.action = action
        self.previous_sample = None
        previous = {}
        roll = None
        for frame in np.arange(1, end+.001, .5):
            source_frame = 1+(frame-1)*(source_end-1)/(end-1)
            self.pose(label, source_frame)
            current = self.capture()
            if label=='Blocking':
                if frame>=end-4:
                    self.blend(current, self.anchors['Blocking'], self.ease((frame-(end-4))/4))
            elif label=='ToBlocking':
                if frame<=6:
                    self.blend(self.anchors['Idle'], current, self.ease((frame-1)/5))
                if frame>=end-5:
                    self.blend(current, self.anchors['Blocking'], self.ease((frame-(end-5))/5))
            elif label=='BlockingToIdle':
                if frame<=5:
                    self.blend(self.anchors['Blocking'], current, self.ease((frame-1)/4))
                if frame>=end-7:
                    self.blend(current, self.anchors['Idle'], self.ease((frame-(end-7))/7))
            else:
                if frame<=5:
                    self.blend(self.anchors['Idle'], current, self.ease((frame-1)/4))
                if label=='Hit' and frame>=end-8:
                    self.blend(current, self.anchors['Idle'], self.ease((frame-(end-8))/8))
            snapshot = self.capture()
            self.scene.frame_set(int(frame), subframe=float(frame%1))
            self.restore(snapshot)
            self.correct_floor(label)
            roll = self.key_pose(action, frame, previous, roll)
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if bag:
                        for curve in bag.fcurves:
                            for point in curve.keyframe_points:
                                point.interpolation = 'LINEAR'
        for key,value in dict(name='MB_C_'+label, start_frame=1, end_frame=end,
            duration_seconds=(end-1)/30, loop=label=='Blocking', reference='MB_C_Stance',
            layer='FullBody', root_motion='stationary_root_local_fall' if label=='Death' else 'in_place',
            two_handed=label not in ('Hit','Death'), source_reference=source['filepath'],
            source_fps=source['fps'], source_start=1., source_end=source_end,
            motion_revision='2026-09-14-defense-reactions').items():
            action[key] = value
        markers = {'Blocking':{'GuardLoop':1}, 'ToBlocking':{'GuardStart':1,'GuardReady':end},
                   'BlockingToIdle':{'GuardRelease':1,'IdleReady':end},
                   'Hit':{'Impact':5,'Recoil':12,'Recover':27,'IdleReady':end},
                   'Death':{'Impact':5,'Collapse':22,'ReleaseLeft':43,'Grounded':58,'EndHold':end}}
        for name,frame in markers[label].items():
            action.pose_markers.new(name).frame = frame
        self.scene.render.fps = 30
        self.scene.frame_start = 1
        self.scene.frame_end = end-1 if label=='Blocking' else end
        self.scene.frame_set(1)
        print('DEFENSE_REACTION '+label+' '+str(end), flush=True)
        return action


def create_session():
    session = DefenseReactions()
    bpy.app.driver_namespace['defense_reaction_session'] = session
    return session
