"""MiniBot C の新規造形と人型リグ。既存アセットの再生成には使用しない。

Author: Hasegawa Jin
Date: 2026-09-13
"""

import bpy
import math
import json
from pathlib import Path
from mathutils import Vector, Matrix

ROOT_DIR = Path(r"C:\Users\jinhs\Downloads\FBZZ_Engine")
ASSET_DIR = ROOT_DIR / "GreenWare/Assets/_src/MiniBotC"
PREVIEW_DIR = ROOT_DIR / "Docs/Art/MiniBotC"
POINTS = {}
BONE_MAP = {}
PARTS = []


def BuildRig():
    scene = bpy.data.scenes['MiniBotC_Studio']
    bpy.context.window.scene = scene
    meta = bpy.data.objects['MiniBotC_Metarig']
    bpy.context.view_layer.objects.active = meta
    meta.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    bones = meta.data.edit_bones
    for name in ['breast.L', 'breast.R', 'pelvis.L', 'pelvis.R']:
        if name in bones:
            bones.remove(bones[name])
    levels = [1.065, 1.19, 1.32, 1.45, 1.65, 1.73, 1.81, 2.28]
    for i in range(7):
        name = 'spine' if i == 0 else f'spine.{i:03d}'
        bones[name].head = (0, 0, levels[i])
        bones[name].tail = (0, 0, levels[i + 1])
        bones[name].roll = 0
    for side, s in [('L', 1), ('R', -1)]:
        coords = {
            'shoulder': ((s*.09, 0, 1.60), (s*.335, 0, 1.61)),
            'upper_arm': ((s*.335, 0, 1.61), (s*.515, .018, 1.275)),
            'forearm': ((s*.515, .018, 1.275), (s*.67, -.013, .975)),
            'hand': ((s*.67, -.013, .975), (s*.727, -.02, .86)),
            'thigh': ((s*.19, 0, 1.115), (s*.225, -.047, .645)),
            'shin': ((s*.225, -.047, .645), (s*.23, .01, .18)),
            'foot': ((s*.23, .01, .18), (s*.23, -.185, .068)),
            'toe': ((s*.23, -.185, .068), (s*.23, -.33, .068)),
            'heel.02': ((s*.23-.085, .092, .012), (s*.23+.085, .092, .012)),
        }
        for stem, (head, tail) in coords.items():
            b = bones[f'{stem}.{side}']
            b.head, b.tail = head, tail
            b.align_roll(Vector((0, 1, 0)))
            POINTS[b.name] = (Vector(head), Vector(tail))
        wrist = Vector(coords['hand'][0])
        direction = (Vector(coords['hand'][1]) - wrist).normalized()
        across = Vector((s*.896, 0, .444)).normalized()
        for finger, offset, length in [('index', -.052, .103), ('middle', -.017, .115),
                                       ('ring', .019, .107), ('pinky', .052, .084)]:
            h = wrist + direction*.114 + across*offset
            for j, fraction in enumerate([.43, .33, .24]):
                t = h + direction*length*fraction + Vector((0, -.003, 0))
                name = f'f_{finger}.{j+1:02d}.{side}'
                b = bones.get(name) or bones.new(name)
                b.head, b.tail = h, t
                b.parent = bones[f'hand.{side}' if j == 0 else f'f_{finger}.{j:02d}.{side}']
                b.use_connect = j > 0
                b.align_roll(Vector((0, 1, 0)))
                POINTS[name] = (h.copy(), t.copy())
                h = t
        h = wrist + direction*.042 - across*.069 + Vector((0, -.006, 0))
        td = (direction*.7 - across*.65 + Vector((0, -.25, 0))).normalized()
        for j, length in enumerate([.037, .030, .025]):
            t = h + td*length
            name = f'thumb.{j+1:02d}.{side}'
            b = bones.get(name) or bones.new(name)
            b.head, b.tail = h, t
            b.parent = bones[f'hand.{side}' if j == 0 else f'thumb.{j:02d}.{side}']
            b.use_connect = j > 0
            b.align_roll(Vector((0, 1, 0)))
            POINTS[name] = (h.copy(), t.copy())
            h = t
    bpy.ops.object.mode_set(mode='OBJECT')
    for side in ['L', 'R']:
        for stem in ['upper_arm', 'thigh']:
            p = meta.pose.bones[f'{stem}.{side}'].rigify_parameters
            p.segments = 1
            p.bbones = 1
            p.rotation_axis = 'x'
        for finger in ['f_index', 'f_middle', 'f_ring', 'f_pinky', 'thumb']:
            pb = meta.pose.bones[f'{finger}.01.{side}']
            pb.rigify_type = 'limbs.super_finger'
            pb.rigify_parameters.rotation_axis = 'x'
    bpy.ops.pose.rigify_generate()
    control = bpy.context.object
    control.name = 'MiniBotC_ControlRig'
    control.show_in_front = True
    for pb in control.pose.bones:
        if 'IK_Stretch' in pb:
            pb['IK_Stretch'] = 0.0
        if 'stretch_length' in pb:
            pb['stretch_length'] = 1.0
    meta.hide_set(True)
    meta.hide_render = True
    mapping = {'Root': 'root', 'Hips': 'ORG-spine', 'Spine': 'ORG-spine.001',
               'Chest': 'ORG-spine.002', 'UpperChest': 'ORG-spine.003',
               'Neck': 'ORG-spine.004', 'Head': 'ORG-spine.006'}
    parents = {'Root': None, 'Hips': 'Root', 'Spine': 'Hips', 'Chest': 'Spine',
               'UpperChest': 'Chest', 'Neck': 'UpperChest', 'Head': 'Neck'}
    for side, prefix in [('L', 'Left'), ('R', 'Right')]:
        for chain, initial in [([('Shoulder', 'shoulder'), ('Arm', 'upper_arm'),
                                ('ForeArm', 'forearm'), ('Hand', 'hand')], 'UpperChest'),
                               ([('UpLeg', 'thigh'), ('Leg', 'shin'),
                                 ('Foot', 'foot'), ('ToeBase', 'toe')], 'Hips')]:
            parent = initial
            for target, source in chain:
                name = prefix + target
                mapping[name] = f'ORG-{source}.{side}'
                parents[name] = parent
                parent = name
                BONE_MAP[f'{source}.{side}'] = name
        for stem, label in [('f_index', 'Index'), ('f_middle', 'Middle'), ('f_ring', 'Ring'),
                            ('f_pinky', 'Pinky'), ('thumb', 'Thumb')]:
            parent = prefix + 'Hand'
            for j in range(1, 4):
                name = f'{prefix}Hand{label}{j}'
                source = f'{stem}.{j:02d}.{side}'
                mapping[name] = 'ORG-' + source
                parents[name] = parent
                parent = name
                BONE_MAP[source] = name
    bpy.ops.object.select_all(action='DESELECT')
    data = bpy.data.armatures.new('MiniBotC_Humanoid')
    export = bpy.data.objects.new('MiniBotC_Humanoid', data)
    scene.collection.objects.link(export)
    bpy.context.view_layer.objects.active = export
    export.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    for name, source in mapping.items():
        source_bone = control.data.bones[source]
        b = data.edit_bones.new(name)
        b.matrix = source_bone.matrix_local.copy()
        b.length = source_bone.length
        b.use_deform = name != 'Root'
        if parents[name]:
            b.parent = data.edit_bones[parents[name]]
    bpy.ops.object.mode_set(mode='OBJECT')
    for name, source in mapping.items():
        c = export.pose.bones[name].constraints.new('COPY_TRANSFORMS')
        c.name = 'Rigify evaluated pose'
        c.target = control
        c.subtarget = source
        c.owner_space = c.target_space = 'POSE'
    export['humanoid_mapping'] = json.dumps(mapping)
    export['authoring_rig'] = control.name
    export['asset_version'] = 'C / single green sword / 2026-09-13'
    export.hide_set(True)
    export.hide_render = True
    for bone in data.bones:
        bone.bbone_segments = 1
    bpy.context.view_layer.update()
    return control, export


if __name__ == '__main__':
    BuildRig()
