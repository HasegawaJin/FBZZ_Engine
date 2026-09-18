"""Tripo の原本からゲーム用複製を作り、密度と座標を整える。

Author: Hasegawa Jin
Date: 2026-09-13
"""

import bpy
import json
import math
import time
from pathlib import Path
from mathutils import Matrix, Vector

ROOT = Path(r"C:\Users\jinhs\Downloads\FBZZ_Engine")
OUT = ROOT / "GreenWare/Assets/_src/MiniBotC/GameReady"
OUT.mkdir(parents=True, exist_ok=True)
START = time.time()


def Log(message):
    print(f"MINIBOT [{time.time() - START:.1f}s] {message}", flush=True)


def Activate(obj):
    bpy.ops.object.select_all(action='DESELECT')
    obj.hide_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def Components(obj):
    mesh = obj.data
    adjacency = [[] for _ in mesh.vertices]
    for edge in mesh.edges:
        a, b = edge.vertices
        adjacency[a].append(b)
        adjacency[b].append(a)
    seen = set()
    result = []
    for v in mesh.vertices:
        if v.index in seen:
            continue
        seen.add(v.index)
        stack, indices = [v.index], []
        while stack:
            i = stack.pop()
            indices.append(i)
            for j in adjacency[i]:
                if j not in seen:
                    seen.add(j)
                    stack.append(j)
        coords = [mesh.vertices[i].co for i in indices]
        result.append({'vertices': len(indices), 'bounds': [
            [round(min(c[k] for c in coords), 5), round(max(c[k] for c in coords), 5)]
            for k in range(3)]})
    return sorted(result, key=lambda c: c['vertices'], reverse=True)


source_scene = bpy.context.scene
sources = sorted([o for o in source_scene.objects if o.type == 'MESH'],
                 key=lambda o: len(o.data.vertices), reverse=True)
assert len(sources) == 2, 'Expected the captured Player and sword only'
scene = bpy.data.scenes.new('MiniBotC_GameReady')
bpy.context.window.scene = scene
scene.unit_settings.system = 'METRIC'
scene.unit_settings.scale_length = 1
scene.render.fps = 30
high_collection = bpy.data.collections.new('SOURCE_HighPoly')
game_collection = bpy.data.collections.new('GAME_LOD0')
scene.collection.children.link(high_collection)
scene.collection.children.link(game_collection)
report = {}

for src, label, height, target in zip(sources, ['Player', 'Sword'], [1.70, 1.39], [80000, 8000]):
    Log(f'Normalize {label}')
    high = src.copy()
    high.data = src.data.copy()
    high.name = 'SOURCE_' + label
    high_collection.objects.link(high)
    matrix = src.matrix_world.copy()
    if label == 'Sword':
        matrix = Matrix.Rotation(math.pi / 2, 4, 'X')
    corners = [matrix @ Vector(c) for c in src.bound_box]
    lo = Vector([min(c[k] for c in corners) for k in range(3)])
    hi = Vector([max(c[k] for c in corners) for k in range(3)])
    origin = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
    factor = height / (hi.z - lo.z)
    transform = Matrix.Scale(factor, 4) @ Matrix.Translation(-origin) @ matrix
    high.data.transform(transform)
    high.matrix_world = Matrix.Identity(4)
    high.data.update()
    low = high.copy()
    low.data = high.data.copy()
    low.name = label + '_LOD0'
    low['source_object'] = high.name
    game_collection.objects.link(low)
    high.hide_set(True)
    high.hide_render = True
    Activate(low)
    modifier = low.modifiers.new('Game triangle budget', 'DECIMATE')
    modifier.ratio = target / len(low.data.polygons)
    modifier.use_collapse_triangulate = True
    modifier.use_symmetry = label == 'Player'
    modifier.symmetry_axis = 'X'
    Log(f'Decimate {label}: {len(low.data.polygons)} to approximately {target}')
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    for poly in low.data.polygons:
        poly.use_smooth = True
    islands = Components(low)
    report[label] = {
        'source_triangles': len(high.data.polygons),
        'triangles': len(low.data.polygons), 'vertices': len(low.data.vertices),
        'height_m': height, 'components': islands,
    }
    Log(f'{label}: {len(low.data.polygons)} triangles, {len(islands)} components')

high_collection.hide_render = True
scene['pipeline_stage'] = 'normalized_and_reduced'
(OUT / 'PreparationReport.json').write_text(json.dumps(report, indent=2), encoding='utf8')
bpy.context.view_layer.update()
bpy.ops.wm.save_as_mainfile(filepath=str(OUT / '01_Reduced.blend'), compress=True)
Log('PREPARATION COMPLETE')
