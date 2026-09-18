"""一刀の構え・待機・走行・斬撃・パリィを現在の操作リグへ制作する。"""
import bpy
import json
import math
import numpy as np
from pathlib import Path
from mathutils import Vector, Matrix, Euler

ROOT = Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT = ROOT / 'Docs/Art/MiniBotC/Foundation'
OUT.mkdir(parents=True, exist_ok=True)
BLEND = ROOT / 'GreenWare/Assets/_src/MiniBotC/MiniBotC_PlayerMotions.blend'
scene = bpy.context.scene
control = bpy.data.objects['MiniBotC_ControlRig']
rig = bpy.data.objects['MiniBotC_Humanoid']
body = bpy.data.objects['Player_LOD0']
sword = bpy.data.objects['Sword_Control']
scene.render.fps = 30
scene.render.fps_base = 1
if bpy.context.object and bpy.context.object.mode != 'OBJECT':
    bpy.ops.object.mode_set(mode='OBJECT')
control.animation_data.action = bpy.data.actions['Player_NewAction']
scene.frame_set(1)
bpy.context.view_layer.update()
rest = {p.name: p.matrix_basis.copy() for p in control.pose.bones}
rest_world = {p.name: p.matrix.copy() for p in control.pose.bones}
hand_to_sword = rest_world['hand_ik.R'].inverted() @ sword.matrix_world
control.animation_data.action = None
key_bones = ['root', 'torso', 'hips', 'chest', 'head', 'hand_ik.L', 'hand_ik.R',
             'foot_ik.L', 'foot_ik.R', 'toe_ik.L', 'toe_ik.R']
key_bones += [f'{stem}.{joint:02d}.{side}'
              for stem in ['f_index', 'f_middle', 'f_ring', 'f_pinky', 'thumb']
              for joint in range(1, 4) for side in ['L', 'R']]
foot_ids = {}
for side, label in [('R', 'Right'), ('L', 'Left')]:
    groups = {g.index for g in body.vertex_groups if g.name in [label + 'Foot', label + 'ToeBase']}
    foot_ids[side] = [v.index for v in body.data.vertices
                      if sum(g.weight for g in v.groups if g.group in groups) > .55]


def update():
    control.update_tag()
    rig.update_tag()
    bpy.context.view_layer.update()


def sample(keys, frame, linear=False):
    """極値を越えない補間。支持脚の前後移動は別途一定速度で与える。"""
    times = sorted(keys)
    if frame <= times[0]:
        return np.array(keys[times[0]], dtype=float)
    if frame >= times[-1]:
        return np.array(keys[times[-1]], dtype=float)
    i = next(i for i in range(len(times)-1) if times[i] <= frame < times[i+1])
    t1, t2 = times[i:i+2]
    p1, p2 = np.array(keys[t1], dtype=float), np.array(keys[t2], dtype=float)
    u = (frame-t1)/(t2-t1)
    if linear:
        return p1 + (p2-p1)*u
    t0, t3 = times[max(0, i-1)], times[min(len(times)-1, i+2)]
    p0, p3 = np.array(keys[t0], dtype=float), np.array(keys[t3], dtype=float)
    slope = (p2-p1)/(t2-t1)
    m1 = (p2-p0)/max(1, t2-t0)
    m2 = (p3-p1)/max(1, t3-t1)
    m1 = np.where((p1-p0)*(p2-p1) <= 0, 0, m1)
    m2 = np.where((p2-p1)*(p3-p2) <= 0, 0, m2)
    m1 = np.sign(slope)*np.minimum(np.abs(m1), 3*np.abs(slope))
    m2 = np.sign(slope)*np.minimum(np.abs(m2), 3*np.abs(slope))
    return ((2*u**3-3*u*u+1)*p1 + (u**3-2*u*u+u)*(t2-t1)*m1
            + (-2*u**3+3*u*u)*p2 + (u**3-u*u)*(t2-t1)*m2)


def rotation(degrees):
    return Euler(tuple(math.radians(float(a)) for a in degrees), 'XYZ').to_quaternion()


def local_rotation(name, degrees):
    p = control.pose.bones[name]
    p.rotation_mode = 'QUATERNION'
    p.rotation_quaternion = rotation(degrees)


def world_pose(name, position, degrees=(0, 0, 0)):
    m = rotation(degrees).to_matrix().to_4x4() @ rest_world[name]
    m.translation = Vector(position)
    control.pose.bones[name].matrix = m


def sword_pose(grip, angles):
    yaw, pitch, bank = [math.radians(float(x)) for x in angles]
    m = (Matrix.Rotation(yaw, 4, 'Z') @ Matrix.Rotation(bank, 4, 'Y')
         @ Matrix.Rotation(pitch + math.pi/2, 4, 'X'))
    m.translation = Vector(grip)
    control.pose.bones['hand_ik.R'].matrix = m @ hand_to_sword.inverted()


def fingers(side, closed):
    for index, stem in enumerate(['f_index', 'f_middle', 'f_ring', 'f_pinky']):
        for joint, base in [(1, .50), (2, .85), (3, .66)]:
            amount = (.10 + .02*joint) * (1-closed) + (base + index*.035)*closed
            local_rotation(f'{stem}.{joint:02d}.{side}', (math.degrees(amount), 0, 0))
    for joint, angle in [(1, 20), (2, 28), (3, 18)]:
        local_rotation(f'thumb.{joint:02d}.{side}', (angle*closed, 0, 0))


def base_pose():
    return dict(torso=(-.008, -.018, -.038), hips=(0, 0, -3), chest=(3, 0, 5),
                head=(-2, 0, -3), grip=(-.265, -.205, .955), blade=(-12, 8, 0),
                left=(.31, -.045, .825), left_rot=(3, 0, -8),
                foot_r=(-.23, -.065, .132), foot_l=(.23, .155, .132),
                foot_rot_r=(0, 0, -4), foot_rot_l=(0, 0, 5), support=('R', 'L'),
                left_closed=.12, two_hand=0.0)


def evaluate_foot_surface():
    evaluated = body.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    vertices = np.empty(len(mesh.vertices)*3, dtype=np.float32)
    mesh.vertices.foreach_get('co', vertices)
    evaluated.to_mesh_clear()
    return vertices.reshape(-1, 3)


def apply_pose(pose):
    for p in control.pose.bones:
        p.matrix_basis = rest[p.name]
    control['two_hand_grip'] = float(pose['two_hand'])
    control.pose.bones['torso'].location = pose['torso']
    for name in ['hips', 'chest', 'head']:
        local_rotation(name, pose[name])
    update()
    for side, suffix in [('R', 'r'), ('L', 'l')]:
        world_pose('foot_ik.' + side, pose['foot_' + suffix], pose['foot_rot_' + suffix])
    world_pose('hand_ik.L', pose['left'], pose['left_rot'])
    sword_pose(pose['grip'], pose['blade'])
    fingers('R', 1.0)
    fingers('L', float(pose['left_closed']))
    update()
    # 元メッシュの足裏を使う。足IKの原点は床より上なので、そのZ値だけでは接地を判定できない。
    for _ in range(2):
        points = evaluate_foot_surface()
        for side in pose['support']:
            height = float(np.min(points[foot_ids[side], 2]))
            p = control.pose.bones['foot_ik.' + side]
            m = p.matrix.copy()
            m.translation.z += .002-height
            p.matrix = m
        update()


def keyed_pose(track, frame):
    pose = base_pose()
    for name, keys in track.items():
        value = sample(keys, frame)
        pose[name] = float(value.item()) if isinstance(pose[name], (int, float)) else value
    return pose


def idle(frame):
    return keyed_pose({
        'torso': {1:(-.008,-.018,-.038), 19:(.002,-.015,-.034), 37:(-.004,-.020,-.029), 55:(-.017,-.017,-.035), 73:(-.008,-.018,-.038)},
        'chest': {1:(3,0,5), 19:(3.6,-.5,3.8), 37:(2.1,.5,4.2), 55:(2.8,0,5.9), 73:(3,0,5)},
        'head': {1:(-2,0,-3), 25:(-1.3,0,-4.2), 43:(-2.1,.5,-1.8), 61:(-2.7,0,-2.7), 73:(-2,0,-3)},
        'grip': {1:(-.265,-.205,.955), 25:(-.261,-.204,.960), 49:(-.269,-.20,.957), 73:(-.265,-.205,.955)},
        'left': {1:(.31,-.045,.825), 25:(.318,-.048,.830), 49:(.306,-.042,.822), 73:(.31,-.045,.825)},
    }, frame)


def run(frame):
    phase = (frame-1) % 18
    pose = base_pose()
    pose['torso'] = (float(sample({0:[-.018], 4:[0], 9:[.018], 13:[0], 18:[-.018]}, phase)[0]), -.045,
                     float(sample({0:[-.070],2:[-.095],4:[-.040],6:[.030],8:[-.020],9:[-.070],11:[-.095],13:[-.040],15:[.030],17:[-.020],18:[-.070]}, phase)[0]))
    twist = float(sample({0:[-7], 4:[0], 9:[7], 13:[0], 18:[-7]}, phase)[0])
    pose['hips'] = (12, 0, twist)
    pose['chest'] = (5, 0, -twist*.8)
    pose['head'] = (-10, 0, twist*.45)
    pose['grip'] = sample({0:(-.315,-.095,.975),4:(-.33,-.10,.995),9:(-.31,-.19,1.01),13:(-.325,-.13,1.00),18:(-.315,-.095,.975)}, phase)
    pose['blade'] = sample({0:(-27,15,0),9:(-18,11,0),18:(-27,15,0)}, phase)
    pose['left'] = sample({0:(.275,-.33,1.055),4:(.30,-.18,.94),9:(.285,.19,.865),13:(.28,-.02,.955),18:(.275,-.33,1.055)}, phase)
    pose['left_rot'] = sample({0:(-12,0,-8),9:(28,0,-3),18:(-12,0,-8)}, phase)
    pose['left_closed'] = .35
    support = []
    foot = {0:(-.40,.132,-7),1:(-.20,.132,0),2:(.00,.132,0),3:(.20,.148,12),
            4:(.40,.225,30),6:(.45,.425,58),9:(.16,.53,55),12:(-.30,.46,0),
            15:(-.48,.265,-14),17:(-.50,.175,-10),18:(-.40,.132,-7)}
    for side, offset, x, suffix in [('R',0,-.18,'r'),('L',9,.18,'l')]:
        t = (phase+offset) % 18
        y, z, pitch = sample(foot, t, linear=t<=3)
        pose['foot_'+suffix] = (x,float(y),float(z))
        pose['foot_rot_'+suffix] = (float(pitch),0,0)
        if t<=3:
            support.append(side)
    pose['support'] = support
    return pose


SLASH = {
    'grip': {1:(-.265,-.205,.955),4:(-.32,-.13,1.13),7:(-.32,-.10,1.32),9:(-.06,-.46,1.06),11:(.30,-.31,.86),14:(.31,-.24,.91),17:(-.04,-.30,.94),20:(-.265,-.205,.955)},
    'blade': {1:(-12,8,0),4:(-32,-60,8),7:(-48,-98,12),9:(5,2,0),11:(48,27,-8),14:(58,19,0),17:(15,10,0),20:(-12,8,0)},
    'torso': {1:(-.008,-.018,-.038),4:(-.03,.015,-.06),7:(-.035,.022,-.085),9:(-.005,-.07,-.085),11:(.03,-.08,-.08),14:(.02,-.045,-.055),20:(-.008,-.018,-.038)},
    'hips': {1:(0,0,-3),7:(2,0,-8),9:(5,0,10),11:(3,0,15),20:(0,0,-3)},
    'chest': {1:(3,0,5),4:(0,-2,-10),7:(-3,-3,-20),9:(9,0,14),11:(8,3,24),15:(4,1,12),20:(3,0,5)},
    'head': {1:(-2,0,-3),7:(1,0,15),9:(-4,0,-10),11:(-5,0,-17),20:(-2,0,-3)},
    'left': {1:(.31,-.045,.825),7:(.30,-.09,1.02),9:(.34,.00,.98),11:(.38,.07,.90),20:(.31,-.045,.825)},
}
PARRY = {
    'grip': {1:(-.265,-.205,.955),2:(-.10,-.25,1.065),3:(.015,-.265,1.10),8:(.015,-.265,1.10),10:(.025,-.22,1.08),12:(.12,-.34,1.16),15:(.04,-.26,1.10),18:(-.17,-.25,1.01),22:(-.265,-.205,.955)},
    'blade': {1:(-12,8,0),2:(40,-28,-5),3:(65,-28,-10),8:(65,-28,-10),10:(70,-25,-14),12:(38,-38,-3),15:(20,-25,0),18:(-5,0,0),22:(-12,8,0)},
    'torso': {1:(-.008,-.018,-.038),3:(-.015,.002,-.075),8:(-.015,.002,-.075),10:(-.02,.03,-.09),12:(-.012,-.02,-.065),22:(-.008,-.018,-.038)},
    'hips': {1:(0,0,-3),3:(0,0,-4),10:(-2,0,-6),12:(4,0,4),22:(0,0,-3)},
    'chest': {1:(3,0,5),3:(-3,0,-5),8:(-3,0,-5),10:(-6,-2,-8),12:(5,0,12),22:(3,0,5)},
    'head': {1:(-2,0,-3),3:(2,0,4),10:(4,0,6),12:(-4,0,-8),22:(-2,0,-3)},
    'two_hand': {1:[0],2:[.4],3:[1],12:[1],15:[.8],18:[0],22:[0]},
    'left_closed': {1:[.12],3:[1],15:[1],18:[.3],22:[.12]},
}


manifest = {'schema':1, 'fps':30, 'skeleton':'MiniBotC_Humanoid', 'bones':54,
            'source_blend':'../../../_src/MiniBotC/MiniBotC_PlayerMotions.blend',
            'companion_animation':False, 'runtime_verified':False, 'clips':{}}
specs = [
    ('Stance',2,False,lambda f:base_pose(),{},'Reference',0),
    ('Idle',73,True,idle,{},'Base',0),
    ('Run_F',19,True,run,{'FootContact_R':1,'ToeOff_R':5,'FootContact_L':10,'ToeOff_L':14},'Base',6.0),
    ('Slash01',20,False,lambda f:keyed_pose(SLASH,f),{'Windup':4,'WindupPeak':7,'Hit':9,'FollowThrough':12,'Recover':15},'Attack',0),
    ('Parry',22,False,lambda f:keyed_pose(PARRY,f),{'GuardReady':3,'GuardEnd':8,'Impact':10,'Riposte':12,'Recover':16},'Attack',0),
]
diagnostics = {}
for label, end, loop, pose_at, markers, layer, speed in specs:
    name = 'MB_C_' + label
    existing = bpy.data.actions.get(name)
    if existing:
        raise RuntimeError('Refusing to overwrite an existing authored Action: ' + name)
    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    control.animation_data.action = action
    minimum_sword = 1e9
    maximum_hand_error = 0
    for frame in range(1,end+1):
        scene.frame_set(frame)
        pose = pose_at(frame)
        apply_pose(pose)
        for n in key_bones:
            p = control.pose.bones[n]
            p.rotation_mode = 'QUATERNION'
            for channel in ['location','rotation_quaternion','scale']:
                p.keyframe_insert(channel,frame=frame,group=n)
        control.keyframe_insert('["two_hand_grip"]',frame=frame,group='Weapon')
        minimum_sword = min(minimum_sword, min((sword.matrix_world @ v.co).z for v in bpy.data.objects['Sword_LOD0'].data.vertices))
        if float(pose['two_hand']) > .99:
            error = (rig.pose.bones['LeftHand'].head-bpy.data.objects['LeftHand_GripTarget'].matrix_world.translation).length
            maximum_hand_error = max(maximum_hand_error,error)
    for slot in action.slots:
        for action_layer in action.layers:
            for strip in action_layer.strips:
                bag = strip.channelbag(slot)
                if bag:
                    for curve in bag.fcurves:
                        for key in curve.keyframe_points:
                            key.interpolation = 'LINEAR'
    for marker, frame in markers.items():
        action.pose_markers.new(marker).frame = frame
    clip = {'name':name,'start_frame':1,'end_frame':end,'duration_seconds':(end-1)/30,
            'loop':loop,'reference':'MB_C_Stance','layer':layer,'root_motion':'in_place',
            'reference_speed_mps':speed,'markers':{n:{'frame':f,'seconds':(f-1)/30} for n,f in markers.items()}}
    for key,value in clip.items():
        if isinstance(value,(str,int,float,bool)):
            action[key]=value
    manifest['clips'][name]=clip
    diagnostics[name]={'sword_min_z_m':minimum_sword,'two_hand_max_error_m':maximum_hand_error}
    assert minimum_sword > .02,(name,'sword intersects floor',minimum_sword)
    assert maximum_hand_error < .015,(name,'two-hand target unreachable',maximum_hand_error)
    print('AUTHORED '+name+' '+json.dumps(diagnostics[name]),flush=True)

control.animation_data.action=bpy.data.actions['MB_C_Idle']
scene.frame_start=1
scene.frame_end=73
scene.frame_set(1)
for o in scene.objects:
    if o.name.startswith('Companion'):
        o.hide_set(True)
        o.hide_render=True
for marker in list(scene.timeline_markers):
    scene.timeline_markers.remove(marker)
scene['pipeline_stage']='foundation_motion_review'
scene['animation_status']='Four authored Player motions; runtime integration pending'
bpy.ops.object.select_all(action='DESELECT')
control.hide_set(False)
control.select_set(True)
bpy.context.view_layer.objects.active=control
bpy.ops.object.mode_set(mode='POSE')
bpy.ops.pose.select_all(action='DESELECT')
control.data.bones.active=control.data.bones['hand_ik.R']
control.pose.bones['hand_ik.R'].select=True
(OUT/'AuthoringDiagnostics.json').write_text(json.dumps(diagnostics,indent=2),encoding='utf8')
dest=ROOT/'GreenWare/Assets/Models/MiniBotC_AnimationReady/Animations/motion_manifest.json'
dest.write_text(json.dumps(manifest,indent=2),encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(BLEND),compress=True)
print('FOUNDATION AUTHORING COMPLETE '+str(BLEND),flush=True)
