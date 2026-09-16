# MiniBot C — 完成版の書き出し仕様とFBZZへの受け渡し

- 更新: 2026-09-14
- 状態: **Playerとして31本のFBXを出力・再読み込み検証済み。新Controllerと3マスクを構築済み。FBZZでのインポート・再生検証は未実施。**
- 制作の正本: [MiniBotC_PlayerMotions.blend](../../GreenWare/Assets/_src/MiniBotC/MiniBotC_PlayerMotions.blend)
- 機械可読の一覧: [ExportManifest.json](../../GreenWare/Assets/Models/Player/ExportManifest.json)
- クリップ表: [Clips.csv](../../GreenWare/Assets/Models/Player/Clips.csv)
- 設定と事前検証: [prepare_minibot_c_export.py](../../Tools/BlenderExport/prepare_minibot_c_export.py)
- 実出力: [export_minibot_c_player.py](../../Tools/BlenderExport/export_minibot_c_player.py)
- Controller・シーン撤去: [設計](player-export-controller.md)、実行記録は `Docs/Art/MiniBotC/ExportDelivery/DeliveryReport.json` (追跡外)

## 1. 出力するデータ

| 対象 | LOD0 | LOD1 | LOD2 | 出力方法 |
|---|---:|---:|---:|---|
| Player（体＋補修した指） | 60,428三角形 | 30,051 | 13,040 | 各LODに同じ54骨。アニメーションなしのバインド姿勢 |
| 剣 | 5,994 | 2,994 | 2,194 | 静的メッシュ。握り位置を原点とする |
| おとも | 19,999 | 9,999 | 3,999 | 各LODに同じ4骨。移動・浮遊はゲーム側で制御 |
| Playerモーション | 22クリップ | 共通 | 共通 | 1 Action = 1 FBX = 1テイク。メッシュを同梱しない |

合計はモデルFBX **9本**、モーションFBX **22本**、実使用テクスチャ **19画像**。
Playerの指は別メッシュなので、`Player_LOD0` だけを選択して書き出すと指が欠ける。
必ず同じLODの `Player_Fingers_LOD*` を含める。LODの選択と切り替えはFBZZ側で設定する。

| 分類 | 出力するクリップ |
|---|---|
| 移動 | Idle / SwordWalk / Run_F / RunStop / Dodge |
| 通常ジャンプ | JumpStart / FallLoop / Land |
| 攻撃 | Slash01 / HighSpinAttack / JumpAttack / SlideAttack / UnderSlash / UnderSlashandUpperSlash |
| 防御 | ToBlocking / Blocking / GuardHit / BlockingToIdle |
| 被弾・死亡 | Hit / Death |
| リザルト | Victory / DefeatIdle |

ループは **Idle / SwordWalk / Run_F / FallLoop / Blocking / Victory / DefeatIdle** の7本。
それ以外は単発。実フレーム範囲・秒数はManifestとCSVを正本にする。

`MB_C_Stance` は基準姿勢として制作側に保持し、ゲーム用クリップには含めない。
`REF_*`、`REFERENCE_*`、`DEMO_*`、`Player_NewAction`、操作用Rigify、メタリグ、ウィジェット、
確認床、カメラ、ライト、過去の制作ファイルは出力対象外。
通常歩き・パリィ・登攀は追加しない。

## 2. ファイル配置

出力先は `Models/Player`。旧フォルダは `.meta` ごと `Models/Player_BackUp` へ移動済み。
旧 `Animation/Player` も、そのバックアップ内の `Animation` にGUIDを保ったまま退避した。

```text
GreenWare/Assets/
  _src/MiniBotC/
    README.md
    MiniBotC_PlayerMotions.blend       # 唯一の完成モーション正本
    Archive/                          # 工程ごとのバックアップ
  Models/
    Player/
      Player.fbx                      # LOD0
      Player_LOD1.fbx
      Player_LOD2.fbx
      Animations/
        Idle.fbx
        SwordWalk.fbx
        ...                           # Manifestに列挙した22本だけ
      Sword/
        Sword.fbx                     # LOD0
        Sword_LOD1.fbx
        Sword_LOD2.fbx
      Companion/
        Companion.fbx                 # LOD0
        Companion_LOD1.fbx
        Companion_LOD2.fbx
      Textures/                       # 実使用19画像、相対参照
      ExportManifest.json
      Clips.csv
      README.md
    Player_BackUp/                    # 旧モデル130ファイル＋フォルダ.meta
      Animation/                      # 旧Controller・maskの9ファイル
  Animation/Player/
    Player.animcontroller
    Player_Base.mask
    M_UpperBody.mask
    M_Arms.mask
```

既存テクスチャはManifestに元パス・用途・解像度・Blender内への梱包有無を記録した。
完成版は移送できるパッケージとして19画像を共有Texturesフォルダへ収集し、`path_mode=RELATIVE` で書き出した。
LODごとの重複コピーは作らず、制作元の画像・GUIDは維持する。最終配置先で画像参照も再検証した。

Stage_01〜03・Resultの旧Player階層と装着武器は削除済み。変更前の完全なシーンは
`Docs/Art/MiniBotC/ExportDelivery/SceneBackups` (追跡外) に保存した。新Playerのシーン配置はまだ行っていない。

## 3. 座標系と単位

| 項目 | 制作側 | FBX / FBZZ側 |
|---|---|---|
| 単位 | Metric、1 Blender単位 = 1 m | `global_scale=1`、インポート倍率1 |
| 上 | +Z | +Y |
| このPlayerの正面 | −Y | 標準FBX設定で取り込むと **−Z** |
| ゲームの前方 | — | `Vector3::FORWARD` は **+Z** |
| サンプリング | 30 fpsのタイムライン | 推奨0.25フレーム間隔 = 120サンプル/秒。尺は変えない |
| Root | 原点固定 | Root Motionを無効。移動・ジャンプ軌道はゲーム制御 |

Blenderの実際の `axis_conversion(to_forward='-Z', to_up='Y')` と、
インポーターの左手系変換を合わせた点の対応は **`(x,y,z) → (x,z,y)`**。
原点や軸を手で先回りして回したり、100倍・0.01倍を別途掛けたりしない。

ゲーム上の +Z と正面を合わせるときは、移動・当たり判定を持つActorの下に表示用の `Visual` を置き、
**VisualをY軸に180度** 回す。Player、剣、ソケットは同じVisualの配下に置く。
この補正込みの点の対応は **`(x,y,z) → (−x,z,−y)`**。
アニメーションFBXだけを180度回す、スキンメッシュだけを回す、ソケットへ同じ補正を二度掛ける、といった運用にしない。

参照: [Vector3.hpp](../../Projects/Math/include/Math/Vector3.hpp)、
[FbxImportTool.cpp](../../Projects/Editor/src/Import/FbxImportTool.cpp)。
変換行列・現行実装・完成FBXの軸メタデータを照合済み。FBZZの実機表示は未検証。

## 4. 固定するFBX設定

設定値は `prepare_minibot_c_export.py` の `FBX_SETTINGS` と `ANIMATION_SETTINGS` にまとめてある。

| 設定 | 値 |
|---|---|
| `axis_forward` / `axis_up` | `-Z` / `Y` |
| `apply_unit_scale` / `global_scale` | `True` / `1.0` |
| `apply_scale_options` | **`FBX_SCALE_NONE`** |
| `use_space_transform` / `bake_space_transform` | `True` / `False` |
| `add_leaf_bones` | `False` |
| `use_armature_deform_only` | `False`（Rootと武器ソケットも必要） |
| `primary_bone_axis` / `secondary_bone_axis` | `Y` / `X` |
| `armature_nodetype` | `NULL` |
| `use_selection` | `True`。Manifestの明示リストで選ぶ |
| `bake_anim_use_all_actions` / `bake_anim_use_nla_strips` | `False` / `False` |
| `bake_anim_use_all_bones` / `bake_anim_force_startend_keying` | `True` / `True` |
| `bake_anim_step` / `bake_anim_simplify_factor` | **`0.25` / `0.0`** |
| テクスチャ | 外部画像、`embed_textures=False` |

`FBX_SCALE_NONE` を採用する理由は現在のインポーターの処理順にある。
`NormalizeBlenderRootTransforms` はノードのscale100を除去して `unitScale` へ移し、
同時に `bindBakeRotation` を設定する。`ModelSubExporter` はその回転を頂点へ焼き、
逆バインド行列にも逆回転を反映するため、Animatorなしの静止表示もY-upになる。

`FBX_SCALE_ALL` ではノードscaleが1になり、上記関数が早期returnするため、
**現行コードでは `bindBakeRotation` の設定を通らない**。
旧DCC設計の推奨値や制作開始時の `export_animation_ready_clip.py` を、そのまま最終版の設定にしない。
旧 `fbzz_export_minibot.py` の「回転も除去する」「派生物をFBXの隣へ置く」という説明も現在の実装とは異なる。
この仕様では回転を維持する現行実装と `FBX_SCALE_NONE` を基準にする。

0.25フレーム間隔は、ローリング中の剣先接地や細かく修正した腕の軌道を保持するため。
30fpsで時間を定義しつつ細かく評価し、キー削減はFBZZの位置0.0001m・回転0.05度の最適化へ任せる。
実際のFBX出力後にはキー間の接地も再検査する。

## 5. 評価・ベイクする順序

完成した `.blend` を開いて、**出力用の複製**だけで以下を実行する。
制作リグへ変換適用やリセットを掛けない。

1. `MB_C_Stance` / Idleを基準に未キーのチャンネルを初期化し、対象Actionと
   **`OBMiniBotC_ControlRig` スロットを明示的に割り当てる**。NLAを混ぜない。
2. 制作リグを評価し、`MiniBotC_Humanoid` の54骨の最終行列を取得する。
   IK・両手保持・指・制約・ドライバーは評価結果へ含める。
3. 制約を取り除いた出力用54骨へ、親の評価行列とバインド行列を使ってローカルTRSへ変換し、
   0.25フレーム間隔でキーを記録する。Quaternionの符号を隣接キーで連続にする。
4. モデル3LODと全22クリップで、出力アーマチュア名・骨名・親子・バインド姿勢を統一する。
   `EXPORT_Humanoid.001` のような一時名がファイル間で変わらないようにする。
5. アニメーションFBXは骨格のみを選択し、出力中だけシーン名をManifestの `take` へ設定する。
   各ファイルのテイク数を1本にする。モデルFBXは `bake_anim=False` とする。
6. 終了時は一時リグ・一時Action・一時メッシュを片付け、元のAction・スロット・フレーム・選択を戻す。

親子ローカルへの変換は [export_animation_ready_clip.py](../../Tools/BlenderExport/export_animation_ready_clip.py) の
`convert_local_to_pose(..., invert=True)` が参考になる。
1クリップ1FBXと基準姿勢の初期化は [fbzz_export_minibot.py](../../Tools/BlenderExport/fbzz_export_minibot.py) を参考にする。
ただしどちらも **完成版の剣ソケット処理と下記の取り込み設定を追加してから** 出力に使用する。
`prepare_minibot_c_export.py` は一覧生成・検証・整理専用で、これらのFBXベイク処理は実行しない。

### 剣と敗北の取り扱い

剣は1本の独立メッシュで、Playerに結合しない。
`Sword_Control` の見た目は、握り位置の補正と、敗北時の `Defeat_SwordGround` 制約で決まる。
`sword_release` をカスタムプロパティのままFBXへ渡しても、FBZZが同じ制約を実行するわけではない。

受け渡し方式は、**出力用 `Socket_Weapon_R` に `Sword_Control` の評価済み姿勢をベイクする**方式とする。
このソケットへウェイトが付いた頂点は全Player LODで0個と検証済みなので、体の変形には影響しない。
出力用ソケットの基準姿勢も握り補正済みの姿勢へ合わせ、モデル・モーションで同じものを使う。
通常クリップでは掌に保持し、DefeatIdleではPlayer原点に対して床に残る姿勢を焼く。
RightHandの子のままでも、親の逆行列でローカル姿勢へ変換すれば再現できる。骨の総数は54を維持する。

剣モデルには原点・単位スケールの `Attach_Grip` ノードを出力用に設ける。
FBZZでは [AttachToSocket](../../Projects/Engine/src/Scene/SocketAttach.cpp) を使い、
モデルのインポート時の軸変換も含めたアタッチ相対姿勢を打ち消す。
取り込み後に **剣のアタッチ点と出力ソケットのワールド姿勢が一致すること** を確認する。
生のBlender行列を無変換でゲームのオフセットへ貼り付けない。

DefeatIdleは剣を落とす導入を含まず、床に置いた後の4秒ループ。
物理的な落下や武器の所有状態変更が必要になった場合はゲーム側の別処理とする。

### おともの原点

現在の表示位置 `(-0.85, 0, 1.1)` はPlayerの横へ置くための制作配置。
出力用のリグ・全メッシュを同じ基準へ移し、リグのオブジェクト変換を恒等にして出す。
メッシュだけを原点へ動かすと逆バインド行列とずれるため、4骨と全LODを一体のローカル空間として扱う。
待機や浮遊のActionは出力せず、ゲームがこのローカル原点を動かす。

## 6. 画像と材質

実際に使っているのはBaseColor **10画像**とNormal **9画像**。材質はLOD・指を含め12種類。
Player本体のBaseColorは全LODで共有し、法線はLODごとに別画像。
補修指はBaseColorのみ。現在の材質はMetallic=0、Roughness=0.5で、発光画像は接続されていない。
緑の色を保つために、存在しないORMやEmission画像を出力対象に加えない。

- BaseColor: sRGB。既存のTripo画像と補修指画像を使用する。
- Normal: Non-Color。Blenderで使うOpenGL規約として取り込む。
- FBZZの `normal_map_convention = "OpenGL"` を設定し、エンジンの法線取り込み経路で緑成分を変換する。
  画像側を事前反転してさらに取り込み設定でも反転する運用にしない。
- インポート後の `.mat` と `.tex` を見て、実際のスロット参照と反転処理の経路を照合する。
  任意のBlenderノードグラフがそのまま移植されるとは扱わない。

## 7. FBZZ Editorへの取り込み

1. 完成FBXと新規GUID付き `.meta` は配置済み。再出力では同じパスと `.meta` を保ち、GUIDを作り直さない。
2. FBXの取り込み設定は `source_dcc=Blender`、倍率1、法線規約OpenGLに設定済み。
   ノーマル・タンジェントを有効にし、上方向に追加の手動変換を重ねない。
3. `.fbx.meta` のクリップ名・ループは設定済み。FBZZ Editorで31本のFBXをインポートする。
   ループの7本は `loop=true`、他はfalse。開始0・終了−1で全区間を使う。
   Blenderの1始まりのフレーム番号を、そのまま取り込みの切り出し値に転記しない。
4. ループ末尾の複製キーもFBXには含める。例: Idleは1〜145で4.8秒。
   Nパネルの1〜144はプレビューで二重再生を避ける範囲であり、出力の終了値ではない。
5. 取り込み先は現行コードでは **`GreenWare/Library/Baked/<fbx-guid>/`**。
   `.fzasset` / `.mesh` / `.skel` / `anims/<take>.anim` はEditorが生成する。
   古い `Foo/Foo.fzasset` や `Idle@Idle.anim` という手順は使わない。
6. 新Controllerは生成される `anims/<take>.anim` を、原本FBXのGUIDから導出したGUIDで参照済み。
   原本FBXをAnimatorのsourcePathへ直接渡すと、現行のモデルキャッシュ経由ではクリップが読み出せない。
7. 新Playerを配置し、Animator、LOD、マテリアル、武器アタッチを設定する。参照はGUIDで持つ。
   Root Motionは無効。ゲームの回避0.36秒と素材のDodge 1.4秒の調整は接続時に判断する。

`.fzhint` はDCC設計書には登場するが、現行 `FbxImportTool::Import` の `sourceHint` 引数は未使用。
それだけで座標設定やループが伝わるとは扱わず、**`.fbx.meta` が取り込み設定の正本**。
参照: [FbxMetaSerializer.cpp](../../Projects/Editor/src/Import/FbxMetaSerializer.cpp)、
[AnimSubExporter.cpp](../../Projects/Editor/src/Import/AnimSubExporter.cpp)。

## 8. 検証済みの範囲と出力後の確認

検証済み:

- 9モデルのUV、最大4ウェイト、重みの正規化、骨名の対応。
- 54骨、22クリップ、全クリップのスロット・実フレーム範囲。
- 各クリップの先頭・中間・末尾の計66姿勢で有限値、Root固定、骨スケールを確認。
- 使用画像19枚の実体または梱包データ。全23公式ActionのFCurveが整理前後で不変。
- 31本のFBXを独立したBlenderへ再読み込み。骨格・バインド姿勢・メッシュ・UV・4ウェイト・材質画像を照合。
- 全22クリップを0.25フレーム間隔で比較。最大の骨位置誤差は約0.0000022m。武器Socketの位置・向きも一致。
- 全FBXの単一テイク、54骨、Y-up、UnitScaleFactor=1、リグの軸回転−90度とスケール100を検査。
- スキンなしクリップにも共通バインド姿勢を保持するため、範囲外のフレーム0に出力用の基準キーを置く。
  FBXの実ベイク範囲は1〜末尾のままなので、尺や先頭モーションは変わらない。
- Controllerの全22クリップ参照、20ステートの到達性、3マスクの対象骨、旧Playerのバックアップ整合を検査。

実際の書き出し後に行う確認:

1. FBZZ Editorで31本のFBXをインポートし、Controllerの導出GUIDが実際の.animへ解決されることを確認する。
2. FBZZでAnimatorなしのPlayerが立ち、1.7m程度の高さになり、正面がゲームの移動方向と一致すること。
3. Idle、Run_F、Dodge、JumpAttack、DefeatIdleで足、掌、剣の握り、剣先の床接触を比較する。
   特にDodgeはキー間の剣先、DefeatIdleは剣が床に残ることを確認する。
4. 全22本の再生・単発終了・7本のループ、LOD切替、指、法線の凹凸を確認する。
5. シーン接続前後で `.meta` のGUIDが維持されていることを確認する。

ここまで通ってから「FBZZで検証済みの最終出力」とする。

## 9. 一覧と事前検証の再生成

制作ファイルを開いたBlenderのPython Consoleで実行する。
これは一覧と検証記録だけを更新し、FBXを書き出さない。

```python
import sys, importlib
sys.path.insert(0, r"C:\Users\jinhs\Downloads\FBZZ_Engine\Tools\BlenderExport")
import prepare_minibot_c_export as prep
importlib.reload(prep)
manifest = prep.main()
```

実出力後の正本一覧は `Models/Player/ExportManifest.json`。上の準備用スクリプトは過去の事前検証用一覧を再生成する。
Blender内では `MiniBotC_ExportManifest.json` と `MiniBotC_ExportGuide.md` から今回の出力情報を読める。
Nタブ「モーション」の完成モーション確認機能は継続して使う。
