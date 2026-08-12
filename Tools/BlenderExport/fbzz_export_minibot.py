# FBZZ Engine
# fbzz_export_minibot.py | Blender >= 4.4 (slotted action 対応)
#
# MiniBot (Player.blend) を FBZZ Engine のアセット規約へ書き出すエクスポーター。
#
# ── FBZZ 側の受け取り規約 ────────────────────────────────────────────────
#   AssetManager は「パッケージフォルダ Foo/ の隣に原本 Foo.fbx」を前提にする
#   (Projects/Engine/src/Asset/AssetManager.cpp)。Editor の FbxImportTool が
#   Foo.fbx を読み、Foo/ 以下へ .fzasset / .mesh / .skel / anims/*.anim を吐く。
#   よって本スクリプトは Assets/Models/MiniBot/ 直下へ *.fbx を並べるだけでよい。
#
#   AnimSubExporter は 1 FBX 内の全 AnimStack (テイク) をループして
#   `<FBX名>@<テイク名>.anim` を生成する。テイク名の衝突・武器リグへの
#   アクション総当たりを避けるため、本スクリプトは
#   **1 アクション = 1 FBX = 1 テイク** で書き出す (既存 DebugCharacter と同じ構成)。
#
#   テイク名は Blender FBX エクスポータの仕様上「シーン名」になる
#   (bake_anim_use_all_actions=False のとき AnimStack はシーン名で 1 本だけ作られる)。
#   そこで書き出し中だけシーン名をクリップ名へ差し替え、終了後に復元する。
#   → 生成物は `MiniBot_Idle@Idle.anim` のように読める名前になる。
#
# ── Root Motion ─────────────────────────────────────────────────────────
#   fbzz_root_motion.extract_root_motion() が Root ボーンの水平移動 + Yaw を
#   Root_Motion Empty へ移し替える。エンジンはチャンネル名 Root_Motion を見て
#   自動的に Root Motion トラックとして扱う。詳細は fbzz_root_motion.py を参照。
#
# ── 使い方 ───────────────────────────────────────────────────────────────
#   Blender の Scripting タブから:
#       import sys; sys.path.append(r"<repo>/Tools/BlenderExport")
#       import fbzz_export_minibot as fx; import importlib; importlib.reload(fx)
#       fx.main(r"<repo>/Assets/Models")
#   もしくはコマンドラインから:
#       blender Player.blend --background --python fbzz_export_minibot.py -- <出力先>

import os
import sys

import bpy
from mathutils import Matrix

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbzz_root_motion as rm  # noqa: E402
import fbzz_anim_events as ev  # noqa: E402

# ── リグ定義 ────────────────────────────────────────────────────────────
# package : 出力パッケージ名 (= FBX のファイル名の接頭辞 / フォルダ名)
# prefix  : このリグに属するアクションの接頭辞
# root_motion: Root Motion 抽出を行うか (武器リグは不要)
RIGS = [
    {
        "package": "MiniBot",
        "armature": "MiniBot_Armature",
        "prefix": "MiniBot_",
        "root_bone": "Root",
        "root_motion": True,
    },

    {
        "package": "WPN_Assault",
        "armature": "WPN_Assault_Rig",
        "prefix": "Assault_",
        "root_bone": None,
        "root_motion": False,
    },
    {
        "package": "WPN_Pistol_R",
        "armature": "WPN_Pistol_R_Rig",
        "prefix": "Pistol_",
        "root_bone": None,
        "root_motion": False,
        # 左右のピストルは別アーマチュアだが同じ Pistol_ 接頭辞を共有するため、
        # スロット (slot identifier) で所属を判定する。
        "slot_filter": "OBWPN_Pistol_R_Rig",
    },
    {
        "package": "WPN_Pistol_L",
        "armature": "WPN_Pistol_L_Rig",
        "prefix": "Pistol_",
        "root_bone": None,
        "root_motion": False,
        "slot_filter": "OBWPN_Pistol_L_Rig",
    },
    {
        "package": "WPN_Sword_R",
        "armature": "WPN_Sword_R_Rig",
        "prefix": "Sword_",
        "root_bone": None,
        "root_motion": False,
        "slot_filter": "OBWPN_Sword_R_Rig",
    },
    {
        "package": "WPN_Sword_L",
        "armature": "WPN_Sword_L_Rig",
        "prefix": "Sword_",
        "root_bone": None,
        "root_motion": False,
        "slot_filter": "OBWPN_Sword_L_Rig",
    },
]

# Root Motion を「抽出」するクリップ名 (接頭辞除去後)。
#
# WHY: 現行 MiniBot の 30 クリップはすべて原地アニメで、Root ボーンの
#      正味変位は 0 だった。Dodge_Roll も接地移動ではなく体の中心まわりの
#      円弧表現なので、これを抜くと GameObject が前後に 1.1m 振られて壊れる。
#      よって既定は全クリップ mode="zero" (Root_Motion ノードは付けるがキーは恒等)。
#      実移動を持つクリップを追加したらここに名前を足す。
ROOT_MOTION_EXTRACT_CLIPS = set()

# Blender FBX エクスポータの共通設定。
# WHY: FbxImportTool は「Blender が root ノードへ付ける +90°X / scale100」を
#      検出して打ち消す軸補正を持つ (FbxImportTool.cpp :: axisFixNodes)。
#      よって axis_forward / axis_up / apply_scale_options は Blender 既定のまま
#      渡すのが正解で、こちら側で先回りして補正してはいけない。
COMMON_FBX_OPTIONS = dict(
    use_selection=True,
    use_visible=False,
    apply_scale_options="FBX_SCALE_NONE",
    axis_forward="-Z",
    axis_up="Y",
    use_mesh_modifiers=True,
    mesh_smooth_type="FACE",
    use_tspace=True,
    # 余分な _end リーフボーンを出さない。53 本 → 106 本に膨らむとスキニング定数
    # バッファ (MAX_SKINNING_BONES = 128) の余裕が無くなるうえリターゲットも汚れる。
    add_leaf_bones=False,
    primary_bone_axis="Y",
    secondary_bone_axis="X",
    armature_nodetype="NULL",
    bake_space_transform=False,
)

# アニメーションベイク設定。
# WHY: MiniBot は 43 本のボーンに LIMIT_ROTATION コンストレイントを持つ。
#      設計方針 (Docs/design/animation-system-v3.md) どおり DCC 側でベイクして
#      Transform キーへ落とす。simplify_factor=0 で間引かず、キー削減は
#      エンジンの OptimizeVectorKeys / OptimizeQuaternionKeys に任せる。
ANIM_BAKE_OPTIONS = dict(
    bake_anim=True,
    bake_anim_use_all_bones=True,
    bake_anim_use_nla_strips=False,
    bake_anim_use_all_actions=False,
    bake_anim_force_startend_keying=True,
    bake_anim_step=1.0,
    bake_anim_simplify_factor=0.0,
)


def action_fcurves(action):
    """Blender 4.4+ のスロット付きアクションから FCurve を取り出す。

    WHY: 4.4 でアクションが layer/strip/channelbag 構造になり、
         action.fcurves が存在しないケースがある。両対応させる。
    """
    try:
        return list(action.fcurves)
    except AttributeError:
        curves = []
        for layer in action.layers:
            for strip in layer.strips:
                for channelbag in strip.channelbags:
                    curves.extend(channelbag.fcurves)
        return curves


def action_slots(action):
    return [slot.identifier for slot in getattr(action, "slots", [])]


def actions_for_rig(rig):
    """リグに属するアクションを接頭辞 + スロットで抽出する。"""
    result = []
    for action in bpy.data.actions:
        if not action.name.startswith(rig["prefix"]):
            continue
        if action.name.startswith("RM_"):
            continue  # Root Motion 用の派生アクションは対象外
        slot_filter = rig.get("slot_filter")
        if slot_filter and slot_filter not in action_slots(action):
            continue
        result.append(action)
    return sorted(result, key=lambda a: a.name)


def collect_hierarchy(root_object, include_types):
    """root_object 以下を再帰的に集める (BONE 親 / OBJECT 親の両方を辿る)。"""
    collected = []
    stack = [root_object]
    while stack:
        current = stack.pop()
        if current.type in include_types:
            collected.append(current)
        stack.extend(current.children)
    return collected


def select_only(objects):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.hide_set(False)
        obj.select_set(True)
    if objects:
        bpy.context.view_layer.objects.active = objects[0]


def clip_name_for(rig, action):
    """アクション名からリグ接頭辞を落として短いクリップ名にする。"""
    name = action.name
    if name.startswith(rig["prefix"]):
        name = name[len(rig["prefix"]):]
    return name or action.name


def normalize_root_transforms(armature):
    """スキンメッシュのオブジェクト変換を恒等・原点ゼロに揃える (冪等)。

    WHY: Blender の FBX エクスポータはアーマチュア変形メッシュを root 直下へ昇格させ、
         各 root 直下子へ Blender 基底 (-90°X / scale100) を焼き込む。
         FbxImportTool の axis fix は「root 直下子の変換がすべて同一」でないと
         `mixed root transforms` として補正自体をスキップし、その結果
         unitScale が 0.01 のまま = モデルが 100 分の 1 になる。
         さらに axis fix は root 直下子の変換を identity へ潰すため、
         平行移動が残っているとボーンのバインド行列とズレてパーツが中央へ寄る。
         よって回転・スケールだけでなく **原点も 0 に揃える** 必要がある。
    """
    changed = {"transform": [], "origin": []}
    meshes = [o for o in bpy.data.objects
              if o.type == "MESH" and o.parent is armature]
    if not meshes:
        return changed

    # 回転・スケールをメッシュデータへ焼き込む
    bpy.context.view_layer.objects.active = meshes[0]
    if bpy.context.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    skewed = [o for o in meshes
              if any(abs(v) > 1e-5 for v in o.rotation_euler)
              or any(abs(v - 1.0) > 1e-5 for v in o.scale)]
    for o in skewed:
        o.hide_set(False)
        o.hide_viewport = False
        o.select_set(True)
    if skewed:
        bpy.context.view_layer.objects.active = skewed[0]
        bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
        changed["transform"] = [o.name for o in skewed]

    # 原点をワールド原点へ移す
    bpy.context.view_layer.update()
    for o in meshes:
        offset = o.matrix_world.translation.copy()
        if offset.length > 1e-9:
            o.data.transform(Matrix.Translation(offset))
            o.location = (0.0, 0.0, 0.0)
            o.matrix_parent_inverse.identity()
            changed["origin"].append(o.name)
    bpy.context.view_layer.update()
    return changed


def export_mesh_package(rig, output_dir, package_name):
    """スキンメッシュ本体の FBX を書き出す (アニメーションなし)。"""
    armature = bpy.data.objects[rig["armature"]]
    normalize_root_transforms(armature)
    targets = [armature] + collect_hierarchy(armature, {"MESH", "EMPTY"})
    select_only(targets)

    path = os.path.join(output_dir, f"{package_name}.fbx")
    bpy.ops.export_scene.fbx(
        filepath=path,
        object_types={"ARMATURE", "MESH", "EMPTY"},
        path_mode="COPY",
        embed_textures=True,
        bake_anim=False,
        **COMMON_FBX_OPTIONS,
    )
    return path


def export_clip(rig, action, output_dir, root_motion_empty):
    """1 アクション = 1 FBX = 1 テイクで書き出す (メッシュなし)。"""
    scene = bpy.context.scene
    armature = bpy.data.objects[rig["armature"]]

    armature.animation_data_create()
    armature.animation_data.action = action
    rm._assign_first_slot(armature)

    targets = [armature] + collect_hierarchy(armature, {"EMPTY"})
    select_only(targets)

    frame_start = int(round(action.frame_range[0]))
    frame_end = int(round(action.frame_range[1]))

    # ベイク範囲とテイク名を一時的に差し替える
    saved = (scene.name, scene.frame_start, scene.frame_end)
    clip = clip_name_for(rig, action)
    scene.frame_start = frame_start
    scene.frame_end = frame_end
    scene.name = clip

    # ファイル名はクリップ名のみ。既存の DebugCharacter/Idle.fbx と同じ規約に合わせる
    # (FbxImportTool が Idle.fbx → Idle/ パッケージを作る)。
    path = os.path.join(output_dir, f"{clip}.fbx")
    try:
        bpy.ops.export_scene.fbx(
            filepath=path,
            object_types={"ARMATURE", "EMPTY"},
            path_mode="AUTO",
            embed_textures=False,
            **COMMON_FBX_OPTIONS,
            **ANIM_BAKE_OPTIONS,
        )
    finally:
        scene.name, scene.frame_start, scene.frame_end = saved

    return path


def main(models_dir, rig_names=None, clip_filter=None, export_mesh=True,
         package_override=None):
    """models_dir 例: <project>/Assets/Models

    rig_names        : 対象リグの package 名リスト。None なら全リグ。
    clip_filter      : クリップ名 (接頭辞除去後) のリスト。None なら全クリップ。
    package_override : 出力フォルダ名 / メッシュ FBX 名を差し替える。
                       例: MiniBot リグを GreenWare 側の Player パッケージへ出す。
    """
    scene = bpy.context.scene
    saved_frame = scene.frame_current
    report = []

    for rig in RIGS:
        if rig_names and rig["package"] not in rig_names:
            continue
        if rig["armature"] not in bpy.data.objects:
            continue

        armature = bpy.data.objects[rig["armature"]]
        package_name = package_override or rig["package"]
        output_dir = os.path.join(models_dir, package_name)
        os.makedirs(output_dir, exist_ok=True)

        root_motion_empty = None
        if rig["root_motion"]:
            root_motion_empty = rm.ensure_root_motion_empty(armature)

        if export_mesh:
            report.append({"rig": rig["package"], "mesh":
                           export_mesh_package(rig, output_dir, package_name)})

        for action in actions_for_rig(rig):
            clip = clip_name_for(rig, action)
            if clip_filter and clip not in clip_filter:
                continue

            entry = {"rig": rig["package"], "clip": clip, "action": action.name}
            if rig["root_motion"]:
                mode = "extract" if clip in ROOT_MOTION_EXTRACT_CLIPS else "zero"
                entry["root_motion"] = rm.build_root_motion(
                    scene, armature, root_motion_empty, action,
                    mode=mode, root_bone_name=rig["root_bone"])
            # クリップ毎に FBZZ_EVENT__* ノードのアクションを差し替える。
            # 対応する EV_ アクションが無いノードは静止したまま書き出され、
            # 取り込み側では「値が変化しない = イベント 0 件」になるので無害。
            entry["events"] = ev.bind_events_for_action(armature, action)
            entry["fbx"] = export_clip(rig, action, output_dir, root_motion_empty)
            report.append(entry)

    scene.frame_set(saved_frame)
    return report


if __name__ == "__main__":
    argv = sys.argv
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    target = args[0] if args else os.path.join(os.getcwd(), "Assets", "Models")
    for line in main(target):
        print(line)
