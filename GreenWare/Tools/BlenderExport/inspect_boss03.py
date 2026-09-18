"""Boss03 の保存済み原本を読み、非破壊で形状検査と確認画像を出力する。"""

import json
import math
from pathlib import Path

import bpy
from mathutils import Vector

scene = bpy.context.scene
out = Path(bpy.data.filepath).parent
rig = bpy.data.objects['Boss03_Armature']
meshes = [o for o in bpy.data.collections['EXPORT_Boss03'].objects if o.type == 'MESH']
original_action = rig.animation_data.action
original_frame = scene.frame_current
checks = {'meshes':len(meshes),'bones':len(rig.data.bones),
          'vertices':sum(len(o.data.vertices) for o in meshes),
          'triangles':sum(sum(len(p.vertices)-2 for p in o.data.polygons) for o in meshes),
          'actions':{},'issues':[]}
for obj in meshes:
    if not obj.data.uv_layers:
        checks['issues'].append(obj.name + ': missing UV0')
    if any(len(v.groups) != 1 or abs(v.groups[0].weight-1) > 1e-5 for v in obj.data.vertices):
        checks['issues'].append(obj.name + ': invalid rigid weights')
    if not any(m.type == 'ARMATURE' and m.object == rig for m in obj.modifiers):
        checks['issues'].append(obj.name + ': missing armature')
for action in bpy.data.actions:
    if not action.name.startswith('Boss03_'):
        continue
    rig.animation_data.action = action
    if action.slots:
        rig.animation_data.action_slot = action.slots[0]
    frames = [1,int(action.get('Frames',action.frame_range[1]))//2,int(action.get('Frames',action.frame_range[1]))]
    bounds = []
    for frame in frames:
        scene.frame_set(frame)
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        points = [obj.matrix_world @ Vector(p) for source in meshes
                  for obj in [source.evaluated_get(depsgraph)] for p in obj.bound_box]
        if any(not math.isfinite(v) for p in points for v in p):
            checks['issues'].append(f'{action.name}: nonfinite geometry at {frame}')
        low = [min(p[i] for p in points) for i in range(3)]
        high = [max(p[i] for p in points) for i in range(3)]
        bounds.append({'frame':frame,'min':low,'max':high})
    checks['actions'][action.name] = bounds
(out/'Boss03_Validation.json').write_text(json.dumps(checks,indent=2),encoding='utf-8')
print('BOSS03_CHECKS '+json.dumps({'issues':checks['issues'],'actions_checked':len(checks['actions'])}),flush=True)
rig.animation_data.action = original_action
if original_action.slots:
    rig.animation_data.action_slot = original_action.slots[0]
bpy.data.collections['STUDIO_Preview_Only'].hide_viewport = False
scene.cycles.samples = 24
for filename,frame,camera in [('Boss03_Open.png',1,'Camera_Hero'),('Boss03_Closed.png',81,'Camera_Hero'),
                              ('Boss03_Front.png',1,'Camera_Front'),('Boss03_Back.png',1,'Camera_Back')]:
    scene.frame_set(frame)
    scene.camera = bpy.data.objects[camera]
    scene.render.filepath = str(out/filename)
    bpy.ops.render.render(write_still=True)
    print('BOSS03_RENDER '+filename,flush=True)
