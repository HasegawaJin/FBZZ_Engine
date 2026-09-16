"""MiniBot C の制作済み Action を N パネルから確認する。"""

import bpy
from bpy.props import EnumProperty, StringProperty


RIG_NAME = "MiniBotC_ControlRig"
CLIPS = (
    ("MB_C_Idle", "待機", "POSE_HLT"),
    ("MB_C_SwordWalk", "剣歩き（両手）", "POSE_HLT"),
    ("MB_C_Run_F", "走り（両手）", "ARMATURE_DATA"),
    ("MB_C_RunStop", "走り停止", "POSE_HLT"),
    ("MB_C_Dodge", "回避ローリング", "ARMATURE_DATA"),
    ("MB_C_JumpStart", "ジャンプ踏切", "ARMATURE_DATA"),
    ("MB_C_FallLoop", "空中・落下", "POSE_HLT"),
    ("MB_C_Land", "着地", "ARMATURE_DATA"),
    ("MB_C_Slash01", "斬撃 01（両手）", "FORCE_FORCE"),
    ("MB_C_HighSpinAttack", "回転斬り（両手）", "FORCE_FORCE"),
    ("MB_C_JumpAttack", "ジャンプ斬り（両手）", "FORCE_FORCE"),
    ("MB_C_SlideAttack", "スライド斬り（両手）", "FORCE_FORCE"),
    ("MB_C_UnderSlash", "切り上げ（両手）", "FORCE_FORCE"),
    ("MB_C_UnderSlashandUpperSlash", "連続斬り（両手）", "FORCE_FORCE"),
    ("MB_C_ToBlocking", "ガード開始", "FAKE_USER_ON"),
    ("MB_C_Blocking", "ガード（両手）", "FAKE_USER_ON"),
    ("MB_C_GuardHit", "ガードヒット", "FAKE_USER_ON"),
    ("MB_C_BlockingToIdle", "ガード解除", "FAKE_USER_ON"),
    ("MB_C_Hit", "被弾", "ERROR"),
    ("MB_C_Death", "死亡", "CANCEL"),
    ("MB_C_Victory", "勝利（Result）", "CHECKMARK"),
    ("MB_C_DefeatIdle", "敗北（Result）", "ERROR"),
)


def preview_rig(context):
    return context.scene.objects.get(RIG_NAME)


def current_action(context):
    rig = preview_rig(context)
    return rig.animation_data.action if rig and rig.animation_data else None


def stop_playback(context):
    if context.screen and context.screen.is_animation_playing:
        bpy.ops.screen.animation_cancel(restore_frame=False)
        return True
    return False


class MBC_OT_preview_clip(bpy.types.Operator):
    bl_idname = "mbc.preview_clip"
    bl_label = "モーションを選択"
    bl_description = "Action と再生範囲を切り替えます。再生中なら再生を続けます"
    bl_options = {"REGISTER", "UNDO"}

    action_name: StringProperty()

    @classmethod
    def poll(cls, context):
        return preview_rig(context) is not None

    def execute(self, context):
        rig = preview_rig(context)
        action = bpy.data.actions.get(self.action_name)
        if action is None or not action.name.startswith("MB_C_"):
            self.report({"ERROR"}, "指定した MiniBot C の Action がありません")
            return {"CANCELLED"}
        slot = next((s for s in action.slots if s.identifier == "OB" + RIG_NAME), None)
        if slot is None:
            self.report({"ERROR"}, "ControlRig 用の Action スロットがありません")
            return {"CANCELLED"}

        playing = stop_playback(context)
        rig.animation_data_create()
        rig.animation_data.action = action
        rig.animation_data.action_slot = slot
        scene = context.scene
        start = int(action.get("start_frame", action.frame_range[0]))
        end = int(action.get("end_frame", action.frame_range[1]))
        # ループ末尾は先頭の複製なので、二重に再生すると一瞬停止して見える。
        if action.get("loop", False):
            end -= 1
        scene.use_preview_range = False
        scene.frame_start = start
        scene.frame_end = max(start, end)
        scene.frame_set(start)
        context.view_layer.update()
        if playing:
            bpy.ops.screen.animation_play()
        for area in context.screen.areas if context.screen else ():
            area.tag_redraw()
        return {"FINISHED"}


class MBC_OT_preview_transport(bpy.types.Operator):
    bl_idname = "mbc.preview_transport"
    bl_label = "プレビュー操作"
    bl_description = "先頭に戻る・コマ送り・再生と一時停止"

    command: EnumProperty(items=(
        ("FIRST", "先頭", "先頭フレームへ戻して停止"),
        ("PREVIOUS", "前のコマ", "1 フレーム戻して停止"),
        ("PLAY", "再生 / 一時停止", "表示中の範囲を繰り返し再生"),
        ("NEXT", "次のコマ", "1 フレーム進めて停止"),
    ))

    @classmethod
    def poll(cls, context):
        return context.screen is not None and current_action(context) is not None

    def execute(self, context):
        playing = stop_playback(context)
        if self.command == "PLAY":
            if not playing:
                bpy.ops.screen.animation_play()
        else:
            scene = context.scene
            frame = scene.frame_start if self.command == "FIRST" else scene.frame_current + (
                -1 if self.command == "PREVIOUS" else 1)
            scene.frame_set(max(scene.frame_start, min(scene.frame_end, frame)))
        return {"FINISHED"}


class MBC_PT_motion_preview(bpy.types.Panel):
    bl_label = "Player アニメーション"
    bl_idname = "MBC_PT_motion_preview"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "モーション"

    @classmethod
    def poll(cls, context):
        return preview_rig(context) is not None

    def draw(self, context):
        layout = self.layout
        action = current_action(context)
        active_name = action.name if action else ""
        column = layout.column(align=True)
        column.scale_y = 1.45
        for name, label, icon in CLIPS:
            row = column.row(align=True)
            row.enabled = name in bpy.data.actions
            op = row.operator("mbc.preview_clip", text=label, icon=icon,
                              depress=active_name == name)
            op.action_name = name

        box = layout.box()
        label = next((label for name, label, _ in CLIPS if name == active_name), active_name)
        box.label(text="選択中: " + (label or "未選択"))
        if action:
            duration = float(action.get("duration_seconds", 0.0))
            kind = "ループ素材" if action.get("loop", False) else "単発素材"
            box.label(text=f"{duration:.2f} 秒 / {kind}")
        playing = bool(context.screen and context.screen.is_animation_playing)
        row = box.row(align=True)
        row.scale_y = 1.3
        for command, icon in (("FIRST", "REW"), ("PREVIOUS", "PREV_KEYFRAME"),
                              ("PLAY", "PAUSE" if playing else "PLAY"),
                              ("NEXT", "NEXT_KEYFRAME")):
            row.operator("mbc.preview_transport", text="", icon=icon).command = command
        box.prop(context.scene, "frame_current", text="フレーム")
        box.label(text=f"範囲: {context.scene.frame_start} ～ {context.scene.frame_end}")
        box.label(text="確認用に範囲内を繰り返し再生", icon="FILE_REFRESH")
        layout.prop(context.space_data.overlay, "show_overlays", text="リグ・ガイド表示")
        floor = context.scene.objects.get("MiniBotC_PreviewFloor")
        if floor is not None:
            layout.prop(floor, "hide_viewport", text="確認用の床を隠す")
        layout.separator()
        op = layout.operator("mbc.preview_clip", text="基準の構え", icon="ARMATURE_DATA")
        op.action_name = "MB_C_Stance"


CLASSES = (MBC_OT_preview_clip, MBC_OT_preview_transport, MBC_PT_motion_preview)


def unregister():
    for cls in reversed(CLASSES):
        registered = getattr(bpy.types, cls.__name__, None)
        if registered is not None:
            bpy.utils.unregister_class(registered)


def register():
    unregister()
    for cls in CLASSES:
        bpy.utils.register_class(cls)


register()
