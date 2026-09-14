"""Boss03 初回造形用。既存の .blend を入力・上書きしない。

Blender --background --factory-startup --python create_boss03.py -- <出力ディレクトリ>
作成後の正本は Boss03.blend。手作業による編集をこのスクリプトで作り直さない。
"""

import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Euler, Matrix, Quaternion, Vector


OUT = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
BLEND = OUT / 'Boss03.blend'
if BLEND.exists():
    raise RuntimeError(f'既存の原本は上書きできません: {BLEND}')

bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
for collection in list(bpy.data.collections):
    if collection.name != 'Collection':
        bpy.data.collections.remove(collection)
export = bpy.data.collections.get('Collection')
export.name = 'EXPORT_Boss03'
studio = bpy.data.collections.new('STUDIO_Preview_Only')
bpy.context.scene.collection.children.link(studio)
parts = []
assignments = {}


def move_collection(obj, collection):
    for old in list(obj.users_collection):
        old.objects.unlink(obj)
    collection.objects.link(obj)


def material(name, color, metal, roughness, emission=0.0):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*color, 1)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get('Principled BSDF')
    bsdf.inputs['Base Color'].default_value = (*color, 1)
    bsdf.inputs['Metallic'].default_value = metal
    bsdf.inputs['Roughness'].default_value = roughness
    if emission:
        bsdf.inputs['Emission Color'].default_value = (*color, 1)
        bsdf.inputs['Emission Strength'].default_value = emission
    return mat


armor = material('B03_Armor_Ceramic_Gray', (0.32, 0.36, 0.40), .64, .32)
armor_light = material('B03_Armor_Edge_Silver', (.46, .51, .56), .72, .28)
armor_dark = material('B03_Armor_Secondary', (.16, .19, .23), .7, .34)
mechanism = material('B03_Mechanism_Graphite', (.025, .034, .045), .72, .3)
steel = material('B03_Joint_Machined_Steel', (.15, .19, .22), .88, .24)
rubber = material('B03_Recess_Black', (.007, .012, .018), .16, .42)
red = material('B03_Emission_Coral', (1.0, .065, .032), .32, .25, 3.0)
red_dim = material('B03_Emission_Indicator', (.62, .035, .018), .4, .3, 1.4)
hot = material('B03_Emission_Core', (1.0, .17, .07), .2, .24, 4.0)


def register(obj, name, mat, bone='Body', matrix=None, bevel=0):
    obj.name = name
    move_collection(obj, export)
    if mat:
        obj.data.materials.append(mat)
    if matrix is not None:
        obj.matrix_world = matrix @ obj.matrix_world
    if bevel:
        mod = obj.modifiers.new('Machined_Edge', 'BEVEL')
        mod.width = bevel
        mod.segments = 3
    if obj.type == 'MESH':
        for poly in obj.data.polygons:
            poly.use_smooth = True
        normal = obj.modifiers.new('Face_Normals', 'WEIGHTED_NORMAL')
        normal.keep_sharp = True
    parts.append(obj)
    assignments[obj.name] = bone
    return obj


def cube(name, loc, size, mat, bone='Body', matrix=None, bevel=.06):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc)
    obj = bpy.context.object
    obj.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return register(obj, name, mat, bone, matrix, bevel)


def sphere(name, loc, size, mat, bone='Body', matrix=None):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=1, location=loc)
    obj = bpy.context.object
    obj.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return register(obj, name, mat, bone, matrix)


def cylinder(name, a, b, radius, mat, bone='Body', matrix=None, vertices=24):
    a, b = Vector(a), Vector(b)
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=(b-a).length,
                                     location=(a+b)/2)
    obj = bpy.context.object
    obj.rotation_mode = 'QUATERNION'
    obj.rotation_quaternion = (b-a).to_track_quat('Z', 'Y')
    return register(obj, name, mat, bone, matrix, .018)


def torus(name, loc, radius, tube, mat, bone='Body', matrix=None, front=True):
    bpy.ops.mesh.primitive_torus_add(major_segments=48, minor_segments=10,
                                    location=loc, major_radius=radius, minor_radius=tube)
    obj = bpy.context.object
    if front:
        obj.rotation_euler.x = math.pi/2
    return register(obj, name, mat, bone, matrix)


def mesh_object(name, verts, faces, mat, bone='Body', matrix=None, bevel=.025):
    mesh = bpy.data.meshes.new(name + '_Mesh')
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    export.objects.link(obj)
    return register(obj, name, mat, bone, matrix, bevel)


def plate(name, outline, yfront, thickness, mat, bone='Body', matrix=None, bevel=.035):
    n = len(outline)
    verts = [(x, yfront, z) for x, z in outline]
    verts += [(x, yfront+thickness, z) for x, z in outline]
    faces = [tuple(reversed(range(n))), tuple(range(n, 2*n))]
    faces += [(i, (i+1)%n, (i+1)%n+n, i+n) for i in range(n)]
    return mesh_object(name, verts, faces, mat, bone, matrix, bevel)


def capsule_outline(width, height, z, steps=16):
    radius = height/2
    half = (width-height)/2
    return [(half+radius*math.cos(-math.pi/2+i*math.pi/steps),
             z+radius*math.sin(-math.pi/2+i*math.pi/steps)) for i in range(steps+1)] + [
            (-half+radius*math.cos(math.pi/2+i*math.pi/steps),
             z+radius*math.sin(math.pi/2+i*math.pi/steps)) for i in range(steps+1)]


def capsule_ring(name, width, height, border, z, y, depth, mat):
    outer = capsule_outline(width, height, z)
    inner = capsule_outline(width-2*border, height-2*border, z)
    n = len(outer)
    verts = [(x, yy, zz) for yy, loop in [(y, outer), (y, inner),
             (y+depth, outer), (y+depth, inner)] for x, zz in loop]
    faces = []
    for i in range(n):
        j = (i+1)%n
        faces.extend([(i, j, n+j, n+i), (i, 2*n+i, 2*n+j, j),
                      (n+i, n+j, 3*n+j, 3*n+i), (2*n+i, 3*n+i, 3*n+j, 2*n+j)])
    return mesh_object(name, verts, faces, mat, bevel=.018)


# 胴体は暗い内部フレームを装甲が覆い、目と炉心には実際の凹みを残す。
sphere('B03_Body_InternalHull', (0, .02, 3.80), (1.00, .72, 1.34), mechanism)
sphere('B03_Body_RearCarapace', (0, .32, 4.02), (.96, .65, 1.05), armor_dark)
plate('B03_Head_CrownArmor', [(-.85,4.25),(-.73,4.93),(-.40,5.22),
      (.40,5.22),(.73,4.93),(.85,4.25),(.56,4.35),(-.56,4.35)], -.61,.41,armor, bevel=.09)
plate('B03_Head_CenterKeel', [(-.12,4.47),(-.16,5.12),(0,5.33),(.16,5.12),(.12,4.47)],
      -.70,.10,armor_light, bevel=.025)
capsule_ring('B03_Eye_OuterBezel', 1.67,.75,.13,4.10,-.85,.22,armor_light)
capsule_ring('B03_Eye_InnerSocket', 1.43,.53,.09,4.10,-.91,.15,rubber)
plate('B03_Eye_Lens', capsule_outline(1.21,.31,4.10), -.90,.04,red, bevel=.025)
plate('B03_Eye_LensHotline', capsule_outline(.83,.075,4.105), -.923,.01,hot, bevel=.01)
for side, s in [('L', -1), ('R', 1)]:
    outline = [(s*x,z) for x,z in [(.88,4.43),(1.12,4.12),(.99,3.43),(.64,3.18),
                                  (.52,3.49),(.70,3.86)]]
    if s < 0:
        outline.reverse()
    plate('B03_Cheek_' + side, outline,-.62,.42,armor,bevel=.07)
    plate('B03_Brow_' + side, [(s*x,z) for x,z in [(.37,4.57),(.82,4.68),(.90,4.35),(.70,4.33)]],
          -.77,.15,armor_dark,bevel=.03)
    cylinder('B03_NeckRail_' + side,(s*.52,-.20,2.40),(s*.75,-.13,3.50),.10,steel)
    cylinder('B03_NeckRailCollar_' + side,(s*.59,-.18,2.72),(s*.68,-.15,3.15),.16,mechanism)
    cube('B03_SideVentHousing_' + side,(s*.96,.08,3.94),(.18,.63,.65),mechanism)
    for j in range(5):
        cube(f'B03_SideVent_{side}_{j}',(s*1.06,-.05,3.69+j*.12),(.06,.38,.055),steel,bevel=.014)
plate('B03_JawArmor', [(-.62,3.71),(.62,3.71),(.47,3.30),(.23,3.17),(-.23,3.17),(-.47,3.30)],
      -.70,.30,armor,bevel=.07)
cube('B03_Jaw_StatusBar',(0,-.872,3.47),(.32,.04,.055),red_dim,bevel=.015)
cube('B03_Dorsal_Spine',(0,.84,4.04),(.32,.32,1.76),mechanism,bevel=.055)
for j in range(5):
    cube(f'B03_Dorsal_HeatSink_{j}',(0,1.01,3.44+j*.28),(.56,.20,.075),steel,bevel=.018)

sphere('B03_Core_Containment',(0,-.02,2.72),(.54,.48,.57),mechanism,'Core')
sphere('B03_Core_Reactor',(0,-.36,2.72),(.36,.34,.39),hot,'Core')
torus('B03_Core_OuterRing',(0,-.43,2.72),.47,.070,steel,'Core')
torus('B03_Core_LightRing',(0,-.47,2.72),.393,.025,red,'Core')
cube('B03_Core_EquatorBrace',(0,-.715,2.72),(.91,.10,.090),armor_dark,'Core',bevel=.024)
for side,s in [('L',-1),('R',1)]:
    plate('B03_Core_Guard_' + side,[(s*x,z) for x,z in [(.54,3.21),(.72,3.00),(.65,2.28),(.34,2.04),(.37,2.55)]],
          -.25,.32,armor,'Body',bevel=.045)
cylinder('B03_Thruster_Nozzle',(0,.05,2.30),(0,.05,2.07),.31,mechanism)
torus('B03_Thruster_Emission',(0,.05,2.08),.22,.025,red,front=False)


wing_frames = {}
closed_frames = {}
wing_names = []
for side,s in [('L',-1),('R',1)]:
    for tier,angle,z,slot in [('Upper',18,4.24,0),('Middle',61,3.81,1),('Lower',139,3.35,2)]:
        bone = f'Wing_{side}_{tier}'
        wing_names.append(bone)
        anchor = Vector((s*.92,.22,z))
        rot = Euler((0,math.radians(s*angle),0)).to_matrix().to_4x4()
        frame = Matrix.Translation(anchor) @ rot
        wing_frames[bone] = frame
        # 六方の閉鎖姿勢。上部・中部・下部の翼を周方向に並べて繭を作る。
        theta = math.radians(s*(30+60*slot))
        closed_frames[bone] = Matrix.Translation((.50*math.sin(theta),-.50*math.cos(theta),1.70)) @ \
            Euler((0,0,theta)).to_matrix().to_4x4()
        cylinder('B03_Mount_' + bone,(s*.65,.24,z),(s*1.02,.24,z),.26,mechanism)
        cylinder(bone+'_RootAxle',(-.24,0,0),(.24,0,0),.22,steel,bone,frame)
        cylinder(bone+'_RootBearing',(0,-.18,0),(0,.18,0),.28,mechanism,bone,frame)
        torus(bone+'_BreakableJoint',(0,-.21,0),.20,.035,red_dim,bone,frame)
        cylinder(bone+'_JointHub',(0,-.245,0),(0,-.285,0),.13,steel,bone,frame)
        cube(bone+'_Link',(0,.06,.34),(.30,.28,.61),mechanism,bone,frame,.045)
        for sx in [-1,1]:
            cylinder(bone+f'_Piston_{sx}',(sx*.18,-.07,.12),(sx*.18,-.07,.73),.055,steel,bone,frame)
        cylinder(bone+'_Wrist',(0,-.22,.72),(0,.22,.72),.255,mechanism,bone,frame)
        cylinder(bone+'_WristCap',(0,-.245,.72),(0,-.285,.72),.175,steel,bone,frame)
        torus(bone+'_WristRing',(0,-.30,.72),.13,.015,armor_light,bone,frame)
        # 幅の異なる断面をつなぎ、平たい板ではなく中央稜線と厚みのある盾にする。
        sections = [(.83,.23,.04),(1.23,.59,-.10),(2.18,.65,-.19),(3.11,.40,-.10),(3.76,.035,.12)]
        verts = []
        for zz,ww,yy in sections:
            verts.extend([(-ww,yy,zz),(0,yy-.13,zz),(ww,yy,zz),
                          (ww,yy+.22,zz),(0,yy+.30,zz),(-ww,yy+.22,zz)])
        faces = [tuple(reversed(range(6)))]
        for k in range(len(sections)-1):
            for j in range(6):
                faces.append((k*6+j,k*6+(j+1)%6,(k+1)*6+(j+1)%6,(k+1)*6+j))
        faces.append(tuple(range(24,30)))
        mesh_object(bone+'_ShieldStructure',verts,faces,armor_dark,bone,frame,.035)
        plate(bone+'_Shield_InnerArmor',[(-.22,.88),(-.55,1.28),(-.59,2.14),(-.19,2.10),(-.07,1.47),(-.075,1.01)],
              -.275,.15,armor,bone,frame,.035)
        plate(bone+'_Shield_OuterArmor',[(.075,1.01),(.07,1.47),(.19,2.10),(.59,2.14),(.55,1.28),(.22,.88)],
              -.275,.15,armor_light,bone,frame,.035)
        plate(bone+'_Blade_InnerArmor',[(-.59,2.19),(-.36,3.12),(-.018,3.76),(-.042,2.24)],
              -.28,.14,armor,bone,frame,.025)
        plate(bone+'_Blade_OuterArmor',[(.042,2.24),(.018,3.76),(.36,3.12),(.59,2.19)],
              -.28,.14,armor_light,bone,frame,.025)
        plate(bone+'_Spine',[(-.034,1.30),(.034,1.30),(.035,2.88),(0,3.62),(-.035,2.88)],
              -.33,.05,steel,bone,frame,.01)
        plate(bone+'_LightSocket',[(-.53,2.30),(-.32,3.13),(-.21,3.28),(-.41,2.28)],
              -.365,.035,rubber,bone,frame,.015)
        plate(bone+'_AttackLight',[(-.496,2.35),(-.304,3.09),(-.265,3.12),(-.451,2.35)],
              -.393,.025,red,bone,frame,.008)
        cube(bone+'_BaseLatch',(0,-.30,1.08),(.30,.12,.25),steel,bone,frame,.035)
        for j in range(3):
            cube(bone+f'_RearVent_{j}',(0,.34,1.49+j*.23),(.49,.10,.085),mechanism,bone,frame,.015)
        for j,(xx,zz) in enumerate([(-.43,1.39),(.43,1.39),(-.41,2.18),(.41,2.18)]):
            cylinder(bone+f'_Fastener_{j}',(xx,-.30,zz),(xx,-.35,zz),.036,steel,bone,frame,vertices=12)


# モディファイアを確定してから剛体ウェイトを与える。関節で金属が伸びない構成。
for obj in parts:
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    for mod in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    obj.select_set(False)

arm_data = bpy.data.armatures.new('Boss03_Skeleton')
rig = bpy.data.objects.new('Boss03_Armature',arm_data)
export.objects.link(rig)
bpy.context.view_layer.objects.active = rig
rig.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
root = arm_data.edit_bones.new('Root')
root.head = (0,0,0)
root.tail = (0,0,1)
root.use_deform = False
for name,head,tail in [('Body',(0,0,3.70),(0,0,4.50)),('Core',(0,0,2.72),(0,0,3.10))]:
    bone = arm_data.edit_bones.new(name)
    bone.head, bone.tail = head, tail
    bone.parent = root
for name,frame in wing_frames.items():
    bone = arm_data.edit_bones.new(name)
    bone.head = frame.translation
    bone.tail = frame @ Vector((0,0,.72))
    bone.parent = root
for name,location in [('SOCKET_Core',(0,-.4,2.72)),('SOCKET_Eye',(0,-.9,4.1))]:
    bone = arm_data.edit_bones.new(name)
    bone.head = location
    bone.tail = Vector(location)+Vector((0,-.20,0))
    bone.parent = arm_data.edit_bones['Core' if 'Core' in name else 'Body']
    bone.use_deform = False
bpy.ops.object.mode_set(mode='OBJECT')
rig.show_in_front = True
rig.data.display_type = 'STICK'
rig['Design'] = 'Stage03 / Six-wing floating machine / Front -Y / Up +Z'
rig['Actions'] = 'Use Action Editor. Scene timeline previews open / closed / deploy.'
for name in wing_names:
    rig.pose.bones[name]['Detachable'] = True
    rig.pose.bones[name]['Role'] = 'Independent shield / attack / breakable joint'
for obj in parts:
    group = obj.vertex_groups.new(name=assignments[obj.name])
    group.add(list(range(len(obj.data.vertices))),1.0,'REPLACE')
    obj.parent = rig
    mod = obj.modifiers.new('Boss03_RigidSkin','ARMATURE')
    mod.object = rig
    obj['PartBone'] = assignments[obj.name]

# UV0 は編集・ベイク用。マテリアルは外部画像なしで開ける。
bpy.ops.object.select_all(action='DESELECT')
for obj in parts:
    obj.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(angle_limit=math.radians(66),island_margin=.015)
bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.object.select_all(action='DESELECT')


def mix_matrix(a,b,t):
    loc_a,rot_a,scale_a = a.decompose()
    loc_b,rot_b,scale_b = b.decompose()
    return Matrix.LocRotScale(loc_a.lerp(loc_b,t),rot_a.slerp(rot_b,t),scale_a.lerp(scale_b,t))


def pose_frame(frame, closure=0, bob=0, overrides=None, tilt=0, drop=0):
    scene.frame_set(frame)
    for bone in rig.pose.bones:
        bone.matrix_basis.identity()
        bone.rotation_mode = 'QUATERNION'
    bpy.context.view_layer.update()
    for name,opened in wing_frames.items():
        target = mix_matrix(opened,closed_frames[name],closure)
        if overrides and name in overrides:
            target = overrides[name]
        rig.pose.bones[name].matrix = target @ opened.inverted() @ arm_data.bones[name].matrix_local
    rig.pose.bones['Root'].location.y = bob-drop
    rig.pose.bones['Root'].rotation_quaternion = Quaternion((1,0,0),math.radians(tilt))
    for bone in rig.pose.bones:
        if bone.name.startswith('SOCKET_'):
            continue
        bone.keyframe_insert(data_path='location',frame=frame,group=bone.name)
        bone.keyframe_insert(data_path='rotation_quaternion',frame=frame,group=bone.name)
        bone.keyframe_insert(data_path='scale',frame=frame,group=bone.name)


def begin_action(name, length, note):
    rig.animation_data_create()
    rig.animation_data.action = None
    action = bpy.data.actions.new('Boss03_'+name)
    action.use_fake_user = True
    rig.animation_data.action = action
    action['Description'] = note
    action['FPS'] = 30
    action['Frames'] = length
    return action


scene = bpy.context.scene
scene.render.fps = 30
scene.unit_settings.system = 'METRIC'
scene.unit_settings.scale_length = 1
scene.name = 'Boss03_Preview'
begin_action('Idle',121,'低空待機・ループ')
for f,bob in [(1,0),(31,.09),(61,0),(91,-.07),(121,0)]:
    overrides = {n: m @ Euler((math.radians(1.5*math.sin((f-1)/120*math.tau)),0,0)).to_matrix().to_4x4()
                 for n,m in wing_frames.items()}
    pose_frame(f,bob=bob,overrides=overrides)
begin_action('Cocoon_Idle',121,'閉鎖待機・ループ')
for f,bob in [(1,0),(31,.08),(61,0),(91,-.06),(121,0)]:
    pose_frame(f,closure=1,bob=bob)
begin_action('Deploy',91,'繭から六翼を展開')
for f,c,b in [(1,1,0),(18,1,.15),(54,.08,.12),(72,0,-.04),(91,0,0)]:
    pose_frame(f,closure=c,bob=b)
begin_action('Close',61,'防御・突進への閉鎖')
for f,c in [(1,0),(15,.05),(45,1),(61,1)]:
    pose_frame(f,closure=c)
for side,s in [('L',-1),('R',1)]:
    begin_action('Slam_'+side,91,'振り上げ・打ち下ろし・関節への反撃時間・復帰')
    name = f'Wing_{side}_Middle'
    windup = Matrix.Translation((s*.93,.25,4.15)) @ Euler((-.35,math.radians(s*13),0)).to_matrix().to_4x4()
    impact = Matrix.Translation((s*1.5,-.32,3.35)) @ Euler((.28,math.radians(s*150),0)).to_matrix().to_4x4()
    for f,m,b in [(1,wing_frames[name],0),(24,windup,.12),(32,windup,.12),
                  (40,impact,-.30),(64,impact,-.30),(91,wing_frames[name],0)]:
        pose_frame(f,bob=b,overrides={name:m})
begin_action('Laser_Fan',101,'翼を前へ構える・照射・復帰。レーザー自体はゲーム側')
aim = {n: m @ Euler((math.radians(38),0,0)).to_matrix().to_4x4() for n,m in wing_frames.items()}
for f,t in [(1,0),(26,1),(40,1),(76,1),(101,0)]:
    pose_frame(f,overrides={n:mix_matrix(wing_frames[n],aim[n],t) for n in wing_names})
begin_action('Dash_InPlace',91,'閉鎖・突進姿勢・停止後展開。水平移動はゲーム側')
for f,c,t,b in [(1,0,0,0),(23,1,0,.14),(33,1,-12,.10),(54,1,-12,.10),(65,.65,6,-.14),(91,0,0,0)]:
    pose_frame(f,closure=c,tilt=t,bob=b)
begin_action('Stagger',151,'落下・コア露出・復帰')
for f,d,t in [(1,0,0),(18,1.20,8),(30,1.05,-3),(111,1.05,0),(151,0,0)]:
    pose_frame(f,drop=d,tilt=t)
begin_action('Death',121,'機能停止・落下・不完全な閉鎖')
for f,d,c,t in [(1,0,0,0),(24,.3,.18,-5),(55,1.3,.65,12),(85,1.58,.72,17),(121,1.58,.72,17)]:
    pose_frame(f,drop=d,closure=c,tilt=t)
for name in wing_names:
    begin_action('Detach_'+name.removeprefix('Wing_'),51,'単翼の脱落見本。破壊時の表示制御はゲーム側')
    orig = wing_frames[name]
    s = -1 if '_L_' in name else 1
    fallen = Matrix.Translation((orig.translation.x+s*2,.6,orig.translation.z-1.8)) @ \
        orig.to_quaternion().to_matrix().to_4x4() @ Euler((.8,.6*s,.4)).to_matrix().to_4x4()
    for f,t in [(1,0),(12,.05),(32,.55),(51,1)]:
        pose_frame(f,overrides={name:mix_matrix(orig,fallen,t)})
showcase = begin_action('Showcase',241,'プレビュー: 1 展開 / 81 閉鎖 / 151 展開開始 / 211 全開')
for f,c,b in [(1,0,0),(31,0,.05),(81,1,0),(121,1,.06),(151,1,0),(211,0,0),(241,0,0)]:
    pose_frame(f,closure=c,bob=b)
scene.frame_start,scene.frame_end = 1,241
for label,frame in [('OPEN / 六翼展開',1),('CLOSED / 繭',81),('DEPLOY / 展開開始',151),('OPEN / 展開完了',211)]:
    scene.timeline_markers.new(label,frame=frame)


def studio_object(obj):
    move_collection(obj,studio)
    return obj


ground_mat = material('Studio_Slate',(.020,.030,.047),.2,.55)
bpy.ops.mesh.primitive_plane_add(size=200,location=(0,0,-.2))
ground = studio_object(bpy.context.object)
ground.name = 'Studio_Floor_NotForExport'
ground.data.materials.append(ground_mat)


def camera(name,location,target,ortho):
    data = bpy.data.cameras.new(name)
    obj = bpy.data.objects.new(name,data)
    studio.objects.link(obj)
    obj.location = location
    obj.rotation_euler = (Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()
    data.type = 'ORTHO'
    data.ortho_scale = ortho
    data.lens = 52
    return obj


cam = camera('Camera_Hero',(10,-22,9),(0,0,3.65),11.3)
front_cam = camera('Camera_Front',(0,-22,4.0),(0,0,4.0),10.5)
side_cam = camera('Camera_Side',(20,0,4.0),(0,0,4.0),10.5)
back_cam = camera('Camera_Back',(0,22,4.0),(0,0,4.0),10.5)
scene.camera = cam
for name,loc,power,color,size,target in [
    ('Key_Softbox',(-6,-9,12),2200,(.84,.92,1),8,(0,0,3.5)),
    ('Fill_Softbox',(7,-4,6),1500,(.64,.78,1),7,(0,0,3.6)),
    ('Rim_Softbox',(1,6,10),2600,(.80,.90,1),6,(0,0,4)),
    ('Front_Softbox',(-1,-10,3),600,(1,.84,.73),5,(0,0,3.5))]:
    data = bpy.data.lights.new(name,'AREA')
    data.energy,data.color,data.shape,data.size = power,color,'DISK',size
    obj = bpy.data.objects.new(name,data)
    studio.objects.link(obj)
    obj.location = loc
    obj.rotation_euler = (Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()
world = bpy.data.worlds.new('Boss03_Studio_World')
world.use_nodes = True
world.node_tree.nodes['Background'].inputs[0].default_value = (.055,.075,.11,1)
world.node_tree.nodes['Background'].inputs[1].default_value = .4
scene.world = world
scene.render.engine = 'CYCLES'
scene.cycles.samples = 32
scene.cycles.use_denoising = True
scene.render.resolution_x = 1400
scene.render.resolution_y = 1200
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'PNG'
scene.view_settings.view_transform = 'AgX'
scene.render.film_transparent = False

readme = bpy.data.texts.new('START_HERE_Boss03.txt')
readme.write('''BOSS03 / 六翼の浮遊機兵
========================
2026-09-13 / 初回モデル

Timeline: frame 1 = OPEN, frame 81 = CLOSED, frame 211 = OPEN.
Play the timeline to preview the transformation.
Action Editor: select Boss03_Armature to choose Boss03_* clips.
30 fps. Blender front -Y, up +Z. Metric units.

EXPORT_Boss03 contains only the rig and rigid-skinned mesh parts.
STUDIO_Preview_Only contains the cameras, lights and floor. Do not export it.
Each wing has its own Wing_L/R_Upper/Middle/Lower bone and mesh prefix.
Core and eye sockets: SOCKET_Core / SOCKET_Eye.
All mesh parts have UV0 and full-weight rigid skinning.
Materials use no external textures. Emission is identified by material name.

Clips: Idle, Cocoon_Idle, Deploy, Close, Slam_L, Slam_R, Laser_Fan,
Dash_InPlace, Stagger, Death, Detach_(six wings), Showcase.
The clips are authored movement assets. AI, hitboxes, attack lights,
HP, laser effects and actual wing removal must be connected in the game.
FBX export and engine integration have not been validated in this file.

Boss03.blend is the editable source of truth. Do not regenerate it over
manual edits using the initial creation script.
''')

scene.frame_set(1)
bpy.context.view_layer.update()
bpy.ops.object.select_all(action='DESELECT')
rig.select_set(True)
bpy.context.view_layer.objects.active = rig
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            area.spaces.active.region_3d.view_perspective = 'PERSP'
            area.spaces.active.region_3d.view_distance = 13
            area.spaces.active.region_3d.view_location = (0,0,3.7)
            area.spaces.active.region_3d.view_rotation = cam.rotation_euler.to_quaternion()
            area.spaces.active.shading.type = 'MATERIAL'
            area.spaces.active.overlay.show_overlays = False
            area.spaces.active.clip_end = 500
studio.hide_viewport = True
bpy.ops.wm.save_as_mainfile(filepath=str(BLEND))

stats = {'mesh_objects':len(parts),'bones':len(arm_data.bones),
         'vertices':sum(len(o.data.vertices) for o in parts),
         'triangles':sum(sum(len(p.vertices)-2 for p in o.data.polygons) for o in parts),
         'actions':[a.name for a in bpy.data.actions if a.name.startswith('Boss03_')],
         'materials':len({m.name for o in parts for m in o.data.materials}),
         'blend':str(BLEND)}
(OUT/'Boss03_Report.json').write_text(json.dumps(stats,indent=2),encoding='utf-8')
print('BOSS03_REPORT '+json.dumps(stats),flush=True)

studio.hide_viewport = False
for filename,frame,camera_obj in [('Boss03_Open.png',1,cam),('Boss03_Closed.png',81,cam),
                                  ('Boss03_Front.png',1,front_cam),('Boss03_Back.png',1,back_cam)]:
    scene.frame_set(frame)
    scene.camera = camera_obj
    scene.render.filepath = str(OUT/filename)
    bpy.ops.render.render(write_still=True)
    print('BOSS03_RENDER '+filename,flush=True)
