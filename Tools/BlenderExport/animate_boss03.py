"""保存済み Boss03 に移動・連撃等のモーションと N サイドバーを差分追加する。"""

import json
import math
import shutil
from pathlib import Path

import bpy
from mathutils import Quaternion, Vector

scene = bpy.context.scene
rig = bpy.data.objects['Boss03_Armature']
path = Path(bpy.data.filepath)
out = path.parent
backup = out/'Boss03_BeforeMotions.blend'
if not backup.exists():
    shutil.copy2(path,backup)


def use_action(action):
    rig.animation_data_create()
    rig.animation_data.action = action
    rig.animation_data.action_slot = action.slots[0]
    rig.animation_data.use_nla = False


def snapshot(action_name, frame):
    use_action(bpy.data.actions['Boss03_'+action_name])
    scene.frame_set(frame-1)
    scene.frame_set(frame)
    bpy.context.view_layer.update()
    return {b.name:(b.location.copy(),b.rotation_quaternion.copy(),b.scale.copy())
            for b in rig.pose.bones if not b.name.startswith('SOCKET_')}


def copy_pose(pose):
    return {n:tuple(v.copy() for v in values) for n,values in pose.items()}


idle = snapshot('Idle',1)
left_windup = snapshot('Slam_L',24)
right_windup = snapshot('Slam_R',24)
left_impact = snapshot('Slam_L',40)
right_impact = snapshot('Slam_R',40)


def make_action(suffix, length, note, anticipation=None, contact=None, recovery=None):
    name = 'Boss03_'+suffix
    old = bpy.data.actions.get(name)
    if old:
        if old.get('MotionPackVersion') == 1:
            raise RuntimeError('モーション追加済みのため再作成しません: '+name)
        raise RuntimeError('同名アクションを保護するため停止します: '+name)
    rig.animation_data.action = None
    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    action['MotionPackVersion'] = 1
    action['Description'] = note
    action['FPS'],action['Frames'] = 30,length
    for key,value in [('AnticipationFrame',anticipation),('ContactFrame',contact),('RecoveryFrame',recovery)]:
        if value is not None:
            action[key] = value
    rig.animation_data.action = action
    return action


def key_pose(frame,pose):
    for name,(loc,rot,scale) in pose.items():
        bone = rig.pose.bones[name]
        bone.rotation_mode = 'QUATERNION'
        bone.location,bone.rotation_quaternion,bone.scale = loc,rot,scale
        for channel in ['location','rotation_quaternion','scale']:
            bone.keyframe_insert(data_path=channel,frame=frame,group=name)


def rotate(pose,name,axis,angle):
    loc,rot,scale = pose[name]
    pose[name] = (loc,rot @ Quaternion(axis,math.radians(angle)),scale)


for suffix,direction in [('Hover_Forward',0),('Hover_Left',-1),('Hover_Right',1)]:
    make_action(suffix,91,'移動用ループ。水平移動はゲーム側で加える')
    for frame,phase in [(1,0),(23,math.pi/2),(46,math.pi),(69,3*math.pi/2),(91,2*math.pi)]:
        pose = copy_pose(idle)
        pose['Root'][0].y += .055*math.sin(phase)
        if direction:
            rotate(pose,'Root',(0,0,1),direction*(8+1.5*math.sin(phase)))
        else:
            rotate(pose,'Root',(1,0,0),7+math.sin(phase))
        for name in pose:
            if name.startswith('Wing_'):
                s = -1 if '_L_' in name else 1
                rotate(pose,name,(0,0,1),s*(3+1.2*math.sin(phase)))
        key_pose(frame,pose)

for suffix,direction in [('Turn_L_InPlace',-1),('Turn_R_InPlace',1)]:
    make_action(suffix,61,'旋回時のバンク。実際の方向転換はゲーム側で加える')
    for frame,t in [(1,0),(16,.75),(31,1),(46,.65),(61,0)]:
        pose = copy_pose(idle)
        rotate(pose,'Root',(0,0,1),direction*12*t)
        rotate(pose,'Body',(0,1,0),direction*16*t)
        for name in pose:
            if name.startswith('Wing_'):
                rotate(pose,name,(0,0,1),direction*9*t)
        key_pose(frame,pose)

make_action('Hit',31,'小被弾の加算前提ではない全身リアクション',contact=6,recovery=18)
for frame,t in [(1,0),(6,1),(12,-.35),(20,.10),(31,0)]:
    pose = copy_pose(idle)
    rotate(pose,'Root',(1,0,0),-6*t)
    rotate(pose,'Body',(1,0,0),-5*t)
    pose['Root'][0].y -= .06*t
    for name in pose:
        if name.startswith('Wing_'):
            rotate(pose,name,(1,0,0),5*t)
    key_pose(frame,pose)

make_action('Pulse',101,'全翼で溜めて開放するパルス攻撃。発光と衝撃波はゲーム側',
            anticipation=23,contact=43,recovery=70)
for frame,t,bob in [(1,0,0),(23,-1,.10),(35,-1,.15),(43,1,-.12),(54,.40,-.05),(70,.08,0),(101,0,0)]:
    pose = copy_pose(idle)
    pose['Root'][0].y += bob
    pose['Core'][0].y -= .13*max(0,t)
    for name in pose:
        if name.startswith('Wing_'):
            s = -1 if '_L_' in name else 1
            rotate(pose,name,(0,0,1),s*10*t)
            rotate(pose,name,(1,0,0),-8*t)
    key_pose(frame,pose)

make_action('Slam_Combo',151,'左・右・溜め直した両翼の三連撃',anticipation=20,contact=32,recovery=120)
for frame,phase in [(1,'IDLE'),(20,'WIND_L'),(27,'WIND_L'),(32,'HIT_L'),(43,'HIT_L'),
                    (54,'WIND_R'),(60,'WIND_R'),(65,'HIT_R'),(77,'HIT_R'),
                    (94,'WIND_BOTH'),(107,'WIND_BOTH'),(114,'HIT_BOTH'),(132,'HIT_BOTH'),(151,'IDLE')]:
    pose = copy_pose(idle)
    for side,windup,impact in [('L',left_windup,left_impact),('R',right_windup,right_impact)]:
        if phase.endswith(side) or phase.endswith('BOTH'):
            source = windup if phase.startswith('WIND') else impact
            name = f'Wing_{side}_Middle'
            pose[name] = tuple(v.copy() for v in source[name])
    if phase.startswith('HIT'):
        pose['Root'][0].y -= .18
    elif phase.startswith('WIND'):
        pose['Root'][0].y += .08
    key_pose(frame,pose)

for suffix,anticipation,contact,recovery in [('Slam_L',24,40,65),('Slam_R',24,40,65),
    ('Laser_Fan',26,40,77),('Dash_InPlace',23,33,65),('Stagger',1,18,112),
    ('Deploy',18,54,72),('Close',15,45,61),('Death',24,55,85)]:
    action = bpy.data.actions['Boss03_'+suffix]
    action['AnticipationFrame'],action['ContactFrame'],action['RecoveryFrame'] = anticipation,contact,recovery

# 自動ハンドルの行き過ぎで装甲が逆方向へ振れることを防ぐ。
for action in bpy.data.actions:
    if action.get('MotionPackVersion') != 1:
        continue
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                for curve in bag.fcurves:
                    for key in curve.keyframe_points:
                        key.handle_left_type = 'AUTO_CLAMPED'
                        key.handle_right_type = 'AUTO_CLAMPED'

source = (out/'Boss03_Preview.py').read_text(encoding='utf-8')
embedded = bpy.data.texts.get('Boss03_Preview.py') or bpy.data.texts.new('Boss03_Preview.py')
embedded.clear()
embedded.write(source)
embedded.use_module = True
namespace = {'__name__':'Boss03_Preview'}
exec(compile(source,'Boss03_Preview.py','exec'),namespace)

checks = []
for suffix,label,group in namespace['CLIPS']:
    result = bpy.ops.boss03.preview_clip(clip=suffix,play=False)
    action = bpy.data.actions['Boss03_'+suffix]
    assert result == {'FINISHED'} and rig.animation_data.action == action
    assert scene.frame_start == int(action.frame_range[0])
    assert scene.frame_end == int(action.frame_range[1])
    scene.frame_set(scene.frame_start)
    start = [tuple(b.matrix.translation) for b in rig.pose.bones]
    for frame in [scene.frame_start,(scene.frame_start+scene.frame_end)//2,scene.frame_end]:
        scene.frame_set(frame)
        bpy.context.view_layer.update()
        assert all(math.isfinite(x) for b in rig.pose.bones for row in b.matrix for x in row)
    checks.append({'action':action.name,'frames':int(action.frame_range[1]),'button':'OK'})
for state,expected in [('FOUR',4),('TWO',2),('SIX',6)]:
    scene.boss03_preview.damage = state
    visible = {obj.get('PartBone') for obj in scene.objects if obj.get('PartBone','').startswith('Wing_') and not obj.hide_get()}
    assert len(visible) == expected,(state,visible)
namespace['register']()
assert len([h for h in bpy.app.handlers.frame_change_post if h.__name__ == 'boss03_preview_frame_handler']) == 1
bpy.ops.boss03.preview_clip(clip='Idle',play=False)
scene.boss03_preview.damage = 'SIX'
scene.boss03_preview.loop = True
scene.render.fps = 30
scene.timeline_markers.clear()
scene.camera = bpy.data.objects['Camera_Hero']
rig['MotionPackVersion'] = 1
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            area.spaces.active.show_region_ui = True

info = bpy.data.texts.get('START_HERE_Boss03.txt')
if info:
    info.clear()
    info.write('''BOSS03 / モーション確認

3D Viewport > N > Boss03
各ボタンでモーションを切り替えて再生します。
再生 / 一時停止、先頭へ戻す、ループ切り替えを利用できます。
予兆 / 攻撃 / 復帰ボタンで該当フレームへ移動します。
六翼 / 四翼 / 二翼は部位破壊後の見た目の確認用です。

N タブがない場合:
Scripting ワークスペースで Boss03_Preview.py を選択し Run Script。
スクリプトの自動実行設定はこのファイルから変更しません。

25 clips / 30 fps. Meshes and rig geometry are unchanged.
New: Hover_Forward/Left/Right, Turn_L/R_InPlace, Hit, Pulse, Slam_Combo.
Locomotion clips stay in place. World movement, damage, attack lights,
laser effects and gameplay timing are connected in the game separately.
''')

report = {'clips':checks,'clip_count':len(checks),'damage_preview':'6 / 4 / 2 OK',
          'registration':'repeat registration OK',
          'auto_run_enabled_in_existing_preferences':bpy.context.preferences.filepaths.use_scripts_auto_execute}
(out/'Boss03_MotionValidation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
bpy.ops.wm.save_as_mainfile(filepath=str(path))
print('BOSS03_MOTIONS '+json.dumps(report,ensure_ascii=False),flush=True)
