"""参照から制作した剣モーションの握り・床・剣の干渉を評価する。"""

import math

import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree


def review(s,labels,action_prefix="REFERENCE_REVIEW_",step=.25,require_two_handed=True,weapon_released=False):
    for w in bpy.context.window_manager.windows:
        if w.screen.is_animation_playing:
            with bpy.context.temp_override(window=w):
                bpy.ops.screen.animation_cancel(restore_frame=False)
    body=s.base.body;weapon=bpy.data.objects['Sword_LOD0'];groups={g.index:g.name for g in body.vertex_groups}
    dominant={v.index:groups[max(v.groups,key=lambda g:g.weight).group] if v.groups else '' for v in body.data.vertices}
    categories={'head':{'Head','Neck'},'torso':{'Hips','Spine','Chest','UpperChest'},'legs':{'RightUpLeg','RightLeg','LeftUpLeg','LeftLeg'}}
    faces={k:[tuple(p.vertices) for p in body.data.polygons if all(dominant[i] in names for i in p.vertices)] for k,names in categories.items()}
    blade=[tuple(p.vertices) for p in weapon.data.polygons if all(weapon.data.vertices[i].co.z>.20 for i in p.vertices)]
    reports={}
    for label in labels:
     action=bpy.data.actions[action_prefix+label];s.control.animation_data.action=action;s.control.animation_data.action_slot=action.slots[0]
     r={'wrist_max':{'R':[0,0],'L':[0,0]},'palm_max_m':0.,'sword_min_m':100.,'feet_min_m':100.,'arm_joint_gap_m':0.,'max_step_rotation':{},'root_drift_m':0.,'collisions':[],'fast_steps':[],'body_min_m':[100,0]}
     previous={};root=None
     for f in np.arange(1,float(action['end_frame'])+.01,step):
      s.scene.frame_set(int(f),subframe=float(f%1));s.grip.update()
      if require_two_handed:
       assert abs(s.control['two_hand_grip']-1)<1e-6
      for side,angle in s.grip.wrist_angles().items():
       if angle>r['wrist_max'][side][0]:r['wrist_max'][side]=[angle,float(f)]
       prefix='Right' if side=='R' else 'Left'
       hand=s.game.pose.bones[prefix+'Hand']
       target=s.grip.sword.matrix_world@Vector((0,0,.055 if side=='R' else -.085))
       if not weapon_released and (side=='R' or s.control['two_hand_grip']>=1.-1e-6):
        r['palm_max_m']=max(r['palm_max_m'],(hand.matrix@s.grip.palms[side]-target).length)
       r['arm_joint_gap_m']=max(r['arm_joint_gap_m'],(s.game.pose.bones[prefix+'ForeArm'].tail-hand.head).length)
      for name in ['Hips','Chest','Head','RightArm','RightForeArm','RightHand','LeftArm','LeftForeArm','LeftHand','RightUpLeg','RightLeg','LeftUpLeg','LeftLeg','Root']:
       bone=s.game.pose.bones[name];q=bone.matrix.to_quaternion()
       assert all(math.isfinite(v) for row in bone.matrix for v in row)
       if name in previous:
        da=math.degrees(previous[name].rotation_difference(q).angle);da=min(da,360-da)
        if da>r['max_step_rotation'].get(name,[0,0])[0]:r['max_step_rotation'][name]=[da,float(f)]
        if da>32:r['fast_steps'].append([name,float(f),da])
       previous[name]=q.copy()
      if root is None:root=s.game.pose.bones['Root'].head.copy()
      r['root_drift_m']=max(r['root_drift_m'],(s.game.pose.bones['Root'].head-root).length)
      ev=body.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh();verts=[ev.matrix_world@v.co for v in mesh.vertices]
      body_min=min(v.z for v in verts)
      if body_min<r['body_min_m'][0]:r['body_min_m']=[body_min,float(f)]
      for side in ('R','L'):r['feet_min_m']=min(r['feet_min_m'],min(verts[i].z for i in s.helpers['foot_ids'][side]))
      sword_vertices=[s.grip.sword.matrix_world@v.co for v in weapon.data.vertices]
      r['sword_min_m']=min(r['sword_min_m'],min(p.z for p in sword_vertices))
      sword_bvh=BVHTree.FromPolygons(sword_vertices,blade)
      for cat,polygons in faces.items():
       overlap=sword_bvh.overlap(BVHTree.FromPolygons(verts,polygons))
       if overlap:r['collisions'].append([float(f),cat,len(overlap)])
      ev.to_mesh_clear()
     reports[label]=r
    return reports
    
