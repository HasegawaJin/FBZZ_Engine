"""軽量メッシュの正投影と関節位置の検討資料を出力する。"""

import bpy
import json
import numpy as np
from pathlib import Path
from mathutils import Vector

ROOT = Path(r"C:\Users\jinhs\Downloads\FBZZ_Engine")
OUT = ROOT / 'Docs/Art/MiniBotC/GameReady'
OUT.mkdir(parents=True, exist_ok=True)
scene = bpy.data.scenes['MiniBotC_GameReady']
bpy.context.window.scene = scene
player = bpy.data.objects['Player_LOD0']
for obj in scene.objects:
    obj.hide_render = obj != player
xyz = np.empty(len(player.data.vertices) * 3, dtype=np.float32)
player.data.vertices.foreach_get('co', xyz)
xyz = xyz.reshape(-1, 3)
bands = []
for z in np.arange(.05, 1.7, .05):
    q = xyz[np.abs(xyz[:, 2] - z) < .01]
    if len(q):
        bands.append({'z': round(float(z), 3), 'bounds': np.round(np.array([q.min(0), q.max(0)]), 4).tolist()})
(OUT / 'HeightBands.json').write_text(json.dumps(bands, indent=2), encoding='utf8')
hand_reports = []
for limit in [.73, .71, .69, .67]:
    ids = set(np.where((xyz[:, 0] > .30) & (xyz[:, 2] < limit) & (xyz[:, 2] > .50))[0].tolist())
    graph = {i: [] for i in ids}
    for e in player.data.edges:
        a, b = e.vertices
        if a in ids and b in ids:
            graph[a].append(b)
            graph[b].append(a)
    islands = []
    while ids:
        stack = [ids.pop()]
        found = []
        while stack:
            i = stack.pop()
            found.append(i)
            for j in graph[i]:
                if j in ids:
                    ids.remove(j)
                    stack.append(j)
        q = xyz[found]
        if len(q) > 10:
            islands.append({'n': len(q), 'center': q.mean(0).tolist(), 'bounds': [q.min(0).tolist(), q.max(0).tolist()]})
    hand_reports.append({'cut_z': limit, 'islands': islands})
(OUT / 'HandComponents.json').write_text(json.dumps(hand_reports, indent=2), encoding='utf8')
cam_data = bpy.data.cameras.new('InspectionCamera')
cam = bpy.data.objects.new('InspectionCamera', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam
scene.render.engine = 'BLENDER_WORKBENCH'
scene.render.image_settings.file_format = 'PNG'
scene.render.resolution_percentage = 100
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'SINGLE'
scene.display.shading.single_color = (.68, .70, .74)
scene.display.shading.show_cavity = True
scene.display.shading.cavity_type = 'BOTH'
scene.display.shading.show_shadows = True
scene.display.shading.background_type = 'WORLD'
scene.world = bpy.data.worlds.new('InspectionWorld')
scene.world.color = (.16, .16, .16)
cam_data.type = 'ORTHO'
for name, location, target, scale, resolution in [
    ('Front', (0, -4, .85), (0, 0, .85), 1.85, (1000, 1500)),
    ('Side', (4, 0, .85), (0, 0, .85), 1.85, (1000, 1500)),
    ('Face', (0, -4, 1.40), (0, 0, 1.40), .70, (1100, 1100)),
    ('Hand', (-.7, -3, .78), (-.36, 0, .78), .40, (1100, 1100)),
]:
    cam.location = location
    cam.rotation_euler = (Vector(target) - cam.location).to_track_quat('-Z', 'Y').to_euler()
    cam_data.ortho_scale = scale
    scene.render.resolution_x, scene.render.resolution_y = resolution
    scene.render.filepath = str(OUT / (name + '_Geometry.png'))
    bpy.ops.render.render(write_still=True)
    print('INSPECTION', name, flush=True)
