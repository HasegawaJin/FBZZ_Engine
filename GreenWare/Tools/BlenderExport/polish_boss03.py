"""曲面装甲の解析的法線とコア収納を設定する、初回制作の最終調整。"""

import math
from pathlib import Path

import bpy
from mathutils import Euler, Matrix, Vector

rig = bpy.data.objects['Boss03_Armature']
scene = bpy.context.scene
out = Path(bpy.data.filepath).parent
opened = {}
for side,s in [('L',-1),('R',1)]:
    for tier,angle,z in [('Upper',18,4.24),('Middle',61,3.81),('Lower',139,3.35)]:
        opened[f'Wing_{side}_{tier}'] = Matrix.Translation((s*.92,.22,z)) @ Euler((0,math.radians(s*angle),0)).to_matrix().to_4x4()
for obj in bpy.data.collections['EXPORT_Boss03'].objects:
    name = obj.get('PartBone','')
    if obj.type != 'MESH' or name not in opened:
        continue
    if not any(part in obj.name for part in ['Shield','Blade','Spine','LightSocket','AttackLight','BaseLatch','RearVent','Fastener']):
        continue
    to_local = opened[name].inverted() @ obj.matrix_world
    normal_transform = to_local.inverted().to_3x3().inverted().transposed()
    positions = []
    for v in obj.data.vertices:
        p = to_local @ v.co
        t = max(0,min(1,(p.z-1.90)/1.86))
        p.y -= 1.43*t*t
        p.x /= 1.36
        positions.append(p)
    normals = [(0,0,1)]*len(obj.data.loops)
    for poly in obj.data.polygons:
        poly.use_smooth = True
        ids = list(poly.vertices)
        source = Vector((0,0,0))
        for i,vid in enumerate(ids):
            source += positions[vid].cross(positions[ids[(i+1)%len(ids)]])
        if source.length < 1e-9:
            source = Vector((0,-1,0))
        source.normalize()
        for li in poly.loop_indices:
            p = positions[obj.data.loops[li].vertex_index]
            t = max(0,min(1,(p.z-1.90)/1.86))
            slope = 2*1.43*t/1.86 if 1.90 < p.z < 3.76 else 0
            normal = normal_transform @ Vector((source.x/1.36,source.y,source.z-slope*source.y))
            normals[li] = normal.normalized()
    obj.data.normals_split_custom_set(normals)
original_action = rig.animation_data.action
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
        bone = rig.pose.bones['Core']
        bone.location.y = .85*factor
        bone.keyframe_insert(data_path='location',frame=frame,group='Core')
rig.animation_data.action = original_action
rig.animation_data.action_slot = original_action.slots[0]
scene.frame_set(1)
scene.camera = bpy.data.objects['Camera_Hero']
bpy.data.collections['STUDIO_Preview_Only'].hide_viewport = True
bpy.ops.wm.save_as_mainfile(filepath=bpy.data.filepath)
bpy.data.collections['STUDIO_Preview_Only'].hide_viewport = False
scene.cycles.samples = 24
for filename,frame in [('Boss03_Open.png',1),('Boss03_Closed.png',81)]:
    scene.frame_set(frame)
    scene.render.filepath = str(out/filename)
    bpy.ops.render.render(write_still=True)
