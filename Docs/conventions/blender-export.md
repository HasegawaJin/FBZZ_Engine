# Blender → FBZZ Engine エクスポート規約

対象: `Tools/BlenderExport/` のスクリプト群 (Blender 5.2 LTS / 4.4+ のスロット付きアクションで検証)。

---

## パッケージ構成

`AssetManager` は「パッケージフォルダ `Foo/` の隣に原本 `Foo.fbx`」を前提にする
(`Projects/Engine/src/Asset/AssetManager.cpp`)。Editor の `FbxImportTool` が `Foo.fbx` を読み、
`Foo/` 以下へ `.fzasset` / `.mesh` / `.skel` / `anims/*.anim` を生成する。

```
Assets/Models/MiniBot/
├── MiniBot.fbx            ← スキンメッシュ本体 (アニメーションなし)
├── MiniBot_Idle.fbx       ← 1 アクション = 1 FBX = 1 テイク
├── MiniBot_Walk.fbx
└── …
```

`.fbx` は `.gitignore` 済み (`**/Assets/**/*.fbx`)。原本は DCC 側で管理し、リポジトリには
インポート生成物のみを載せる。

## 1 アクション = 1 FBX にする理由

`AnimSubExporter` は 1 FBX 内の全 AnimStack をループして
`<FBX名>@<テイク名>.anim` を生成する。Blender の "All Actions" 一括書き出しでは

- テイク名が `MiniBot_Armature|MiniBot_Idle` になり `|` が `_` にサニタイズされて汚れる
- シーン内の全アーマチュア (MiniBot + 武器リグ 5 本) に全アクションが総当たりされる

ため、`bake_anim_use_all_actions=False` で 1 テイクずつ書き出す。
このときテイク名は Blender FBX エクスポータの仕様で **シーン名** になるので、
書き出し中だけシーン名をクリップ名へ差し替える (`fbzz_export_minibot.py::export_clip`)。

## エクスポート設定の要点

| 設定 | 値 | 理由 |
|---|---|---|
| `axis_forward` / `axis_up` | `-Z` / `Y` (Blender 既定) | `FbxImportTool` が Blender 由来の -90°X / scale100 を検出し、**scale100 だけを** `unitScale` へ移す。-90°X は Z-up→Y-up 変換そのものなのでバインド姿勢として保持される。こちらで先回りして補正してはいけない |
| `apply_scale_options` | `FBX_SCALE_NONE` | 同上。`UnitScaleFactor`(=1.0) と root スケール(=100) の組で `ReadUnitScale` 0.01 × 100 = 1.0 が成立する |
| `bake_space_transform` | `False` | `True` にするとメッシュだけ軸が焼き込まれ、アーマチュアの Null は -90°X/100 のまま取り残される。スケール 100 がアーマチュア経由でのみ `unitScale` へ移り、モデルが 100 倍になる |
| `add_leaf_bones` | `False` | `_end` リーフで骨数が倍増する。`MAX_SKINNING_BONES = 128` の余裕を食い、リターゲットも汚れる |
| `bake_anim_simplify_factor` | `0.0` | キー削減はエンジンの `OptimizeVectorKeys` / `OptimizeQuaternionKeys` に任せる |
| `bake_anim` | `True` | LIMIT_ROTATION などの DCC コンストレイントを Transform キーへベイクする (`Docs/design/animation-system-v3.md` の DCC 境界方針) |

### 軸補正の回転はルート直下ノードから押し下げる (2026-08 修正)

回転をルート直下ノード (`MiniBot_Armature` 等) の transform に残していると、
**モデルを別モデルのボーン配下へ入れ子にしたとき回転が二重に掛かる**。
武器をキャラの手ボーンへアタッチすると 90° ずれる、という形で顕在化した。

そこで `NormalizeBlenderRootTransforms` は回転 R をルート直下ノードから剥がし、
その **直下の子ノードのローカルへ押し下げる**。

```
旧: RootChild(T·R·S) → TopBone(L)      W = T·R·L
新: RootChild(T)      → TopBone(R·L)   W = T·R·L   ← W は不変
```

- ルート直下ノードが identity になるので、入れ子にしても二重掛けが起きない
- 頂点は従来どおり `bindBakeRotation` で Y-up 化済み、`offsetMatrix` は R⁻¹ 補正のまま
  → スキニング結果は完全に不変
- `AnimSubExporter` は `axisFixNodes` の回転キーを R⁻¹ で潰し、
  `axisPushNodes` (押し下げ先) のキーへ R を左掛けして整合させる
- 静的メッシュ経路が使う `axisFixRotation` は identity のまま。頂点は Y-up で正しい

**この修正には全 FBX の再インポートが必要**（`.skel` / `.anim` の中身が変わるため）。

### アタッチは「指定ノード基準」で行う

`Engine/Scene/SocketAttach.hpp` の `AttachToSocket()` を使う。

```cpp
// 拳銃の WRoot (グリップ) を右手ソケットへ一致させる
scene::AttachToSocket(*pistolRoot, *sockHandR, "WRoot");
```

modelRoot → attachNode の相対変換をワールド値から実測して `A⁻¹` を入れるため、
途中のノード構成・座標系の差・モデル原点のズレを自動的に吸収する。
呼び出し側は補正値を一切持たなくてよい。

### 軸補正が回転を残す理由 (2026-07 修正 / 現在は上記で押し下げ)

頂点とボーンの生データは Blender の **Z-up のまま**書き出され、Y-up への変換は
RootNode 直下ノードの `-90°X` だけが担っている。

かつて `NormalizeBlenderRootTransforms()` は `F = Rot(q⁻¹)·Scale(1/s)` を左掛けして
**回転ごと**除去していた。これはモデルを Z-up のまま取り込むことに等しく、
`gravity = (0,-9.81,0)` / `worldUp = (0,1,0)` の Y-up ランタイムでは全アセットが
90° 倒れて表示されていた。現在は `F = Scale(1/s)` のみで、回転は保持する。

`ctx.axisFixRotation` は互換のためフィールドを残しているが常に identity で、
`AnimSubExporter` / `ModelSubExporter` 側の合成は自動的に無回転になる。

検証値 (MiniBot.fbx): 頂点 BBox extent が `(2.206, 0.670, 2.519)` → `(2.206, 2.519, 0.670)`
となり最長軸が Z から Y へ移る。`unitScale` は `0.01 × 100 = 1.0` でサイズは不変。

## Root Motion

`AnimSubExporter::IsRootMotionName` は **チャンネル名が `RootMotion` / `Root_Motion`
(大小無視) のノード** だけを Root Motion トラックとして認識する。
`Tools/BlenderExport/fbzz_root_motion.py` がこのノードを整備する。

- `Root_Motion` は Empty で、アーマチュアの **OBJECT 親** にする。
  BONE 親だと骨の動きを引き継いで抽出量と二重になる。
- `AnimatorSystem::SampleNodeLocal` は Root Motion トラックのノードを姿勢評価時に
  bind へ固定する。よって「GameObject を動かす量」と「骨で動かす量」は排他でなければならず、
  Root Motion を持たせる場合は単なるコピーではなく **抽出** (Root_Motion へ移す + Root ボーンから引く) が要る。
- 縦成分は抽出しない。`AnimSubExporter` が `rootMotionApplyY = 0` を書き込むため、
  跳ねやジャンプ弧は骨側に残すのが正しい。

### モード

| mode | 挙動 | 用途 |
|---|---|---|
| `zero` (既定) | ノードだけ用意しキーは恒等 | 原地アニメ。`hasRootMotion = 1` になるが delta は 0 |
| `extract` | 水平移動 + Yaw を Root_Motion へ移し、Root ボーンから差し引く | 実移動を持つクリップ |

`extract` は正味変位 (先頭フレーム→末尾) が 1cm / 1° 未満なら自動的に `zero` へフォールバックする。
ピーク変位ではなく正味変位で判定するのは、原地クリップの揺れ (Idle の ±5mm、Run の ±12mm) を
誤って抜き出さないため。誤抽出した場合は `restore_root_motion()` で完全に元へ戻せる。

Yaw は **ワールド Z 軸まわりの swing-twist 分解**で求める。
「前方ベクトルを水平投影して atan2」方式はピッチ 90° を跨いだ瞬間に 180° 飛ぶ
(MiniBot_Dodge_Roll の 360° 前転で実際に発生した)。

### MiniBot の実測 (2026-07)

全 30 クリップの Root ボーン正味変位は **0**。Walk / Run は最初から原地アニメで、
Dodge_Roll も接地移動ではなく「Root ボーンが体の中心まわりに半径 1.1m の円弧を描く」表現。
この円弧を Root Motion として抜くと GameObject が前後に 1.1m 振られたうえ骨が bind 固定され、
ロールの見た目が壊れる。よって現状は全クリップ `zero`、移動はコード駆動のままとする。

## アニメーションイベント

Blender 側で `FBZZ_EVENT__Footstep` のように命名した補助ノードの Position を
`Footstep` イベントとして取り込む。Position X → `intParam`、Y → `floatParam`。

### ステップ信号として読む (2026-08 修正)

キーを 1:1 でイベント化していた頃は、`bake_anim_step=1.0` が補助ノードも毎フレーム
サンプリングするため **26 フレームのクリップから 26 個のイベントが飛んでいた**。
DCC 側でベイクを切ると 43 本の LIMIT_ROTATION が焼けなくなるので、
`AnimSubExporter` 側で吸収する方式へ変更した。

現在の取り込み規則:

- **先頭キーの値を静止値 (rest) とみなす**
- **rest と異なる値へ遷移した瞬間だけ**イベントとして採用する
- rest へ戻る遷移は発火しない

これにより DCC 側の作法は「普段は静止値、発火させたいフレームで値を変える」だけでよい。

| したいこと | 書き方 |
|---|---|
| 1 回発火 | rest=0 → 発火フレームで 1 |
| 同じイベントを複数回 | 0 → 1 → 0 → 1 (rest を挟む) |
| 種類を撃ち分け | 0 → 1 (右) → 2 (左) のように値を変える |
| そのクリップでは発火させない | ノードにアクションを付けない (静止 = 0 件) |

ベイクでキーが毎フレームに増えても重複値は無視されるため、
センチネル値も特別なエクスポート設定も要らない。

### ツール

`Tools/BlenderExport/fbzz_anim_events.py` がノード生成とキー書き込みを担う。

```python
import fbzz_anim_events as ev
node = ev.ensure_event_empty(armature, "WeaponAttach")
ev.write_events(node, "EV_MiniBot_Draw_Pistols", [(9, 1, 0.0), (11, 2, 0.0)])
```

アクション名を `EV_<アーマチュアのアクション名>` にしておけば、
`fbzz_export_minibot.main()` が書き出し直前に自動で割り当てる
(ノード固有にしたい場合は `EV_<アクション名>__<イベント名>` も可)。

### MiniBot の実装 (2026-08)

| イベント | クリップ | フレーム | intParam |
|---|---|---|---|
| `WeaponAttach` | `Draw_Pistols` | 9 / 11 | 1=右銃 / 2=左銃 |
| `WeaponHolster` | `Holster_Pistols` | 20 / 18 | 1=右銃 / 2=左銃 |

`intParam` の銃を `SOCK_Hand_R/L` (Attach) または `SOCK_Holster_R/L` (Holster) へ
`SetParent` し、ローカル変換を identity にする。

## 検証

`Tools/BlenderExport/fbzz_fbx_inspect.py` が依存なしで FBX バイナリを読み、
テイク名・ボーン数・`Root_Motion` ノードとその AnimationCurveNode 接続を報告する。
Editor を起動せずにエクスポート結果を確認できる。

```
python Tools/BlenderExport/fbzz_fbx_inspect.py Assets/Models/MiniBot/MiniBot_Idle.fbx
```
