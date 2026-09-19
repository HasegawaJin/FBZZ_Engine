"""前転の剣先接地と、敗北の掌を下へ向けた支持姿勢を制作する。"""

import importlib.util
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector


spec = importlib.util.spec_from_file_location('roll_weapon_base', Path(__file__).with_name('refine_minibot_c_roll_weapon.py'))
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)


class ContactArm(previous.RollWeaponRefinement):
    def fit_sword_twist(self, sword, shoulder, preferred_elbow):
        upper = self.game.data.bones['RightArm'].length
        lower = self.game.data.bones['RightForeArm'].length
        best = None
        for degrees in range(-180, 180, 3):
            weapon = sword @ Matrix.Rotation(math.radians(degrees), 4, 'Z')
            hand = weapon @ self.grip.attachment.inverted()
            wrist = hand.translation
            distance = (wrist - shoulder).length
            if not abs(upper - lower) + .01 < distance < (upper + lower) * .985:
                continue
            axis = (wrist - shoulder).normalized()
            along = (upper * upper - lower * lower + distance * distance) / (2 * distance)
            center = shoulder + axis * along
            radius = math.sqrt(max(0., upper * upper - along * along))
            hand_axis = hand.to_3x3().col[1].normalized()
            radial = wrist - hand_axis * lower - center
            radial -= axis * radial.dot(axis)
            if radial.length < 1e-5:
                continue
            elbow = center + radial.normalized() * radius
            angle = math.degrees(hand_axis.angle((wrist - elbow).normalized()))
            cost = angle + max(0., angle - 31.) * 30 + (elbow - preferred_elbow).length * 30
            if getattr(self, 'previous_hand', None) is not None:
                change = math.degrees(self.previous_hand.rotation_difference(hand.to_quaternion()).angle)
                cost += min(change, 360 - change) * .15
            if getattr(self, 'previous_elbow', None) is not None:
                cost += (elbow - self.previous_elbow).length * 80
            if best is None or cost < best[0]:
                best = cost, hand, elbow
        if best is None:
            raise RuntimeError('Ground contact target is outside arm reach')
        self.grip.set_hand('R', best[1])
        self.session.base.aim_elbows({'R': best[2]}, 0., sides=('R',))


class GroundContacts:
    def __init__(self, session):
        self.session = session
        self.control = session.control
        self.game = session.game
        self.grip = session.grip
        self.scene = session.scene
        self.weapon_vertices = [v.co.copy() for v in bpy.data.objects['Sword_LOD0'].data.vertices]

    def select(self, action, frame):
        self.control.animation_data.action = action
        self.control.animation_data.action_slot = action.slots[0]
        self.scene.frame_set(int(frame), subframe=float(frame % 1))
        self.grip.update()

    def key(self, action, names, frame, rotations):
        for name in names:
            bone = self.control.pose.bones[name]
            bone.rotation_mode = 'QUATERNION'
            if name in rotations and rotations[name].dot(bone.rotation_quaternion) < 0:
                bone.rotation_quaternion.negate()
            rotations[name] = bone.rotation_quaternion.copy()
            for channel in ('location', 'rotation_quaternion', 'scale'):
                bone.keyframe_insert(channel, frame=frame, group=name)

    def finish(self, action, names):
        for curve in previous.action_curves(action):
            if any(curve.data_path.startswith(f'pose.bones["{name}"]') for name in names):
                for point in curve.keyframe_points:
                    point.interpolation = 'LINEAR'
        action['motion_revision'] = '2026-09-14-ground-contacts'
        action['authoring_tool'] = 'refine_minibot_c_ground_contacts.py'
        return action

    def defeat(self):
        source = bpy.data.actions['MB_C_DefeatIdle']
        assert 'REFERENCE_REVIEW_DefeatIdle' not in bpy.data.actions
        action = source.copy()
        action.name = 'REFERENCE_REVIEW_DefeatIdle'
        action.use_fake_user = True
        self.select(action, 1)
        names = []
        for side, prefix, sign in [('R', 'Right', -1), ('L', 'Left', 1)]:
            # 手のローカルZは掌の法線ではない。モデルの掌面は左右それぞれ内側を向く。
            normal = Vector((-sign, 0, 0))
            forward = Vector((0, 0, -1))
            rest_plane = Matrix((forward.cross(normal), forward, normal)).transposed()
            direction = Vector((sign * .18, -1, 0)).normalized()
            down = Vector((0, 0, -1))
            target_plane = Matrix((direction.cross(down), direction, down)).transposed()
            rotation = target_plane @ rest_plane.transposed()
            hand = (rotation @ self.game.data.bones[prefix + 'Hand'].matrix_local.to_3x3()).to_4x4()
            hand.translation = Vector((sign * .23, -.36, .08))
            self.grip.set_hand(side, hand)
            names.append('hand_ik.' + side)
            for stem in ('f_index', 'f_middle', 'f_ring', 'f_pinky', 'thumb'):
                finger_direction = Vector((-sign * .65, -.76, 0)).normalized() if stem == 'thumb' else direction
                for joint in range(1, 4):
                    name = f'{stem}.{joint:02d}.{side}'
                    names.append(name)
                    bone = self.control.pose.bones[name]
                    matrix = bone.matrix.copy()
                    current = matrix.to_quaternion()
                    correction = (current @ Vector((0, 1, 0))).rotation_difference(finger_direction)
                    target = (correction @ current).to_matrix().to_4x4()
                    target.translation = matrix.translation
                    bone.matrix = target
                    self.grip.update()
        for _ in range(6):
            points = self.session.helpers['evaluate_foot_surface']()
            for side in ('R', 'L'):
                bone = self.control.pose.bones['hand_ik.' + side]
                matrix = bone.matrix.copy()
                matrix.translation.z += .004 - float(points[self.session.hand_ids[side], 2].min())
                bone.matrix = matrix
            self.grip.update()
        hands = {side:self.control.pose.bones['hand_ik.' + side].matrix.copy() for side in ('R', 'L')}
        fingers = {name:self.control.pose.bones[name].matrix_basis.copy() for name in names if not name.startswith('hand_ik')}
        rotations = {}
        for frame in np.arange(1, 121.001, .5):
            self.select(action, float(frame))
            for side in ('R', 'L'):
                self.grip.set_hand(side, hands[side])
            for name, matrix in fingers.items():
                self.control.pose.bones[name].matrix_basis = matrix
            self.grip.update()
            self.key(action, names, float(frame), rotations)
        action['hand_contact'] = 'Anatomical palms face down; fingers extended; fixed supporting hands'
        self.defeat_controls = names
        return self.finish(action, names)

    def dodge(self):
        source = bpy.data.actions['MB_C_Dodge']
        assert 'REFERENCE_REVIEW_Dodge' not in bpy.data.actions
        samples = {}
        for frame in np.arange(1, 43.001, .25):
            self.select(source, float(frame))
            samples[float(frame)] = {
                'sword':self.grip.sword.matrix_world.copy(),
                'shoulder':self.game.pose.bones['RightArm'].head.copy(),
                'elbow':self.game.pose.bones['RightForeArm'].head.copy(),
                'hand':self.control.pose.bones['hand_ik.R'].matrix.copy(),
                'pole':self.control.pose.bones['upper_arm_ik_target.R'].matrix.copy(),
            }
        action = source.copy()
        action.name = 'REFERENCE_REVIEW_Dodge'
        action.use_fake_user = True
        solver = ContactArm.__new__(ContactArm)
        solver.session = self.session
        solver.control = self.control
        solver.game = self.game
        solver.grip = self.grip
        names = ('hand_ik.R', 'upper_arm_ik_target.R')
        rotations = {}
        self.contact_samples = []
        for frame, sample in samples.items():
            self.select(action, frame)
            self.grip.set_hand('R', sample['hand'])
            self.control.pose.bones['upper_arm_ik_target.R'].matrix = sample['pole']
            self.grip.update()
            weight = self.session.scalar({1:0, 5:0, 12:1, 24:1, 33:0, 43:0}, frame)
            if weight <= 0:
                self.key(action, names, frame, rotations)
                solver.previous_hand = sample['hand'].to_quaternion()
                solver.previous_elbow = sample['elbow'].copy()
                continue
            source_sword = sample['sword']
            axis = source_sword.to_3x3().col[2].normalized()
            heading = Vector((axis.x, axis.y, 0)).normalized()
            position = source_sword.translation.copy()
            position += heading * (.085 * math.sin(math.pi * weight))
            contact_z = max(-.95, min(-.01, (.004-position.z)/1.16))
            for _ in range(6):
                target_axis = heading * math.sqrt(1-contact_z*contact_z) + Vector((0,0,contact_z))
                axis_final = axis.lerp(target_axis, weight).normalized()
                sword = self.grip.blade_frame(axis_final, source_sword.to_3x3().col[0], position)
                solver.fit_sword_twist(sword, sample['shoulder'], sample['elbow'])
                lowest = min((self.grip.sword.matrix_world @ v).z for v in self.weapon_vertices)
                if weight < .999:
                    break
                contact_z = max(-.95, min(-.01, contact_z+(.004-lowest)/1.16))
            if weight < .999:
                self.grip.limit_right_wrist(31.5)
            lowest = min((self.grip.sword.matrix_world @ v).z for v in self.weapon_vertices)
            if lowest < .004:
                hand = self.control.pose.bones['hand_ik.R'].matrix.copy()
                hand.translation.z += .004-lowest
                self.grip.set_hand('R', hand)
            self.contact_samples.append([frame, min((self.grip.sword.matrix_world @ v).z for v in self.weapon_vertices)])
            solver.previous_hand = self.control.pose.bones['hand_ik.R'].matrix.to_quaternion()
            solver.previous_elbow = self.game.pose.bones['RightForeArm'].head.copy()
            self.key(action, names, frame, rotations)
        transition = {}
        for frame in np.arange(6.5, 9.001, .25):
            self.select(action, float(frame))
            transition[float(frame)] = {
                'hand':self.control.pose.bones['hand_ik.R'].matrix.copy(),
                'elbow':self.game.pose.bones['RightForeArm'].head.copy(),
                'shoulder':self.game.pose.bones['RightArm'].head.copy(),
            }
        first, last = transition[6.5], transition[9.]
        rotations = {}
        previous_angle = None
        for frame, sample in transition.items():
            self.select(action, frame)
            amount = self.session.scalar({6.5:0, 9:1}, frame)
            hand = first['hand'].lerp(last['hand'], amount)
            hand.translation = sample['hand'].translation
            self.grip.set_hand('R', hand)
            shoulder = self.game.pose.bones['RightArm'].head.copy()
            axis = (hand.translation - shoulder).normalized()
            radial_a = first['elbow'] - first['shoulder']
            radial_b = last['elbow'] - last['shoulder']
            radial_a = (radial_a - axis * radial_a.dot(axis)).normalized()
            radial_b = (radial_b - axis * radial_b.dot(axis)).normalized()
            angle = math.atan2(axis.dot(radial_a.cross(radial_b)), radial_a.dot(radial_b))
            if previous_angle is not None:
                angle += round((previous_angle-angle)/math.tau) * math.tau
            previous_angle = angle
            radial = Quaternion(axis, angle * amount) @ radial_a
            self.session.base.aim_elbows({'R':shoulder+radial*.2}, 0., sides=('R',))
            self.grip.limit_right_wrist(31.5)
            self.key(action, names, frame, rotations)
        action['weapon_motion'] = 'Folded right arm; sword tip slides along floor during frames 12-24; returns to two-handed guard'
        for name, frame in [('SwordContact', 12), ('SwordLift', 24)]:
            marker = action.pose_markers.get(name) or action.pose_markers.new(name)
            marker.frame = frame
        self.dodge_controls = names
        return self.finish(action, names)


def create_session():
    session = GroundContacts(bpy.app.driver_namespace['roll_defeat_session'])
    bpy.app.driver_namespace['ground_contact_session'] = session
    return session
