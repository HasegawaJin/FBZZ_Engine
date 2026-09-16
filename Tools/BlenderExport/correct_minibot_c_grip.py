"""剣と掌の取り付けを校正し、既存Actionの上半身だけを修正する。"""

import json
import math

import bpy
from mathutils import Matrix, Vector


class GripCorrection:
    def __init__(self):
        self.scene = bpy.context.scene
        self.control = bpy.data.objects['MiniBotC_ControlRig']
        self.game = bpy.data.objects['MiniBotC_Humanoid']
        self.sword = bpy.data.objects['Sword_Control']
        self.target = bpy.data.objects['LeftHand_GripTarget']
        self.left_constraint = self.control.pose.bones['hand_ik.L'].constraints['Two hand grip']
        if self.sword.get('grip_calibration'):
            stored = json.loads(self.sword['grip_calibration'])
            self.old_attachment = Matrix(stored['old_attachment'])
            self.attachment = Matrix(stored['attachment'])
            self.left_attachment = Matrix(stored['left_attachment'])
            self.palms = {side:Vector(value) for side,value in stored['palms'].items()}
            self.twist = stored['twist']
            return
        self.old_attachment = self.game.pose.bones['RightHand'].matrix.inverted() @ self.sword.matrix_world
        rest_r = self.game.data.bones['RightHand'].matrix_local
        rest_l = self.game.data.bones['LeftHand'].matrix_local
        self.palms = {'R': rest_r.inverted() @ Vector((-.377, .048, .708)),
                      'L': rest_l.inverted() @ Vector((.377, .048, .708))}
        old_rest_sword = rest_r @ self.old_attachment
        hand_direction = old_rest_sword.to_3x3().inverted() @ rest_r.to_3x3().col[1]
        self.twist = math.atan2(-hand_direction.y, -hand_direction.x)
        self.attachment = self.old_attachment @ Matrix.Rotation(self.twist, 4, 'Z') @ Matrix.Translation((0,0,-.055))
        # 左手も、柄の軸と掌を通る軸を一致させる。追従先の原点は手首なので握り中心との差を含める。
        old_left = old_rest_sword.copy()
        old_left.translation = Vector((.377, .048, .708))
        hand_direction_l = old_left.to_3x3().inverted() @ rest_l.to_3x3().col[1]
        twist_l = math.atan2(-hand_direction_l.y, -hand_direction_l.x)
        self.left_attachment = rest_l.inverted() @ old_left @ Matrix.Rotation(twist_l, 4, 'Z')

    def update(self):
        self.control.update_tag()
        self.game.update_tag()
        bpy.context.view_layer.update()

    def install(self):
        if self.sword.get('grip_revision'):
            raise RuntimeError('Grip correction already installed; preserve current calibration')
        self.sword.matrix_basis = self.sword.matrix_basis @ Matrix.Rotation(self.twist, 4, 'Z') @ Matrix.Translation((0,0,-.055))
        self.update()

    def forearm(self, side):
        bone = self.game.pose.bones[('Right' if side == 'R' else 'Left') + 'ForeArm']
        return (bone.tail - bone.head).normalized()

    def set_hand(self, side, matrix):
        self.control.pose.bones['hand_ik.' + side].matrix = matrix
        self.update()

    @staticmethod
    def blade_frame(axis, forearm, position):
        z = Vector(axis).normalized()
        projected = Vector(forearm) - z * Vector(forearm).dot(z)
        if projected.length < .01:
            projected = Vector((0, -1, 0)) - z * z.dot(Vector((0, -1, 0)))
        x = -projected.normalized()
        y = z.cross(x).normalized()
        matrix = Matrix((x, y, z)).transposed().to_4x4()
        matrix.translation = position
        return matrix

    def sword_pose(self, position, axis, iterations=5):
        approach = self.forearm('R')
        for _ in range(iterations):
            matrix = self.blade_frame(axis, approach, Vector(position))
            self.set_hand('R', matrix @ self.attachment.inverted())
            approach = self.forearm('R')
        return self.sword.matrix_world.copy()

    def calibrate_left(self, sword_matrix):
        self.control['two_hand_grip'] = 0.0
        self.update()
        center = sword_matrix @ Vector((0, 0, -.085))
        approach = Vector((-.35, -.8, -.3))
        for _ in range(7):
            grip = self.blade_frame(sword_matrix.to_3x3().col[2], approach, center)
            hand = grip @ self.left_attachment.inverted()
            self.set_hand('L', hand)
            approach = self.forearm('L')
        self.target.matrix_parent_inverse = Matrix.Identity(4)
        self.target.matrix_basis = self.sword.matrix_world.inverted() @ hand
        self.control['two_hand_grip'] = 1.0
        self.update()

    def natural_hand(self, side, position, blade_direction=None):
        hand = self.control.pose.bones['hand_ik.' + side]
        matrix = hand.matrix.copy()
        matrix.translation = Vector(position)
        self.set_hand(side, matrix)
        rest = self.game.data.bones[('Right' if side == 'R' else 'Left') + 'Hand'].matrix_local.to_3x3()
        for _ in range(3):
            direction = self.forearm(side)
            if blade_direction is None:
                rotation = rest.col[1].rotation_difference(direction).to_matrix() @ rest
            else:
                z = -Vector(blade_direction)
                z = (z - direction * z.dot(direction)).normalized()
                x = direction.cross(z).normalized()
                z = x.cross(direction).normalized()
                rotation = Matrix((x, direction, z)).transposed()
            matrix = rotation.to_4x4()
            matrix.translation = Vector(position)
            self.set_hand(side, matrix)

    def wrist_angles(self):
        return {side: math.degrees(self.forearm(side).angle(
            self.game.pose.bones[('Right' if side == 'R' else 'Left')+'Hand'].matrix.to_3x3().col[1]))
            for side in ('R', 'L')}

    def idle_pose(self, frame):
        phase = (frame-1) / 72 * math.tau
        position = Vector((-.025 + .003*math.sin(phase), -.28, .930 + .003*math.sin(phase)))
        axis = Vector((-.05, -.2, .978))
        self.sword_pose(position, axis)
        self.solve_left_grip()

    def configure_left_pivot(self):
        pivot = bpy.data.objects.get('LeftHand_GripPivot')
        if pivot is None:
            pivot = bpy.data.objects.new('LeftHand_GripPivot', None)
            self.scene.collection.objects.link(pivot)
        pivot.parent = self.sword
        pivot.matrix_parent_inverse = Matrix.Identity(4)
        pivot.location = (0, 0, -.085)
        pivot.rotation_mode = 'XYZ'
        pivot.rotation_euler = (0, 0, 0)
        pivot.empty_display_type = 'PLAIN_AXES'
        pivot.empty_display_size = .04
        pivot.hide_render = True
        self.control['left_grip_roll'] = 0.0
        self.control.id_properties_ui('left_grip_roll').update(
            description='柄を中心とする左手の捻り。掌の握り位置を保つ。')
        driver = pivot.driver_add('rotation_euler', 2).driver
        driver.expression = 'roll'
        variable = driver.variables.new()
        variable.name = 'roll'
        variable.type = 'SINGLE_PROP'
        variable.targets[0].id = self.control
        variable.targets[0].data_path = '["left_grip_roll"]'
        self.target.parent = pivot
        self.target.matrix_parent_inverse = Matrix.Identity(4)
        self.target.matrix_basis = self.left_attachment.inverted()
        self.update()

    def solve_left_grip(self, weight=1.0):
        self.control['two_hand_grip'] = 1.0
        self.update()
        sword = self.sword.matrix_world.copy()
        center = sword @ Vector((0, 0, -.085))
        for _ in range(5):
            grip = self.blade_frame(sword.to_3x3().col[2], self.forearm('L'), center)
            relative = sword.inverted() @ grip
            roll = math.atan2(relative[1][0], relative[0][0])
            old_roll = self.control['left_grip_roll']
            roll += round((old_roll-roll)/math.tau) * math.tau
            self.control['left_grip_roll'] = roll
            self.update()
        self.control['two_hand_grip'] = weight
        self.update()

    def locomotion_pose(self, label, frame):
        self.control['two_hand_grip'] = 0.0
        self.update()
        phase = (frame-1) / (18 if label == 'Run_F' else 32) * math.tau
        shoulder = self.game.pose.bones['RightArm'].head.copy()
        if label == 'Run_F':
            wrist = shoulder + Vector((-.10, .065+.045*math.cos(phase), -.30+.01*math.sin(phase)**2))
            blade = Vector((0, 1, .2))
        else:
            wrist = shoulder + Vector((-.09, -.025+.035*math.cos(phase), -.32))
            blade = Vector((0, -1, .15))
        self.natural_hand('R', wrist, blade)
        self.natural_hand('L', self.control.pose.bones['hand_ik.L'].matrix.translation.copy())

    def curl(self, side, amount):
        from mathutils import Euler
        for index, stem in enumerate(['f_index', 'f_middle', 'f_ring', 'f_pinky']):
            for joint, base in [(1, .50), (2, .85), (3, .66)]:
                angle = (.10+.02*joint)*(1-amount)+(base+index*.035)*amount
                bone = self.control.pose.bones[f'{stem}.{joint:02d}.{side}']
                bone.rotation_mode = 'QUATERNION'
                bone.rotation_quaternion = Euler((angle,0,0)).to_quaternion()
        for joint, angle in [(1,20),(2,28),(3,18)]:
            bone = self.control.pose.bones[f'thumb.{joint:02d}.{side}']
            bone.rotation_mode = 'QUATERNION'
            bone.rotation_quaternion = Euler((math.radians(angle)*amount,0,0)).to_quaternion()

    def limit_right_wrist(self, maximum=30.0):
        from mathutils import Quaternion
        for _ in range(4):
            bone = self.control.pose.bones['hand_ik.R']
            matrix = bone.matrix.copy()
            hand_axis = matrix.to_3x3().col[1].normalized()
            forearm = self.forearm('R')
            angle = hand_axis.angle(forearm)
            if angle <= math.radians(maximum)+.0001:
                break
            correction = hand_axis.rotation_difference(forearm)
            weight = (angle-math.radians(maximum))/angle
            rotation = Quaternion().slerp(correction,weight).to_matrix() @ matrix.to_3x3()
            result = rotation.to_4x4()
            result.translation = matrix.translation
            self.set_hand('R',result)

    @staticmethod
    def interpolate(keys, frame):
        times = sorted(keys)
        if frame <= times[0]:
            return Vector(keys[times[0]])
        if frame >= times[-1]:
            return Vector(keys[times[-1]])
        for first, last in zip(times,times[1:]):
            if first <= frame <= last:
                t = (frame-first)/(last-first)
                return Vector(keys[first]).lerp(Vector(keys[last]),t*t*(3-2*t))

    def finish_combat(self, label):
        action = bpy.data.actions['MB_C_'+label]
        self.control.animation_data.action = action
        maximum = {'R':0.0,'L':0.0}
        previous = None
        for frame in range(1,int(action['end_frame'])+1):
            self.scene.frame_set(frame)
            weight = self.control['two_hand_grip']
            if label == 'Parry':
                position = self.interpolate({1:(-.025,-.28,.930),3:(-.020,-.32,.960),
                    8:(-.020,-.32,.960),10:(-.010,-.285,.950),12:(-.005,-.355,1.000),
                    16:(-.015,-.31,.965),22:(-.025,-.28,.930)},frame)
                axis = self.interpolate({1:(-.05,-.2,.978),3:(-.55,-.2,.81),
                    8:(-.55,-.2,.81),10:(-.64,-.2,.74),12:(-.20,-.30,.93),
                    16:(-.15,-.23,.96),22:(-.05,-.2,.978)},frame)
                self.sword_pose(position,axis)
            self.limit_right_wrist()
            self.solve_left_grip(weight)
            bone = self.control.pose.bones['hand_ik.R']
            if previous is not None and bone.rotation_quaternion.dot(previous)<0:
                bone.rotation_quaternion.negate()
            previous = bone.rotation_quaternion.copy()
            for channel in ('location','rotation_quaternion','scale'):
                bone.keyframe_insert(channel,frame=frame,group=bone.name)
            self.control.keyframe_insert('["left_grip_roll"]',frame=frame,group='Weapon')
            for side,angle in self.wrist_angles().items():
                maximum[side] = max(maximum[side],angle)
        print('COMBAT_WRISTS '+json.dumps({'action':action.name,'maximum_degrees':maximum}),flush=True)

    def clear_swing_floor(self):
        action = bpy.data.actions['MB_C_Slash01']
        self.control.animation_data.action = action
        corners = [Vector(p) for p in bpy.data.objects['Sword_LOD0'].bound_box]
        previous = None
        previous_world = None
        adjusted = []
        for frame in range(1, int(action['end_frame'])+1):
            self.scene.frame_set(frame)
            self.update()
            hand = self.control.pose.bones['hand_ik.R']
            source = hand.matrix.copy()
            if min((self.sword.matrix_world@v.co).z for v in bpy.data.objects['Sword_LOD0'].data.vertices)<.10:
                candidates = []
                # 前腕に対する手首の曲がりは保ち、柄を握ったまま回内・回外で振り抜く。
                for degrees in range(-180,181,2):
                    candidate = source @ Matrix.Rotation(math.radians(degrees),4,'Y')
                    weapon = candidate @ self.attachment
                    if min((weapon@p).z for p in corners)<.08:
                        continue
                    q = candidate.to_quaternion()
                    distance = previous_world.rotation_difference(q).angle if previous_world is not None else 0
                    distance = min(distance,math.tau-distance)
                    candidates.append((abs(degrees)+math.degrees(distance)*.25,candidate))
                if not candidates:
                    raise RuntimeError('No grounded follow-through at frame '+str(frame))
                self.set_hand('R',min(candidates,key=lambda item:item[0])[1])
                adjusted.append(frame)
            self.solve_left_grip(self.control['two_hand_grip'])
            if previous is not None and hand.rotation_quaternion.dot(previous)<0:
                hand.rotation_quaternion.negate()
            previous = hand.rotation_quaternion.copy()
            previous_world = hand.matrix.to_quaternion()
            for channel in ('location','rotation_quaternion','scale'):
                hand.keyframe_insert(channel,frame=frame,group=hand.name)
            self.control.keyframe_insert('["left_grip_roll"]',frame=frame,group='Weapon')
        print('SWING_FLOOR_FIXED '+json.dumps(adjusted),flush=True)

    def bake(self, label):
        action = bpy.data.actions['MB_C_' + label]
        if action.get('grip_revision'):
            raise RuntimeError('Action already corrected: ' + action.name)
        self.control.animation_data.action = action
        end = int(action['end_frame'])
        keyed = ['hand_ik.R', 'hand_ik.L'] + [f'{stem}.{joint:02d}.{side}'
            for stem in ['f_index','f_middle','f_ring','f_pinky','thumb']
            for joint in range(1,4) for side in ('R','L')]
        max_angles = {'R':0.0, 'L':0.0}
        max_palm_error = {'R':0.0, 'L':0.0}
        min_sword_z = 100.0
        lower_body_delta = 0.0
        previous_quaternions = {}
        first_pose = {}
        for frame in range(1,end+1):
            self.scene.frame_set(frame)
            self.control['two_hand_grip'] = 0.0
            self.update()
            lower = {n:self.game.pose.bones[n].matrix.copy()
                     for n in ('Root','Hips','RightFoot','LeftFoot')}
            old_sword = self.sword.matrix_world.copy()
            left_closed = .35 if label == 'Run_F' else .22 if label == 'Walk_F' else .12
            if label in ('Idle','Stance'):
                self.idle_pose(frame if label == 'Idle' else 1)
                left_closed = 1.0
            elif label in ('Run_F','Walk_F'):
                self.locomotion_pose(label,frame)
                self.control['left_grip_roll'] = 0.0
            else:
                # 開始・復帰の握りを新しい両手Idleにつなぐ。斬撃中は左手を離す。
                if label == 'Slash01':
                    transition = max(0.0, (4-frame)/3, (frame-15)/5)
                    weight = max(0.0, (4-frame)/3, (frame-16)/4)
                else:
                    transition = max(0.0, (3-frame)/2, (frame-16)/6)
                    weight = 1.0
                transition = min(1.0,transition)
                blend = transition*transition*(3-2*transition)
                position = old_sword.translation.lerp(Vector((-.025,-.28,.930)),blend)
                axis = old_sword.to_3x3().col[2].lerp(Vector((-.05,-.2,.978)).normalized(),blend).normalized()
                self.sword_pose(position,axis)
                self.natural_hand('L',self.control.pose.bones['hand_ik.L'].matrix.translation.copy())
                self.solve_left_grip(weight)
                left_closed = .12+.88*weight
            self.curl('R',1.0)
            self.curl('L',left_closed)
            self.update()
            for name in keyed:
                bone = self.control.pose.bones[name]
                bone.rotation_mode = 'QUATERNION'
                if name in previous_quaternions and bone.rotation_quaternion.dot(previous_quaternions[name]) < 0:
                    bone.rotation_quaternion.negate()
                previous_quaternions[name] = bone.rotation_quaternion.copy()
                for channel in ('location','rotation_quaternion','scale'):
                    bone.keyframe_insert(channel,frame=frame,group=name)
            for prop in ('two_hand_grip','left_grip_roll'):
                self.control.keyframe_insert(f'["{prop}"]',frame=frame,group='Weapon')
            angles = self.wrist_angles()
            for side in ('R','L'):
                max_angles[side] = max(max_angles[side],angles[side])
                if side == 'R' or self.control['two_hand_grip'] > .999:
                    hand = self.game.pose.bones[('Right' if side == 'R' else 'Left')+'Hand']
                    palm = hand.matrix @ self.palms[side]
                    target = self.sword.matrix_world @ Vector((0,0,.055 if side == 'R' else -.085))
                    max_palm_error[side] = max(max_palm_error[side],(palm-target).length)
            min_sword_z = min(min_sword_z,min((self.sword.matrix_world@v.co).z
                              for v in bpy.data.objects['Sword_LOD0'].data.vertices))
            lower_body_delta = max(lower_body_delta,max(abs(lower[n][row][col]-self.game.pose.bones[n].matrix[row][col])
                for n in lower for row in range(4) for col in range(4)))
            if frame == 1:
                first_pose = {n:self.game.pose.bones[n].matrix.copy() for n in self.game.pose.bones.keys()}
        for slot in action.slots:
            for layer in action.layers:
                for strip in layer.strips:
                    bag = strip.channelbag(slot)
                    if bag:
                        for curve in bag.fcurves:
                            for point in curve.keyframe_points:
                                point.interpolation = 'LINEAR'
        seam = max(abs(first_pose[p.name][row][col]-p.matrix[row][col])
                   for p in self.game.pose.bones for row in range(4) for col in range(4)) if action.get('loop') else None
        report = dict(action=action.name,max_wrist_deviation_degrees=max_angles,
                      max_palm_error_m=max_palm_error,min_sword_z_m=min_sword_z,
                      lower_body_matrix_delta=lower_body_delta,loop_seam_matrix_delta=seam)
        action['grip_revision'] = '2026-09-14-palm-aligned'
        action['grip_validation'] = json.dumps(report)
        print('GRIP_CHECK '+json.dumps(report),flush=True)
        assert max(max_palm_error.values()) < .015, report
        assert min_sword_z > .015, report
        assert lower_body_delta < .0001, report
        if seam is not None:
            assert seam < .0002, report
        return report


def create_session():
    correction = GripCorrection()
    bpy.app.driver_namespace['mini_grip_correction'] = correction
    return correction
