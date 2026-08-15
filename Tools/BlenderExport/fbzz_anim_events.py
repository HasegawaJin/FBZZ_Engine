# FBZZ Engine
# fbzz_anim_events.py | Blender >= 4.4 (slotted action 対応)
#
# アニメーションイベントノードの管理モジュール。
#
# ── 規約 ────────────────────────────────────────────────────────────────
# `FBZZ_EVENT__<イベント名>` という名前の補助ノードの Position を
# AnimSubExporter がイベントとして取り込む (Position X → intParam / Y → floatParam)。
#
# 取り込み側は値を「ステップ信号」として読む:
#   先頭キーの値を静止値 (rest) とみなし、rest と異なる値へ遷移した瞬間だけ発火する。
#   rest へ戻る遷移は発火しない。
#
# よって DCC 側の作法は「普段は静止値、発火させたいフレームで値を変える」だけでよい。
#   - ベイクでキーが毎フレームに増えても無視される (重複値は発火しない)
#   - rest → A → rest → A で同じイベントを何度でも打てる
#   - センチネル値も特別なエクスポート設定も要らない
#
# ── 使い方 ───────────────────────────────────────────────────────────────
# 1. イベントノードを作る:
#       ev = ensure_event_empty(armature, "WeaponAttach")
# 2. クリップごとのキーを書く (アクション名は EV_<アーマチュアのアクション名>):
#       write_events(ev, "EV_MiniBot_Draw_Pistols", [(9, 1, 0.0), (11, 2, 0.0)])
# 3. あとは fbzz_export_minibot が書き出し直前に自動で割り当てる。
#    そのクリップ用の EV_ アクションが無いノードは自動で無効化される。

import bpy
from mathutils import Matrix

EVENT_PREFIX = "FBZZ_EVENT__"
EVENT_ACTION_PREFIX = "EV_"


def _assign_first_slot(obj):
    anim = obj.animation_data
    if anim is None or anim.action is None:
        return
    if not hasattr(anim, "action_slot"):
        return
    slots = getattr(anim.action, "slots", None)
    if slots and anim.action_slot is None:
        anim.action_slot = slots[0]


def ensure_event_empty(armature, event_name):
    """`FBZZ_EVENT__<event_name>` Empty を armature 直下 (OBJECT 親) に用意して返す。

    WHY: OBJECT 親にするのは Root_Motion と同じ理由。BONE 親だと骨の動きを
         引き継いでノードが動いてしまい、イベント値の読み取りが汚れる。
    """
    name = EVENT_PREFIX + event_name
    empty = bpy.data.objects.get(name)
    if empty is None:
        empty = bpy.data.objects.new(name, None)
        empty.empty_display_type = "PLAIN_AXES"
        empty.empty_display_size = 0.25
        target = (armature.users_collection[0]
                  if armature.users_collection else bpy.context.scene.collection)
        target.objects.link(empty)

    empty.parent = armature
    empty.parent_type = "OBJECT"
    empty.matrix_parent_inverse = Matrix.Identity(4)
    empty.location = (0.0, 0.0, 0.0)
    empty.rotation_mode = "QUATERNION"
    empty.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    empty.scale = (1.0, 1.0, 1.0)
    return empty


def write_events(empty, action_name, keys, rest_frame=1, rest=(0, 0.0)):
    """1 クリップ分のイベントキーを書く。

    keys : [(frame, int_param, float_param), ...]
    rest : 静止値 (int_param, float_param)。先頭キーの値がこれになる。
    """
    old = bpy.data.actions.get(action_name)
    if old is not None:
        bpy.data.actions.remove(old)
    action = bpy.data.actions.new(action_name)
    action.use_fake_user = True

    empty.animation_data_create()
    empty.animation_data.action = action
    _assign_first_slot(empty)

    # WHY: 新規アクションのスロット識別子は「そのとき最後に束縛されたオブジェクト」を
    #     拾ってしまうことがあり、別のイベントノードの名前が入る事故が起きた。
    #     ノード名で明示的に上書きして取り違えを防ぐ。
    for slot in getattr(action, "slots", []):
        try:
            slot.name_display = empty.name
        except Exception:
            pass

    scene = bpy.context.scene
    for frame, int_param, float_param in [(rest_frame, rest[0], rest[1])] + list(keys):
        scene.frame_set(frame)
        empty.location = (float(int_param), float(float_param), 0.0)
        empty.keyframe_insert("location", frame=frame)

    # 補間はステップにしておく。ベイク時に中間値が作られて
    # 意図しない遷移 (= 余計なイベント) が生まれるのを防ぐ。
    for layer in getattr(action, "layers", []):
        for strip in layer.strips:
            for channelbag in strip.channelbags:
                for fcurve in channelbag.fcurves:
                    for keyframe in fcurve.keyframe_points:
                        keyframe.interpolation = "CONSTANT"
    return action


def event_empties(armature):
    return [o for o in bpy.data.objects
            if o.type == "EMPTY" and o.name.startswith(EVENT_PREFIX)
            and o.parent is armature]


def event_action_name(armature_action_name, event_name):
    """イベントノード用アクションの命名規則。

    WHY: ノード名まで含めないと、イベントノードが複数あるときに
         同じアクションが全ノードへ割り当たり、Draw クリップで Holster
         イベントまで発火する。ノード単位で一意にする。
    """
    return f"{EVENT_ACTION_PREFIX}{armature_action_name}__{event_name}"


def write_clip_events(armature, armature_action_name, events, rest_frame=1):
    """1 クリップ分のイベントをまとめて書く。

    events : {"WeaponAttach": [(9, 1, 0.0), (11, 2, 0.0)], ...}
    """
    written = []
    for event_name, keys in events.items():
        empty = ensure_event_empty(armature, event_name)
        action = write_events(
            empty, event_action_name(armature_action_name, event_name),
            keys, rest_frame=rest_frame)
        written.append((empty.name, action.name))
    return written


def bind_events_for_action(armature, action):
    """書き出し直前に、各イベントノードへそのクリップ用アクションを割り当てる。

    `EV_<アクション名>__<イベント名>` が存在すればそれを割り当て、
    無ければアクションを外す。アクションが無いノードは静止したまま書き出され、
    取り込み側では「値が一度も変化しない = イベント 0 件」になるので無害。
    """
    bound = []
    for empty in event_empties(armature):
        event_name = empty.name[len(EVENT_PREFIX):]
        empty.animation_data_create()
        event_action = bpy.data.actions.get(event_action_name(action.name, event_name))
        empty.animation_data.action = event_action
        if event_action is not None:
            _assign_first_slot(empty)
            bound.append((empty.name, event_action.name))
    return bound
