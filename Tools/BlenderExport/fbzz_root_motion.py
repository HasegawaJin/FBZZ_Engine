# FBZZ Engine
# fbzz_root_motion.py | Blender >= 4.4 (slotted action 対応)
#
# Root Motion ノードの整備・抽出・復元モジュール。
#
# ── なぜ必要か ───────────────────────────────────────────────────────────
# FBZZ Engine の AnimSubExporter は「アニメーションチャンネル名が
# RootMotion / Root_Motion のノード」だけを Root Motion トラックとして認識する
# (Projects/Editor/src/Import/AnimSubExporter.cpp :: IsRootMotionName)。
# MiniBot リグにはそのノードが存在しないため、そのままでは
# FzAnimV3Extension.hasRootMotion が常に 0 になり、Root Motion 機構が死ぬ。
#
# ── 二重適用に関する制約 ─────────────────────────────────────────────────
# AnimatorSystem::SampleNodeLocal は Root Motion トラックのノードを姿勢評価時に
# bind 姿勢へ固定する。つまり「GameObject を動かす量」と「骨で動かす量」は
# 排他でなければならない。よって Root Motion を持たせる場合は単なるコピーではなく
# *抽出* (Root_Motion へ移す + Root ボーンから引く) が必要になる。
#
# ── MiniBot の実測結果 (2026-07 時点) ────────────────────────────────────
# 全 30 クリップを実測したところ、Root ボーンの正味移動量 (先頭フレーム→末尾)
# はすべて 0 だった。Walk / Run は最初から原地アニメで、Dodge_Roll も
# 「Root ボーンが体の中心まわりに半径 1.1m で円弧を描く」表現であり、
# 接地移動ではない。この円弧を Root Motion として抜くと GameObject が
# 前後に 1.1m 振られたうえ骨が bind 固定され、ロールの見た目が完全に壊れる。
#
# したがって既定モードは "zero" (ノードだけ用意し、キーは恒等) とする。
# 実移動を持つクリップを将来追加したときだけ mode="extract" を指定する。

import math

import bpy
from mathutils import Matrix, Quaternion, Vector

# Root Motion を運ぶ Empty の名前。
# エンジンの IsRootMotionName が "rootmotion" / "root_motion" を大小無視で受ける。
ROOT_MOTION_NAME = "Root_Motion"

# mode="extract" のときに「移動あり」とみなす正味変位のしきい値。
# WHY: ピーク変位ではなく先頭→末尾の正味変位で判定する。原地クリップの
#      揺れ (Idle の ±5mm、Run の ±12mm) を誤って抜き出さないため。
NET_TRANSLATION_EPSILON = 0.01   # 1cm
NET_YAW_EPSILON = math.radians(1.0)


def _assign_first_slot(obj):
    """Blender 4.4+ のスロット付きアクションで最初のスロットを明示割り当てする。

    WHY: 4.4 以降は action を代入しただけではスロット未割り当てになる場合があり、
         その状態ではアクションが評価されず姿勢が動かない。
         4.3 以前には action_slot が無いので存在チェックしてから触る。
    """
    anim = obj.animation_data
    if anim is None or anim.action is None:
        return
    if not hasattr(anim, "action_slot"):
        return
    slots = getattr(anim.action, "slots", None)
    if not slots:
        return
    if anim.action_slot is None:
        anim.action_slot = slots[0]


def assign_action(obj, action):
    obj.animation_data_create()
    obj.animation_data.action = action
    _assign_first_slot(obj)


def ensure_root_motion_empty(armature):
    """Root_Motion Empty を armature 直下 (OBJECT 親) に用意して返す。

    WHY: BONE 親にすると骨の動きを引き継いでしまい、抽出量と二重になる。
         armature オブジェクト空間 = キャラクターのローカル空間なので、
         エンジンの `owner.transform.rotation * deltaPosition` と意味が一致する。
    """
    empty = bpy.data.objects.get(ROOT_MOTION_NAME)
    if empty is None:
        empty = bpy.data.objects.new(ROOT_MOTION_NAME, None)
        empty.empty_display_type = "ARROWS"
        empty.empty_display_size = 0.4
        target_collection = (
            armature.users_collection[0]
            if armature.users_collection
            else bpy.context.scene.collection
        )
        target_collection.objects.link(empty)

    empty.parent = armature
    empty.parent_type = "OBJECT"
    empty.matrix_parent_inverse = Matrix.Identity(4)
    empty.location = (0.0, 0.0, 0.0)
    empty.rotation_mode = "QUATERNION"
    empty.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    empty.scale = (1.0, 1.0, 1.0)
    return empty


def frames_of(action):
    start = int(round(action.frame_range[0]))
    end = int(round(action.frame_range[1]))
    return list(range(start, end + 1))


def _sample_root_world_matrices(scene, armature, root_bone_name, frames):
    """各フレームで Root ボーンのワールド行列を採取する。

    WHY: MiniBot は 43 本のボーンに LIMIT_ROTATION コンストレイントを持つ。
         評価済み depsgraph から pose_bone.matrix を読むことでコンストレイント
         適用後の最終姿勢が得られ、FBX エクスポータのベイク結果と一致する。
    """
    samples = []
    for frame in frames:
        scene.frame_set(frame)
        depsgraph = bpy.context.evaluated_depsgraph_get()
        armature_eval = armature.evaluated_get(depsgraph)
        pose_bone = armature_eval.pose.bones[root_bone_name]
        samples.append(armature_eval.matrix_world @ pose_bone.matrix.copy())
    return samples


def _yaw_about_z(world_matrix):
    """ワールド Z 軸まわりのねじれ角 (Yaw) を swing-twist 分解で取り出す。

    WHY: 「前方ベクトルを水平投影して atan2」方式は、キャラクターが前転などで
         ピッチ 90° を跨ぐ瞬間に前方が真上/真下を向き、Yaw が 180° 飛ぶ。
         Dodge_Roll (X 軸まわり 360° 回転) で実際に発生した。
         Z 軸まわりの twist 成分だけを取り出す本方式なら純粋なピッチ回転に対して
         twist = 0 を返し、飛びが起きない。
    """
    q = world_matrix.to_quaternion()
    # q を「Z 軸まわりの twist」と「残りの swing」に分解し twist 角を返す
    if abs(q.w) < 1.0e-8 and abs(q.z) < 1.0e-8:
        return 0.0
    return 2.0 * math.atan2(q.z, q.w)


def _unwrap(angles):
    """±pi で折り返す角度列を連続化する (回転の累積を正しく積むため)。"""
    if not angles:
        return angles
    result = [angles[0]]
    for value in angles[1:]:
        previous = result[-1]
        delta = value - previous
        while delta > math.pi:
            delta -= 2.0 * math.pi
        while delta < -math.pi:
            delta += 2.0 * math.pi
        result.append(previous + delta)
    return result


def _write_motion_action(empty, action_name, frames, matrices):
    """Root_Motion Empty へ 1 クリップ分のキーを書き込む。"""
    existing = bpy.data.actions.get(action_name)
    if existing is not None:
        bpy.data.actions.remove(existing)
    motion_action = bpy.data.actions.new(action_name)
    motion_action.use_fake_user = True

    assign_action(empty, motion_action)
    empty.rotation_mode = "QUATERNION"
    scene = bpy.context.scene
    for frame, matrix in zip(frames, matrices):
        scene.frame_set(frame)
        empty.location = matrix.translation
        empty.rotation_quaternion = matrix.to_quaternion()
        empty.keyframe_insert("location", frame=frame)
        empty.keyframe_insert("rotation_quaternion", frame=frame)
    return motion_action


def build_root_motion(scene, armature, root_motion_empty, action,
                      mode="zero", root_bone_name="Root"):
    """1 アクション分の Root_Motion トラックを用意する。

    mode="zero"    : ノードだけ用意しキーは恒等。Root ボーンには触らない。
                     原地アニメ (現行 MiniBot 全クリップ) 用の既定。
    mode="extract" : Root ボーンの水平移動 + Yaw を Root_Motion へ移し替え、
                     Root ボーン側から同量を差し引く。正味変位がしきい値未満の
                     場合は誤抽出を避けるため自動的に "zero" 相当へフォールバックする。

    戻り値: dict(統計情報)
    """
    assign_action(armature, action)
    frames = frames_of(action)
    motion_name = f"RM_{action.name}"

    if mode == "zero":
        _write_motion_action(root_motion_empty, motion_name, frames,
                             [Matrix.Identity(4)] * len(frames))
        return {"action": action.name, "mode": "zero", "frames": len(frames),
                "net_translation": 0.0, "net_yaw_deg": 0.0,
                "rebaked_root_bone": False}

    root_world = _sample_root_world_matrices(scene, armature, root_bone_name, frames)
    origin = root_world[0]
    yaws = _unwrap([_yaw_about_z(m) for m in root_world])
    origin_yaw = yaws[0]

    net_translation = Vector((
        root_world[-1].translation.x - origin.translation.x,
        root_world[-1].translation.y - origin.translation.y,
        0.0,
    )).length
    net_yaw = abs(yaws[-1] - origin_yaw)

    # 原地クリップを誤抽出しないためのガード。
    if net_translation < NET_TRANSLATION_EPSILON and net_yaw < NET_YAW_EPSILON:
        _write_motion_action(root_motion_empty, motion_name, frames,
                             [Matrix.Identity(4)] * len(frames))
        return {"action": action.name, "mode": "zero(fallback)",
                "frames": len(frames),
                "net_translation": round(net_translation, 5),
                "net_yaw_deg": round(math.degrees(net_yaw), 3),
                "rebaked_root_bone": False}

    # ── Root Motion 行列 M[f] を組む (水平移動 + Yaw のみ) ────────────────
    # 縦成分を含めない理由: AnimSubExporter は rootMotionApplyY = 0 を書き込むため
    # 縦は GameObject へ適用されない。跳ね・ジャンプ弧は骨側に残すのが正しい。
    motion_matrices = []
    for world, yaw in zip(root_world, yaws):
        translation = Vector((
            world.translation.x - origin.translation.x,
            world.translation.y - origin.translation.y,
            0.0,
        ))
        motion_matrices.append(
            Matrix.Translation(translation) @ Matrix.Rotation(yaw - origin_yaw, 4, "Z")
        )

    _write_motion_action(root_motion_empty, motion_name, frames, motion_matrices)

    # ── Root ボーンから抽出分を差し引く ───────────────────────────────────
    # R' = M⁻¹ @ R。GameObject(M) と骨(R') の合成が元の R に一致する。
    pose_bone = armature.pose.bones[root_bone_name]
    assign_action(armature, action)
    inverse_world = armature.matrix_world.inverted()
    for frame, world, motion in zip(frames, root_world, motion_matrices):
        scene.frame_set(frame)
        pose_bone.matrix = inverse_world @ (motion.inverted() @ world)
        _key_pose_bone(pose_bone, frame, root_bone_name)

    return {"action": action.name, "mode": "extract", "frames": len(frames),
            "net_translation": round(net_translation, 5),
            "net_yaw_deg": round(math.degrees(net_yaw), 3),
            "rebaked_root_bone": True}


def _key_pose_bone(pose_bone, frame, group):
    pose_bone.keyframe_insert("location", frame=frame, group=group)
    if pose_bone.rotation_mode == "QUATERNION":
        pose_bone.keyframe_insert("rotation_quaternion", frame=frame, group=group)
    else:
        pose_bone.keyframe_insert("rotation_euler", frame=frame, group=group)


def restore_root_motion(scene, armature, root_motion_empty, action,
                        root_bone_name="Root"):
    """extract の逆操作。Root_Motion の移動量を Root ボーンへ戻す。

    WHY: 抽出は破壊的操作なので、判断を誤ったときに戻せる経路を必ず用意する。
         R = M @ R' を各フレームで再合成し、Root_Motion キーを恒等へ戻す。
    """
    assign_action(armature, action)
    motion_action = bpy.data.actions.get(f"RM_{action.name}")
    if motion_action is None:
        return {"action": action.name, "restored": False, "reason": "no RM action"}
    assign_action(root_motion_empty, motion_action)

    frames = frames_of(action)
    composed = []
    for frame in frames:
        scene.frame_set(frame)
        depsgraph = bpy.context.evaluated_depsgraph_get()
        armature_eval = armature.evaluated_get(depsgraph)
        empty_eval = root_motion_empty.evaluated_get(depsgraph)
        root_world = armature_eval.matrix_world @ \
            armature_eval.pose.bones[root_bone_name].matrix.copy()
        composed.append(empty_eval.matrix_world.copy() @ root_world)

    pose_bone = armature.pose.bones[root_bone_name]
    inverse_world = armature.matrix_world.inverted()
    for frame, world in zip(frames, composed):
        scene.frame_set(frame)
        pose_bone.matrix = inverse_world @ world
        _key_pose_bone(pose_bone, frame, root_bone_name)

    _write_motion_action(root_motion_empty, f"RM_{action.name}", frames,
                         [Matrix.Identity(4)] * len(frames))
    return {"action": action.name, "restored": True, "frames": len(frames)}
