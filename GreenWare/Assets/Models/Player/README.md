# Player / 刀 — FBZZ Engine 向け書き出し

Tools/BlenderExport/fbzz_export_minibot.py の規約で出力。
1 アクション = 1 FBX = 1 テイク、パッケージフォルダの隣に原本 FBX。

## コピー先

    FBX/Player/*         -> GreenWare/Assets/Models/Player/
    FBX/WPN_Sword_R/*    -> GreenWare/Assets/Models/WPN_Sword_R/
    FBX/WPN_Sword_L/*    -> GreenWare/Assets/Models/WPN_Sword_L/

FbxImportTool が Foo.fbx から Foo/ 以下へ .fzasset / .mesh / .skel / anims/*.anim を吐く。

## 内容

- Player/Player.fbx        メッシュパッケージ (LimbNode 58 / Geometry 10 / マテリアル 10)
- Player/<Clip>.fbx        50 クリップ。テイク名 = クリップ名 = ファイル名
- WPN_Sword_R/L/*.fbx      刀 (LimbNode 9、テイクなし)

クリップ名は MiniBot_ 接頭辞を落としたもの。刀のモーションは Katana_ で始まる 31 本。

## 書き出し設定 (fbzz_export_minibot の COMMON_FBX_OPTIONS / ANIM_BAKE_OPTIONS)

apply_scale_options=FBX_SCALE_NONE / axis -Z,Y / add_leaf_bones=False /
primary=Y secondary=X / armature_nodetype=NULL / bake_space_transform=False /
bake_anim_step=1.0 / simplify_factor=0.0 / use_all_bones=True / use_all_actions=False

基準ポーズ (未キーのボーンを埋める) は MiniBot_Idle -> MiniBot_Pose_GripKatana。
※ スクリプトの RIGS は MiniBot_Pose_GripPistols を指したままなので、
   本書き出しでは実行時に差し替えた。スクリプト側も更新が必要。

## Root Motion

全 50 クリップとも mode="zero" (Root_Motion ノードあり / キーは恒等)。
Root ボーンの正味変位は全クリップ 0.000 m、Yaw 0.0 度で、実移動を持つクリップは無い。

## アニメーションイベント

ノード: FBZZ_EVENT__WeaponAttach
Position X (intParam) がステップ信号。静止値は 0。

    1 = 右の刀を右手のソケットへ   (SOCKET_Katana_R)
    2 = 左の刀を左手のソケットへ   (SOCKET_Katana_L)
    3 = 右の刀を背中のソケットへ   (SOCKET_BackSword_R)
    4 = 左の刀を背中のソケットへ   (SOCKET_BackSword_L)

    Katana_Draw      f16 -> 1   f18 -> 2
    Katana_Iai       f16 -> 1   f19 -> 2   f57 -> 3   f63 -> 4
    Katana_Sheathe   f16 -> 3   f22 -> 4
    Katana_Victory   f36 -> 3   f43 -> 4

上記以外の 46 クリップは値が 0 のまま変化しない = イベント 0 件。
刀の初期状態は Katana_Draw / Katana_Iai のみ「背中」、他はすべて「両手」。

## 刀のアタッチ

刀側の基準は SOCKET_Grip。SOCKET_Tip / SOCKET_Trail_Base / SOCKET_Trail_Tip は斬撃トレイル用。

## ループするクリップ

Idle, Walk_B/F/L/R, Run_B/F/L/R, FallIdle と、その Katana_ 版、および Katana_Lose。

## モーションの内容

各クリップが何をしているかは GreenWare/Assets/Docs/player-motions.md に書いた。

## 検査

Tools/BlenderExport/fbzz_fbx_inspect.py で 53 ファイルすべて確認済み。
FBX 7400 / テイク名一致 / LimbNode 58 (< MAX_SKINNING_BONES 128) /
Root_Motion ノードと R,S,T カーブあり / イベント値は上表と一致。
