# MiniBot アニメーション エクスポート規約

対象: `Player_Model/MiniBot.blend` / `WPN_Pistol_L.blend` / `WPN_Pistol_R.blend`
関連: `Docs/conventions/blender-export.md` (共通のFBX設定・軸補正・Root Motion)
     `Docs/design/animation-system-v3.md` (レイヤー / BlendTree / Additive の設計)

このドキュメントは **クリップ単位のエクスポート方式** と **Animator の推奨構成** を定める。
FBX の共通設定 (`axis_forward` / `bake_space_transform` / `add_leaf_bones` 等) は
`blender-export.md` が正であり、ここでは差分だけを扱う。

---

## 0. 前提となるリグ構成

| 項目 | 値 |
|---|---|
| アーマチュア | `MiniBot_Armature` (58 ボーン) |
| うち指ボーン | 24 (`F1A_L` … `ThC_R`) |
| うち下半身 | 10 (`Root` `Pelvis` `Thigh/Shin/Foot/Toe_L/R`) |
| ソケット | `SOCKET_Hand_L/R` `SOCKET_Holster_L/R` `SOCKET_Shoulder_L/R` `SOCKET_Back` |
| キャラ前方 | `-Y` / 右手側 `-X` / 上 `+Z` |
| 拘束 | 43 ボーンに `LIMIT_ROTATION` (LOCAL space) |

武器は別アセット (`WPN_Pistol_L` / `_R`) として `SOCKET_Hand_L/R` へ実行時アタッチする。
プレイヤークリップに銃のボーンは一切含まれない。

### LIMIT_ROTATION の主な可動域

| ボーン | X (ピッチ) | Y (ヨー/ツイスト) | Z (ロール) |
|---|---|---|---|
| `Chest` | ±17° | ±35° | ±6° |
| `Head` | ±35° | ±60° | ±45° |
| `UpperArm_L/R` | ±90° | ±70° | -95°〜70° |
| `Forearm_L/R` | -5°〜135° | ±80° | ±10° |
| `Hand_L/R` | ±45° | ±30° | ±60° |

**この拘束があるため `bake_anim=True` は必須。** false にすると生の fcurve 値が出力され、
拘束で丸められる前の値がエンジンへ渡って過回転する。

---

## 1. ⚠️ 最大のリスク: `bake_anim_use_all_bones=True`

`Tools/BlenderExport/fbzz_export_minibot.py:140` の現行値:

```python
bake_anim_use_all_bones=True,
```

この設定は **アクションがキーを持たないボーンにもキーを強制生成する**。
MiniBot のクリップは全て部分ボーンしかキーしていないため、影響は全クリップに及ぶ。

| クリップ | キー済 | **未キー** |
|---|---:|---:|
| `MiniBot_Fire_Pistol_R` | 4 | **54** |
| `MiniBot_Hit_F` | 13 | **45** |
| `MiniBot_Aim_RU` | 13 | **45** |
| `MiniBot_Idle` | 23 | **35** |
| `MiniBot_Draw_Pistols` | 42 | **16** |

未キーのボーンにはビューポート上の**残留ポーズ**（直前に再生していたアクションの値）が
そのまま焼き込まれる。結果として:

- `Hit_F` が「上半身13ボーンの被弾リアクション」ではなく
  「脚と指まで含む全身58ボーンの絶対クリップ」になる
- エンジンの `AvatarMask` は**トラックの有無ではなくボーン単位のウェイト**で効くので、
  マスクを掛けても値そのものは存在してしまう。ウェイト0で潰すしかない
- Additive レイヤーでは `sampled - reference` が指や脚にも発生し、
  ベースの走りモーションに残留ポーズの差分が乗る

Blender で見えている「変なポーズ」は**リグの不具合ではなくこの残留表示**であり、
`use_all_bones=True` はそれを**そのままアセットへ焼き付ける**。

### エンジン側は未キーを正しく扱える

```cpp
// AnimatorSystem.cpp AccumulateLayerClips()
for (const auto& track : weighted.clip->tracks) { ... }
```

評価はクリップが持つトラックのみを走査する。トラックが無いボーンには一切書き込まないため、
ベースレイヤーの姿勢が保たれる。

### ⚠️ `use_all_bones=False` では解決しない (実測 2026-08)

「部分クリップは部分のまま出す」が理想だが、**Blender はこの設定を無視する**。

| simplify_factor | use_all_bones | AnimCurveNode (Hit_F) | サイズ |
|---:|---|---:|---:|
| **0.0** | False | **180** | 398KB |
| 0.0 | True | 180 | 398KB |
| 0.001 | False | 74 | 149KB |
| 0.01 | False | 31 | 101KB |
| 0.1 | False | 9 | 74KB |

原因は `io_scene_fbx/export_fbx_bin.py` の `AnimationCurveNodeWrapper.simplify()` が
`simplify_factor == 0.0` で即 return すること。定数カーブの間引きはこの関数の中で
行われるため、**`simplify_factor=0.0` のもとでは `use_all_bones` の True/False は同じ結果**になる。

`simplify_factor` を上げれば減るが、0.01 で既に本物のカーブ (39 本必要) を割り込んでおり、
「キー削減はエンジンの `OptimizeVectorKeys` に任せる」という既存方針とも衝突する。

## 2. クリップ台帳

### ベースレイヤーで全身の姿勢を決めるクリップ

| クリップ | frames | キー済 | 備考 |
|---|---|---:|---|
| `Idle` | 1–61 | 23 | loop |
| `Walk` | 1–33 | 23 | loop |
| `Run` | 1–25 | 23 | loop |
| `JumpUp` | 1–16 | 23 | |
| `FallIdle` | 1–24 | 23 | loop |
| `Land` | 1–20 | 23 | |
| `Dodge_Roll` | 1–20 | 23 | |
| `Hit_Heavy` | 1–24 | 23 | Root/脚をキー。全身リアクション |
| `Aim_Idle` | 1–41 | 25 | loop / 構え姿勢の土台 |
| `Draw` | 1–60 | 18 | Root なし |
| `Draw_Pistols` | 1–26 | 42 | 指込み |
| `Holster_Pistols` | 1–26 | 42 | 指込み |

### 部分クリップ (上半身レイヤー / Additive / BlendTree)

| クリップ | frames | キー済 | 用途 |
|---|---|---:|---|
| `Hit_F` / `Hit_B` / `Hit_L` / `Hit_R` | 1–10 | 13 | Additive (ref = `Idle` @0) |
| `Fire_Pistol_L` / `_R` / `_Dual` | 1–8 / 12 | 4 | Additive (ref = `Pose_FireZero` @0) |
| `Fire_Dry` | 1–6 | 4 | 同上 |
| `Aim_LU` … `Aim_RD` (9枚) | 1–2 | 13 | BlendTree2D (ref = `Aim_CC` @0) |
| `Pose_AimUpper_Idle` / `_Walk` / `_Run` | 1–2 | 13 | Additive (ref = `Pose_AimUpper` @0) |

### 基準ポーズ

再生はせず `AdditiveReferencePose.sourcePath` から参照される。

| クリップ | キー済 | 対になる加算クリップ |
|---|---:|---|
| `Pose_FireZero` | 4 | `Fire_*` (フレーム1が完全一致 0.00°) |
| `Pose_AimUpper` | 13 | `Pose_AimUpper_*` |
| `Aim_CC` | 13 | `Aim_*` 9枚 (自身が中心セル) |
| `Pose_GripPistols` | 26 | エクスポート時の指ベースポーズ |
| `Idle` (先頭フレーム) | 23 | `Hit_F/B/L/R` (フレーム1が完全一致 0.0°) |

### 武器クリップ (`WPN_Pistol_L` / `WPN_Pistol_R`)

リグは 15 ボーン。各クリップは 3〜5 ボーンしかキーしていない。
`Deploy` / `Draw` / `Dry` / `Fire_L`(または`Fire_R`) / `Fold` / `Folded_Idle` / `Idle` / `Reload` の 8 本。
リグは原点・無回転・拘束なしで独立しており、`SOCKET_Grip` を `SOCKET_Hand_L/R` へ合わせてアタッチする。

## 3. 解法: 基準ポーズを全クリップへ流し込む

トラックを減らすのではなく、**焼かれる中身を決定論にする**。

書き出し前に毎回

1. 全ポーズボーンの basis を rest へリセット
2. `base_poses` を順に流し込む
3. 本命のアクションを割り当てて export

とすると、未キーのボーンは **全クリップで同一の既知ポーズ**になる。
アクションはキーを持つチャンネルしか書き換えないため、3 の割り当て後も 2 の値が残る。

### なぜこれで足りるのか

- **Additive レイヤー**: `ResolveAdditiveReferencePose()` は基準クリップに一致トラックが
  無ければ「加算クリップ自身の先頭キー」へフォールバックする。未キーのボーンは
  クリップ全体で定数なので `sampled - reference` が厳密に 0 になり、ベース姿勢を汚さない。
  基準クリップ側に一致トラックがある場合も、両者が同じ基準ポーズで焼かれていれば同じく 0。
- **Override レイヤー**: 入る値が「偶然の残留ポーズ」ではなく意図した姿勢になる。
  マスク外のボーンは weight 0 で潰れるため実害も無い。

### MiniBot の基準ポーズ

```python
"base_poses": ["MiniBot_Idle", "MiniBot_Pose_GripPistols"],
```

`MiniBot_Idle` が 23 ボーン、`MiniBot_Pose_GripPistols` が指 24 ボーンを埋める。
残り 11 本 (`SOCKET_*` / `Core` / `Vent_*_Rotor` / `Grip_L/R` / `Mount_Back`) は rest のまま。

武器リグは `base_poses = None` で `<prefix>Idle` (= `Pistol_Idle`) を自動解決する。

### 検証結果

全 35 クリップについて「未キーのボーンの basis が基準ポーズと一致するか」を確認済み。
**NG 0 件** (最大誤差 < 0.01°)。

## 4. エクスポートスクリプトへの変更


`fbzz_export_minibot.py` に以下を追加済み。

```python
def resolve_base_poses(rig):
    """このリグの「未キーのボーンを固定する」基準アクション列を返す。"""
    names = rig.get("base_poses") or [rig["prefix"] + "Idle"]
    return [a for a in (bpy.data.actions.get(n) for n in names) if a is not None]


def reset_pose_to_rest(armature):
    """全ポーズボーンの basis を恒等へ戻す (オペレータ / モード切替なし)。"""
    for pb in armature.pose.bones:
        pb.location = (0.0, 0.0, 0.0)
        pb.scale = (1.0, 1.0, 1.0)
        if pb.rotation_mode == "QUATERNION":
            pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
        elif pb.rotation_mode == "AXIS_ANGLE":
            pb.rotation_axis_angle = (0.0, 0.0, 1.0, 0.0)
        else:
            pb.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.view_layer.update()


def apply_base_poses(armature, actions):
    reset_pose_to_rest(armature)
    for action in actions:
        armature.animation_data_create()
        armature.animation_data.action = action
        rm._assign_first_slot(armature)
        bpy.context.scene.frame_set(int(round(action.frame_range[0])))
        bpy.context.view_layer.update()


def export_clip(rig, action, output_dir, root_motion_empty):
    ...
    # 未キーのボーンを基準ポーズで埋めてから書き出す。全クリップで実施する。
    apply_base_poses(armature, [a for a in resolve_base_poses(rig) if a is not action])

    armature.animation_data.action = action
    ...
```

`ANIM_BAKE_OPTIONS` は元のまま (`use_all_bones=True` / `force_startend_keying=True` /
`simplify_factor=0.0`)。切り替えても効果が無いことが実測で分かったため、
「効かない設定をいじる」のではなく「基準ポーズで決定論にする」で解いている。

## 5. Animator 推奨構成

```
Base Layer                    Override / マスクなし
  Idle / Walk / Run  (BlendTree1D "Speed")
  JumpUp / FallIdle / Land / Dodge_Roll
  Hit_Heavy / Draw_Pistols / Holster_Pistols

Layer "UpperBody"             Override / mask = M_UpperBody
  Aim_Idle                    ← 銃を構えた腕の形をここで作る
  Draw

Layer "Aim"                   Additive / ref = MiniBot_Aim_CC @ 0
                              mask = M_UpperBody
  State "AimOffset"           BlendTree2D (FreeformCartesian)

Layer "Add_AimSway"           Additive / ref = MiniBot_Pose_AimUpper @ 0
                              mask = M_UpperBody
  Pose_AimUpper_Idle / _Walk / _Run  (BlendTree1D "Speed")

Layer "Add_Hit"               Additive / ref = MiniBot_Idle @ 0
                              mask = M_UpperBody (blendDepth 2)
  Slot 再生 ← Hit_F / _B / _L / _R

Layer "Add_Fire"              Additive / ref = MiniBot_Pose_FireZero @ 0
                              mask = M_Arms
  Slot 再生 ← Fire_Pistol_L / _R / _Dual / Fire_Dry
```

### レイヤーを分ける理由

`AdditiveReferencePose` は **`AnimationLayer` に1つ**しか持てない。

```cpp
struct AnimationLayer {
    AdditiveReferencePose additiveReference;   // ← レイヤー単位
```

基準ポーズが異なる加算クリップ (`Aim_*` / `Hit_*` / `Fire_*`) を同一レイヤーへ入れると、
`ResolveAdditiveReferencePose()` が全クリップに同じ基準を適用するため片方が必ず破綻する。

### 必要な .mask

| 名前 | ルート | blendDepth | 用途 |
|---|---|---:|---|
| `M_UpperBody` | `Chest` | 2 | 胴〜頭〜腕。Chest の継ぎ目を2階層で立ち上げる |
| `M_Arms` | `UpperArm_L` / `UpperArm_R` | 1 | 発砲リコイル用 |

---

## 6. Aim BlendTree2D の設定値

9枚のポーズは `MiniBot_Pose_AimUpper` を中心セルとして生成済み。
中心セル `MiniBot_Aim_CC` は `Pose_AimUpper` と同一なので、
これを `additiveReference` にすると中心で差分が厳密に 0 になる。

```
State "AimOffset"
  mode   = BlendTree2D
  type   = FreeformCartesian        ← 3x3 グリッドはこちら。SimpleDirectional は不可
  paramX = "AimYaw"                 -1 = 左 / +1 = 右
  paramY = "AimPitch"               -1 = 下 / +1 = 上
```

| clipName | posX | posY | 実測ヨー | 実測ピッチ |
|---|---:|---:|---:|---:|
| `MiniBot_Aim_LU` | -1 | +1 | +37.4° | +34.3° |
| `MiniBot_Aim_CU` |  0 | +1 |  -1.2° | +39.1° |
| `MiniBot_Aim_RU` | +1 | +1 | -48.5° | +32.7° |
| `MiniBot_Aim_LC` | -1 |  0 | +45.0° |  -0.1° |
| `MiniBot_Aim_CC` |  0 |  0 |   0.0° |   0.0° |
| `MiniBot_Aim_RC` | +1 |  0 | -44.9° |  -0.2° |
| `MiniBot_Aim_LD` | -1 | -1 | +48.5° | -33.6° |
| `MiniBot_Aim_CD` |  0 | -1 |  -4.9° | -34.0° |
| `MiniBot_Aim_RD` | +1 | -1 | -55.8° | -28.4° |

実測値は `UpperArm_R` 起点 → `Hand_R` 先端のベクトルを `Aim_CC` 基準で差分化したもの
(ヨー `+` = キャラの左 / ピッチ `+` = 上)。角の4セルは球面合成のぶん数度ずれる。

**エイムコーン: ヨー ±45° / ピッチ +39°〜-34°**

### ポーズの作り方 (再生成する場合)

回転はワールド軸で `Chest → Head → UpperArm_L/R` の順に配分し、
銃は `Chest` の子孫なので `Chest + UpperArm` の合計が 100% になるようにする。

| ボーン | ヨー配分 | ピッチ配分 (上) | ピッチ配分 (下) |
|---|---:|---:|---:|
| `Chest` | 30° | +15° | -15° |
| `Head` | +15° | +25° | -20° |
| `UpperArm_L/R` | +15° | +25° | -20° |

`Chest` のピッチ配分が 15° なのは `LIMIT_ROTATION` の X ±17° に収めるため。
残りは肩 (`UpperArm` X ±90°) が負担する。**basis 値そのものを可動域内へ丸めてから**
キーを打つこと。範囲外の basis を残すと「生の fcurve 値」と「拘束適用後の見た目」が
食い違い、`bake_anim` の設定次第で結果が変わる。

### スクリプト側の使い方

```cpp
// 敵方向をキャラのローカル角へ変換して -1..1 へ正規化する
const float yaw   = SignedAngleY(forward, toEnemy);   // degrees
const float pitch = ElevationAngle(toEnemy);          // degrees
animator.SetFloat("AimYaw",   std::clamp(yaw   / 45.0f, -1.0f, 1.0f));
animator.SetFloat("AimPitch", std::clamp(pitch / (pitch > 0.0f ? 39.0f : 34.0f), -1.0f, 1.0f));
```

コーンを超えるヨーは `CharacterControllerComponent` の旋回で吸収する。

---

## 7. 検証手順

1. **未キーボーンの一致**: 各クリップを割り当てた状態で、そのクリップがキーしていない
   ボーンの `matrix_basis` が基準ポーズと一致すること (誤差 < 0.01°)。
   全 FBX は 58 ボーン x 3 = 180 AnimCurveNode を持つのが正常
2. **中心セルのゼロ確認**: `AimYaw = AimPitch = 0` でポーズが `Aim_Idle` と一致すること。
   ずれる場合は `additiveReference` が `MiniBot_Aim_CC @ 0` になっていない
3. **拘束のベイク**: `Chest` のピッチが FBX 上で ±17° を超えていないこと
4. **指の固定**: `Idle` 再生中に指が `Pose_GripPistols` の形を保つこと。
   崩れる場合は種別 A の前処理 (`apply_base_pose`) が抜けている
5. **加算の混線**: `Hit_F` を Slot 再生しながら `Fire_Pistol_R` を撃ち、
   脚と指が動かないこと

---

## 8. 出力先

```
GreenWare/Assets/Models/
├── Player/                     ← MiniBot リグ (package_override="Player")
│   ├── Player.fbx              スキンメッシュ本体
│   └── <35 クリップ>.fbx
├── WPN_Pistol_L/
│   ├── WPN_Pistol_L.fbx
│   └── <8 クリップ>.fbx
└── WPN_Pistol_R/
    ├── WPN_Pistol_R.fbx
    └── <8 クリップ>.fbx
```

書き出しコマンド (Blender の Scripting タブ):

```python
import sys; sys.path.append(r"<repo>/Tools/BlenderExport")
import fbzz_export_minibot as fx, importlib; importlib.reload(fx)

# MiniBot.blend を開いた状態で
fx.main(r"<repo>/GreenWare/Assets/Models", rig_names=["MiniBot"],
        package_override="Player")

# WPN_Pistol_R.blend / WPN_Pistol_L.blend をそれぞれ開いて
fx.main(r"<repo>/GreenWare/Assets/Models", rig_names=["WPN_Pistol_R"])
fx.main(r"<repo>/GreenWare/Assets/Models", rig_names=["WPN_Pistol_L"])
```

3 ファイルに分割したため、1 回の実行では 1 リグしか書き出せない
(`main()` は `bpy.data.objects` にアーマチュアが無いリグをスキップする)。

## 9. 廃止済み

| 対象 | 理由 |
|---|---|
| `MiniBot_Reload_Pistol` | 削除済み。フレーム1が `Idle` から 105.8° 離れており加算不可だった |
| `MiniBot_Reload_Pistols_Dual` | 削除済み |
| `Pistol_Reload` (武器側) | 上記に対応する武器モーション。要否を確認のこと |
| `AimAt` IK によるエイム | 9ポーズ BlendTree2D 方式へ変更。IK は絵作りを制御できないため見送り |
