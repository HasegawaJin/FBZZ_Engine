"""前転中の右腕を胸元へ畳み、胸の旋回と連動した剣の軌道を制作する。"""

import math

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector


def action_curves(action):
    for slot in action.slots:
        for layer in action.layers:
            for strip in layer.strips:
                bag = strip.channelbag(slot)
                if bag:
                    yield from bag.fcurves


class RollWeaponRefinement:
    edited_bones = ('hand_ik.R','upper_arm_ik_target.R')

    def __init__(self, session):
        self.session = session
        self.control = session.control
        self.game = session.game
        self.grip = session.grip
        self.scene = session.scene
        self.source = bpy.data.actions['MB_C_Dodge']
        self.samples = {}
        self.control.animation_data.action = self.source
        self.control.animation_data.action_slot = self.source.slots[0]
        for frame in np.arange(1,43.001,.5):
            self.scene.frame_set(int(frame),subframe=float(frame%1))
            self.grip.update()
            chest = self.game.pose.bones['UpperChest'].matrix.to_quaternion()@self.game.data.bones['UpperChest'].matrix_local.to_quaternion().inverted()
            self.samples[float(frame)] = {
                'chest':chest,
                'shoulder':self.game.pose.bones['RightArm'].head.copy(),
                'hand':self.control.pose.bones['hand_ik.R'].matrix.copy(),
                'pole':self.control.pose.bones['upper_arm_ik_target.R'].matrix.copy(),
                'elbow':self.game.pose.bones['RightForeArm'].head.copy(),
            }

    def scalar(self, keys, frame):
        return self.session.scalar(keys,frame)

    def fit_sword_twist(self, sword, shoulder, preferred_elbow):
        upper = self.game.data.bones['RightArm'].length
        lower = self.game.data.bones['RightForeArm'].length
        best = None
        for degrees in range(-180,180,3):
            weapon = sword@Matrix.Rotation(math.radians(degrees),4,'Z')
            hand = weapon@self.grip.attachment.inverted()
            wrist = hand.translation
            offset = wrist-shoulder
            distance = offset.length
            if distance >= (upper+lower)*.985 or distance <= abs(upper-lower)+.01:
                continue
            axis = offset.normalized()
            along = (upper*upper-lower*lower+distance*distance)/(2*distance)
            center = shoulder+axis*along
            radius = math.sqrt(max(0.,upper*upper-along*along))
            hand_axis = hand.to_3x3().col[1].normalized()
            radial = wrist-hand_axis*lower-center
            radial -= axis*radial.dot(axis)
            if radial.length<1e-5: continue
            elbow = center+radial.normalized()*radius
            wrist_angle = math.degrees(hand_axis.angle((wrist-elbow).normalized()))
            cost = wrist_angle+(elbow-preferred_elbow).length*45
            if getattr(self,'previous_hand',None) is not None:
                angle = math.degrees(self.previous_hand.rotation_difference(hand.to_quaternion()).angle)
                cost += min(angle,360-angle)*.25
            if getattr(self,'previous_elbow',None) is not None:
                cost += (elbow-self.previous_elbow).length*100
            if best is None or cost<best[0]:
                best = (cost,hand,elbow)
        if best is None:
            raise RuntimeError('Folded wrist target is outside arm reach')
        self.grip.set_hand('R',best[1])
        self.session.base.aim_elbows({'R':best[2]},0.,sides=('R',))

    def pose(self, frame):
        sample = self.samples[frame]
        if frame in (1,43):
            self.grip.set_hand('R',sample['hand'])
            self.control.pose.bones['upper_arm_ik_target.R'].matrix = sample['pole']
            self.grip.update()
            self.previous_hand = sample['hand'].to_quaternion()
            self.previous_elbow = self.game.pose.bones['RightForeArm'].head.copy()
            return
        if frame>27:
            amount = self.scalar({27:0,43:1},frame)
            wave = math.sin(math.pi*amount)
            hand = self.recovery_hand.lerp(self.samples[43.]['hand'],amount)
            hand.translation += Vector((-.030*wave,-.050*wave,.020*wave))
            elbow = self.recovery_elbow.lerp(self.samples[43.]['elbow'],amount)+Vector((-.060*wave,0,0))
            self.grip.set_hand('R',hand)
            self.session.base.aim_elbows({'R':elbow},0.,sides=('R',))
            self.grip.limit_right_wrist(31.5)
            return
        chest = sample['chest']
        shoulder = sample['shoulder']
        outward = chest@Vector((-1,0,0))
        horizontal = Vector((outward.x,outward.y,0)).normalized()
        clearance = self.scalar({1:.035,5:.035,9:.065,13:.100,17:.120,19:.140,25:.140,29:.160,31:.140,35:.070,43:.035},frame)
        rise = self.scalar({1:0,5:0,9:.070,13:0,17:.070,19:.030,21:0,25:0,29:.080,31:.050,35:0,43:0},frame)
        front = self.scalar({1:.160,18:.160,21:.070,22:.070,25:.160,43:.160},frame)
        position = shoulder+chest@Vector((-.100,-front,-.060))+horizontal*clearance+Vector((0,0,rise))
        sweep = self.scalar({1:0,5:20,9:35,13:50,17:50,19:70,20:95,21:95,22:75,23:55,25:45,29:25,35:0,43:0},frame)
        heading = Matrix.Rotation(math.radians(sweep),3,'Z')@horizontal
        lift = self.scalar({1:.72,5:.65,9:.42,13:.30,17:.32,19:.38,21:.42,25:.52,29:.82,35:.86,43:.80},frame)
        axis = heading*math.sqrt(1-lift*lift)+Vector((0,0,lift))
        reference = chest@Vector((0,-.7,.7))
        sword = self.grip.blade_frame(axis,reference,position)
        elbow = shoulder+chest@Vector((-.080,.080,-.200))
        self.fit_sword_twist(sword,shoulder,elbow)
        desired_hand = self.control.pose.bones['hand_ik.R'].matrix.copy()
        desired_elbow = self.game.pose.bones['RightForeArm'].head.copy()
        weight = self.scalar({1:0,5:1,35:1,43:0},frame)
        self.grip.set_hand('R',sample['hand'].lerp(desired_hand,weight))
        axis = (self.control.pose.bones['hand_ik.R'].matrix.translation-shoulder).normalized()
        initial = desired_elbow-shoulder
        initial = (initial-axis*initial.dot(axis)).normalized()
        final = sample['elbow']-shoulder
        final = (final-axis*final.dot(axis)).normalized()
        angle = math.atan2(axis.dot(initial.cross(final)),initial.dot(final))
        if weight<1:
            previous_angle = getattr(self,'previous_blend_angle',None)
            if previous_angle is not None:
                angle += round((previous_angle-angle)/math.tau)*math.tau
            self.previous_blend_angle = angle
        else:
            self.previous_blend_angle = None
        radial = Quaternion(axis,angle*(1-weight))@initial
        self.session.base.aim_elbows({'R':shoulder+radial*.2},0.,sides=('R',))
        if weight<1:
            self.grip.limit_right_wrist(31.5)
        self.previous_hand = self.control.pose.bones['hand_ik.R'].matrix.to_quaternion()
        self.previous_elbow = self.game.pose.bones['RightForeArm'].head.copy()
        if frame==27:
            self.recovery_hand = self.control.pose.bones['hand_ik.R'].matrix.copy()
            self.recovery_elbow = self.previous_elbow.copy()

    def author(self):
        assert 'REFERENCE_REVIEW_Dodge' not in bpy.data.actions
        action = self.source.copy()
        action.name = 'REFERENCE_REVIEW_Dodge'
        action.use_fake_user = True
        self.control.animation_data.action = action
        self.control.animation_data.action_slot = action.slots[0]
        previous = {}
        for frame in self.samples:
            self.scene.frame_set(int(frame),subframe=float(frame%1))
            self.grip.update()
            self.pose(frame)
            for name in self.edited_bones:
                bone = self.control.pose.bones[name]
                bone.rotation_mode = 'QUATERNION'
                if name in previous and previous[name].dot(bone.rotation_quaternion)<0:
                    bone.rotation_quaternion.negate()
                previous[name] = bone.rotation_quaternion.copy()
                for channel in ('location','rotation_quaternion','scale'):
                    bone.keyframe_insert(channel,frame=frame,group=name)
        for curve in action_curves(action):
            if any(curve.data_path.startswith(f'pose.bones["{name}"]') for name in self.edited_bones):
                for point in curve.keyframe_points:
                    point.interpolation = 'LINEAR'
        action['motion_revision'] = '2026-09-14-folded-arm-roll'
        action['authoring_tool'] = 'refine_minibot_c_roll_weapon.py'
        action['weapon_motion'] = 'Folded elbow; chest-relative grip; outward sword sweep; return to two-handed guard'
        for name,frame in [('FoldRightArm',5),('CarrySword',17),('ReturnSword',27)]:
            marker = action.pose_markers.get(name) or action.pose_markers.new(name)
            marker.frame = frame
        self.scene.frame_set(1)
        self.grip.update()
        return action


def create_session():
    refinement = RollWeaponRefinement(bpy.app.driver_namespace['roll_defeat_session'])
    bpy.app.driver_namespace['roll_weapon_session'] = refinement
    return refinement
