"""参照の前転を武器付きリグへ移し、剣を離した四点支持の敗北を制作する。"""

import importlib.util
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Euler, Matrix, Vector


TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('roll_result_base', TOOLS/'refine_minibot_c_result_dodge.py')
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)


class RollGroundDefeat(previous.ClearResultDodge):
    ends = {'Dodge':43, 'DefeatIdle':121}
    loops = {'DefeatIdle'}

    def __init__(self):
        super().__init__()
        self.references = {}
        self.reference_rest = {}
        self.reference_paths = {}
        collection = bpy.data.collections['Animation_References']
        collection.hide_viewport = False
        for label,filename in [('StandRoll','Stand To Roll.fbx'),('SprintRoll','Sprinting Forward Roll.fbx')]:
            rig = bpy.data.objects['REF_'+label]
            rig.hide_set(False)
            self.reference_paths[label] = str(Path('C:/Users/jinhs/Downloads/AnimationRef')/filename)
            self.reference_rest[label] = {b.name.split(':')[-1]:rig.matrix_world@b.matrix_local for b in rig.data.bones}
            samples = {}
            end = float(rig.animation_data.action.frame_range[1])
            for frame in np.arange(1,end+.001,.25):
                self.scene.frame_set(int(frame),subframe=float(frame%1))
                self.grip.update()
                samples[round(float(frame)*4)] = {b.name.split(':')[-1]:rig.matrix_world@b.matrix for b in rig.pose.bones}
            self.references[label] = samples
            rig.hide_set(True)
        collection.hide_viewport = True
        self.hand_ids = {}
        for side,prefix in [('R','Right'),('L','Left')]:
            groups = {g.index for g in self.base.body.vertex_groups if g.name.startswith(prefix+'Hand')}
            self.hand_ids[side] = [v.index for v in self.base.body.data.vertices if sum(g.weight for g in v.groups if g.group in groups)>.7]
        self.install_ground_sword()

    def install_ground_sword(self):
        self.control['sword_release'] = 0.
        for action in bpy.data.actions:
            if action.name.startswith('MB_C_') and 'weapon_released' not in action:
                action['weapon_released'] = False
        target = bpy.data.objects.get('Defeat_SwordGround')
        if target is None:
            target = bpy.data.objects.new('Defeat_SwordGround',None)
            self.scene.collection.objects.link(target)
            target.empty_display_size = .08
            target.hide_render = True
        matrix = self.grip.blade_frame(Vector((-1,0,0)),Vector((0,-1,0)),Vector((-.620,.100,0)))
        mesh = bpy.data.objects['Sword_LOD0'].data
        matrix.translation.z += .004-min((matrix@v.co).z for v in mesh.vertices)
        target.matrix_world = matrix
        constraint = self.grip.sword.constraints.get('Result sword release')
        if constraint is None:
            constraint = self.grip.sword.constraints.new('COPY_TRANSFORMS')
            constraint.name = 'Result sword release'
        constraint.target = target
        constraint.target_space = 'WORLD'
        constraint.owner_space = 'WORLD'
        fcurve = constraint.driver_add('influence')
        driver = fcurve.driver
        driver.type = 'AVERAGE'
        for variable in list(driver.variables): driver.variables.remove(variable)
        variable = driver.variables.new()
        variable.name = 'released'
        variable.type = 'SINGLE_PROP'
        variable.targets[0].id = self.control
        variable.targets[0].data_path = '["sword_release"]'
        self.grip.update()

    def source(self, label, frame):
        samples = self.references[label]
        scaled = frame*4
        lower = max(min(samples),min(max(samples),math.floor(scaled)))
        upper = max(min(samples),min(max(samples),lower+1))
        return {name:matrix.lerp(samples[upper][name],scaled-lower) for name,matrix in samples[lower].items()}

    def capture_pose(self):
        return {name:self.control.pose.bones[name].matrix_basis.copy() for name in self.keyed}

    def blend_pose(self, first, second, amount):
        for name in self.keyed:
            self.control.pose.bones[name].matrix_basis = first[name].lerp(second[name],amount)
        self.grip.update()

    def translate_body(self, delta):
        names = ['torso','foot_ik.R','foot_ik.L','hand_ik.R','hand_ik.L','upper_arm_ik_target.R','upper_arm_ik_target.L']
        matrices = {name:self.control.pose.bones[name].matrix.copy() for name in names}
        for name in names:
            bone = self.control.pose.bones[name]
            matrix = matrices[name]
            matrix.translation += delta
            bone.matrix = matrix
            self.grip.update()

    def ground_body(self):
        for _ in range(3):
            minimum = float(self.helpers['evaluate_foot_surface']()[:,2].min())
            if minimum >= .013: break
            self.translate_body(Vector((0,0,.015-minimum)))

    def reference_pose(self, label, frame):
        source = self.source(label,frame)
        rest = self.reference_rest[label]
        self.restore(self.anchors['Idle'])
        self.control['two_hand_grip'] = 0.
        self.grip.update()
        hip = source['Hips'].translation
        target_hip = Vector((0,.035,hip.z*.84))
        def delta(name):
            return source[name].to_quaternion()@rest[name].to_quaternion().inverted()
        torso = delta('Hips').to_matrix().to_4x4()
        torso.translation = target_hip+Vector((0,.0175,.0475))
        self.control.pose.bones['torso'].matrix = torso
        self.grip.update()
        for target,ref in [('chest','Spine2'),('neck','Neck'),('head','Head'),('shoulder.R','RightShoulder'),('shoulder.L','LeftShoulder')]:
            bone = self.control.pose.bones[target]
            matrix = (delta(ref)@self.helpers['rest_world'][target].to_quaternion()).to_matrix().to_4x4()
            matrix.translation = bone.matrix.translation
            bone.matrix = matrix
            self.grip.update()
        for side,prefix in [('R','Right'),('L','Left')]:
            bone = self.control.pose.bones['foot_ik.'+side]
            matrix = (delta(prefix+'Foot')@self.helpers['rest_world'][bone.name].to_quaternion()).to_matrix().to_4x4()
            matrix.translation = target_hip+(source[prefix+'Foot'].translation-hip)*.878
            bone.matrix = matrix
        self.grip.update()
        elbows = {}
        for side,prefix in [('R','Right'),('L','Left')]:
            shoulder = self.game.pose.bones[prefix+'Arm'].head.copy()
            upper = (source[prefix+'ForeArm'].translation-source[prefix+'Arm'].translation).normalized()
            lower = (source[prefix+'Hand'].translation-source[prefix+'ForeArm'].translation).normalized()
            elbows[side] = shoulder+upper*self.game.data.bones[prefix+'Arm'].length
            wrist = elbows[side]+lower*self.game.data.bones[prefix+'ForeArm'].length
            if side=='L':
                matrix = (delta(prefix+'Hand')@self.game.data.bones[prefix+'Hand'].matrix_local.to_quaternion()).to_matrix().to_4x4()
                matrix.translation = wrist
                self.grip.set_hand(side,matrix)
            else:
                position = shoulder+Vector((-.255,-.015,-.035))
                sword = self.grip.blade_frame(Vector((-1,0,0)),Vector((0,-1,0)),position)
                self.grip.set_hand('R',sword@self.grip.attachment.inverted())
                self.reach('R')
                elbows['R'] = shoulder+Vector((-.18,.16,0))
        self.base.aim_elbows(elbows,0.)
        self.grip.limit_right_wrist(28.)
        self.grip.curl('L',.05)
        self.grip.curl('R',1.)
        self.grip.update()
        self.ground_body()

    def defeat_pose(self, frame):
        phase = (frame-1)/120*math.tau
        pulse = (1-math.cos(phase))*.5
        anchor = self.anchors['Idle']
        self.restore(anchor)
        self.control['two_hand_grip'] = 0.
        torso = (self.rotation((76,0,0))@anchor['torso'].to_3x3()).to_4x4()
        torso.translation = anchor['torso'].translation+Vector((0,-.065,-.405-.008*pulse))
        self.control.pose.bones['torso'].matrix = torso
        self.grip.update()
        self.rotate_world('chest',(17+2*pulse,0,0))
        self.rotate_world('head',(15+3*pulse,0,0))
        for side,sign in [('R',-1),('L',1)]:
            matrix = anchor['feet'][side].copy()
            matrix.translation.x = sign*.16
            matrix.translation.y = .48
            self.control.pose.bones['foot_ik.'+side].matrix = matrix
        self.grip.update()
        for _ in range(4):
            points = self.helpers['evaluate_foot_surface']()
            for side in ('R','L'):
                bone = self.control.pose.bones['foot_ik.'+side]
                matrix = bone.matrix.copy()
                matrix.translation.z += .003-float(points[self.helpers['foot_ids'][side],2].min())
                bone.matrix = matrix
            self.grip.update()
        excluded = set(self.hand_ids['R'])|set(self.hand_ids['L'])
        body_ids = [i for i in self.non_foot_ids if i not in excluded]
        for _ in range(8):
            points = self.helpers['evaluate_foot_surface']()
            minimum = float(points[body_ids,2].min())
            if minimum >= .003: break
            bone = self.control.pose.bones['torso']
            matrix = bone.matrix.copy()
            matrix.translation.z += (.005-minimum)*1.2
            bone.matrix = matrix
            self.grip.update()
        elbows = {}
        for side,prefix,sign in [('R','Right',-1),('L','Left',1)]:
            shoulder = self.game.pose.bones[prefix+'Arm'].head.copy()
            # 指先は前へ、掌面は床と平行にする。
            direction = Vector((0,-1,0))
            normal = Vector((0,0,1))
            matrix = Matrix((direction.cross(normal),direction,normal)).transposed().to_4x4()
            matrix.translation = Vector((sign*.215,-.400,.070))
            self.grip.set_hand(side,matrix)
            self.grip.curl(side,0.)
            for joint,angle in [(1,-25),(2,-20),(3,-15)]:
                self.control.pose.bones[f'thumb.{joint:02d}.{side}'].rotation_quaternion = Euler((math.radians(angle),0,0)).to_quaternion()
            elbows[side] = shoulder+Vector((sign*.2,.07,-.10))
        self.base.aim_elbows(elbows,0.)
        for _ in range(12):
            points = self.helpers['evaluate_foot_surface']()
            for side in ('R','L'):
                bone = self.control.pose.bones['hand_ik.'+side]
                matrix = bone.matrix.copy()
                matrix.translation.z += .004-float(points[self.hand_ids[side],2].min())
                bone.matrix = matrix
            self.grip.update()

    def pose(self, label, frame):
        self.control['sword_release'] = float(label=='DefeatIdle')
        if label=='DefeatIdle':
            self.defeat_pose(1 if frame==121 else frame)
            return
        idle = self.anchors['Idle']
        if frame in (1,self.ends['Dodge']):
            self.restore(idle)
            return
        if frame<=31:
            source_frame = 1+max(0,frame-5)*35/26
            self.reference_pose('SprintRoll',source_frame)
            roll = self.capture_pose()
            if frame<5:
                self.blend_pose(idle['bones'],roll,self.scalar({1:0,5:1},frame))
        else:
            self.reference_pose('SprintRoll',36)
            end = self.capture_pose()
            self.reference_pose('StandRoll',61)
            stand = self.capture_pose()
            if frame<=35:
                self.blend_pose(end,stand,self.scalar({31:0,35:1},frame))
            else:
                self.blend_pose(stand,idle['bones'],self.scalar({35:0,43:1},frame))
        self.control['two_hand_grip'] = self.scalar({1:1,4:0,36:0,42:1,43:1},frame)
        self.control['left_grip_roll'] = idle['roll']
        self.grip.update()
        self.ground_body()

    def author(self, label):
        action = super().author(label)
        for frame in (1,self.ends[label]):
            self.control['sword_release'] = float(label=='DefeatIdle')
            self.control.keyframe_insert('["sword_release"]',frame=frame,group='Weapon')
        action['motion_revision'] = '2026-09-14-reference-roll-ground-defeat'
        action['authoring_tool'] = 'author_minibot_c_roll_ground_defeat.py'
        action['two_handed'] = False
        action['weapon_released'] = label=='DefeatIdle'
        if label=='Dodge':
            action['dodge_style'] = 'forward_shoulder_roll'
            action['source_reference'] = self.reference_paths['SprintRoll']
            action['recovery_reference'] = self.reference_paths['StandRoll']
            action['source_type'] = 'reference_retarget_with_weapon_adaptation'
            action['game_timing_reassessment_required'] = True
            action['active_pose_frames'] = '5-31'
        else:
            action['presentation'] = 'result_loop'
            action['weapon_state'] = 'released_on_ground'
        for marker in list(action.pose_markers): action.pose_markers.remove(marker)
        markers = {'Dodge':{'Ready':1,'EnterRoll':5,'ShoulderRoll':16,'Recover':26,'Stand':35,'ReadyEnd':43},
                   'DefeatIdle':{'HandsAndKnees':1,'Exhale':61}}
        for name,frame in markers[label].items(): action.pose_markers.new(name).frame = frame
        self.grip.update()
        return action


def create_session():
    session = RollGroundDefeat()
    bpy.app.driver_namespace['roll_defeat_session'] = session
    return session
