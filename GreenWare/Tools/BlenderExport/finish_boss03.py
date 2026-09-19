"""Boss03 初回制作の曲面法線・閉鎖高さ・ダウン姿勢を確定する。"""

import math
from pathlib import Path

import bmesh
import bpy
from mathutils import Euler, Matrix

rig = bpy.data.objects['Boss03_Armature']
scene = bpy.context.scene
out = Path(bpy.data.filepath).parent
if rig.get('SurfaceFinished',False):
    raise RuntimeError('仕上げは適用済みです')
original_action = rig.animation_data.action
opened,closed = {},{}
for side,s in [('L',-1),('R',1)]:
    for tier,angle,z,slot in [('Upper',18,4.24,0),('Middle',61,3.81,1),('Lower',139,3.35,2)]:
        name = f'Wing_{side}_{tier}'
        opened[name] = Matrix.Translation((s*.92,.22,z)) @ Euler((0,math.radians(s*angle),0)).to_matrix().to_4x4()
        theta = math.radians(s*(30+60*slot))
        closed[name] = Matrix.Translation((1.20*math.sin(theta),-1.20*math.cos(theta),1.90)) @ \
            Euler((0,0,theta)).to_matrix().to_4x4()

for obj in bpy.data.collections['EXPORT_Boss03'].objects:
    name = obj.get('PartBone','')
    if obj.type != 'MESH' or name not in opened:
        continue
    if not any(part in obj.name for part in ['Shield','Blade','Spine','LightSocket','AttackLight','BaseLatch','RearVent','Fastener']):
        continue
    to_local = opened[name].inverted() @ obj.matrix_world
    from_local = to_local.inverted()
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    for v in bm.verts:
        p = to_local @ v.co
        t = max(0,min(1,(p.z-1.90)/1.86))
        p.y -= 1.43*t*t
        p.x /= 1.36
        v.co = p
    # 大面積の面だけ分割し、隣接した装甲が異なる直線補間で交差することを防ぐ。
    bmesh.ops.triangulate(bm,faces=[f for f in bm.faces if f.calc_area() > .035])
    for _ in range(3):
        long_edges = [e for e in bm.edges if e.calc_length() > .18]
        if long_edges:
            bmesh.ops.subdivide_edges(bm,edges=long_edges,cuts=1,use_grid_fill=True)
    for v in bm.verts:
        p = v.co.copy()
        p.x *= 1.36
        t = max(0,min(1,(p.z-1.90)/1.86))
        p.y += 1.43*t*t
        v.co = from_local @ p
    bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces))
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    obj.data.normals_split_custom_set([(0,0,0)]*len(obj.data.loops))
    obj.vertex_groups[name].add(list(range(len(obj.data.vertices))),1.0,'REPLACE')


def mix(a,b,t):
    la,qa,sa = a.decompose()
    lb,qb,sb = b.decompose()
    return Matrix.LocRotScale(la.lerp(lb,t),qa.slerp(qb,t),sa.lerp(sb,t))


closures = {
    'Boss03_Cocoon_Idle':[(1,1),(31,1),(61,1),(91,1),(121,1)],
    'Boss03_Close':[(1,0),(15,.05),(45,1),(61,1)],
    'Boss03_Deploy':[(1,1),(18,1),(54,.08),(72,0),(91,0)],
    'Boss03_Dash_InPlace':[(1,0),(23,1),(33,1),(54,1),(65,.65),(91,0)],
    'Boss03_Death':[(1,0),(24,.18),(55,.65),(85,.72),(121,.72)],
    'Boss03_Showcase':[(1,0),(31,0),(81,1),(121,1),(151,1),(211,0),(241,0)]}
for action_name,keys in closures.items():
    action = bpy.data.actions[action_name]
    rig.animation_data.action = action
    rig.animation_data.action_slot = action.slots[0]
    for frame,factor in keys:
        scene.frame_set(frame)
        bpy.context.view_layer.update()
        parent_matrix = rig.pose.bones['Root'].matrix @ rig.data.bones['Root'].matrix_local.inverted()
        for name,open_matrix in opened.items():
            bone = rig.pose.bones[name]
            target = mix(open_matrix,closed[name],factor)
            bone.matrix = parent_matrix @ target @ open_matrix.inverted() @ rig.data.bones[name].matrix_local
            bone.keyframe_insert(data_path='location',frame=frame,group=name)
            bone.keyframe_insert(data_path='rotation_quaternion',frame=frame,group=name)
            bone.keyframe_insert(data_path='scale',frame=frame,group=name)

action = bpy.data.actions['Boss03_Stagger']
rig.animation_data.action = action
rig.animation_data.action_slot = action.slots[0]
for frame,t in [(1,0),(18,1),(30,1),(111,1),(151,0)]:
    scene.frame_set(frame)
    bpy.context.view_layer.update()
    parent_matrix = rig.pose.bones['Root'].matrix @ rig.data.bones['Root'].matrix_local.inverted()
    for side,s in [('L',-1),('R',1)]:
        name = f'Wing_{side}_Lower'
        spread = Matrix.Translation((s*.92,.22,3.35)) @ Euler((0,math.radians(s*103),0)).to_matrix().to_4x4()
        bone = rig.pose.bones[name]
        bone.matrix = parent_matrix @ mix(opened[name],spread,t) @ opened[name].inverted() @ rig.data.bones[name].matrix_local
        for channel in ['location','rotation_quaternion','scale']:
            bone.keyframe_insert(data_path=channel,frame=frame,group=name)

for mat_name,strength in [('B03_Emission_Coral',1.8),('B03_Emission_Core',2.7)]:
    bpy.data.materials[mat_name].node_tree.nodes.get('Principled BSDF').inputs['Emission Strength'].default_value = strength
rig['SurfaceFinished'] = True
rig.animation_data.action = original_action
rig.animation_data.action_slot = original_action.slots[0]
scene.frame_set(1)
scene.camera = bpy.data.objects['Camera_Hero']
bpy.data.collections['STUDIO_Preview_Only'].hide_viewport = True
bpy.ops.wm.save_as_mainfile(filepath=bpy.data.filepath)
bpy.data.collections['STUDIO_Preview_Only'].hide_viewport = False
scene.cycles.samples = 24
for filename,frame in [('Boss03_Closed.png',81),('Boss03_Open.png',1)]:
    scene.frame_set(frame)
    scene.render.filepath = str(out/filename)
    bpy.ops.render.render(write_still=True)
