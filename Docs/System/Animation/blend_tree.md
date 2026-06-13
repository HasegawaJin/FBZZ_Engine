# Animation BlendTree 設計書

`AnimationState` にクリップ単体再生の代わりに **BlendTree** を持たせ、  
1 つのステートで複数クリップをパラメーター駆動でシームレスにブレンドする機能の設計。  
合わせて **AnyState 遷移** を追加し、「どのステートからでも発火できる遷移」を実現する。

---

## 追加機能一覧

| 機能 | 概要 | Unity 対応 |
|---|---|---|
| **1D BlendTree** | Float 1 本で N クリップを線形補間 | Blend Tree → 1D |
| **2D BlendTree (Simple Directional)** | Float 2 本＋方向正規化でブレンド | Blend Tree → 2D Simple Directional |
| **2D BlendTree (Freeform Cartesian)** | Float 2 本を直交座標としてブレンド | Blend Tree → 2D Freeform Cartesian |
| **AnyState 遷移** | 現ステート問わず発火できる遷移リスト | Any State |

---

## データ構造設計

### BlendTreeMotion（1D / 2D 共通の Motion エントリ）

```cpp
// 1 つのモーション（クリップ参照 + ブレンド位置）
struct BlendTreeMotion {
    // 1D での閾値、2D では使わない（posX / posY を使う）
    float    threshold = 0.0f;

    // 2D での配置座標。1D のときは未使用
    float    posX      = 0.0f;
    float    posY      = 0.0f;

    // Motion が直接所有するアニメーションソース参照
    // WHY: Animator 全体の Clip Sources と index の対応をなくし、Node 単体で参照を完結させる
    std::str sourcePath;

    // クリップ解決: clipName → 名前検索、見つからなければ clipIndex 直接指定
    // WHY: FindClipForState と同じ解決順を使い、FBX 名前揺れに対応する
    std::str clipName;
    int      clipIndex = -1;

    // このモーション固有の再生速度スケール（ステート speed と乗算される）
    float    speed     = 1.0f;

    // モーション固有の IK 係数。ステート IK Weight と乗算後、姿勢 Weight で連続補間する
    // WHY: Run のような歩幅が大きいクリップで足IKを弱め、足の引きずりを防ぐ
    float    ikWeight  = 1.0f;
};
```

Controller version 2 では、Clip Stateも同様に`sourcePath`を持つ。RuntimeはState / Motionから
Sourceを自動収集するため、Animator Component側でClip Sourcesを管理する必要はない。
version 1の`clipSources + clipIndex`は読込時にNode Sourceへ移行する。

### BlendTree1D

```cpp
// Float パラメーター 1 本で N クリップを直線補間する BlendTree
// WHY: Idle→Walk→Run のような速度軸の連続ブレンドに最適
struct BlendTree1D {
    std::str                     paramName; // Float パラメーター名
    std::vector<BlendTreeMotion> motions;   // threshold の昇順でソートして使う
};
```

### BlendTree2DType / BlendTree2D

```cpp
// 2D ブレンドの計算モード
// WHY: 方向性のある移動（SimpleDirectional）と非方向的パラメーター（FreeformCartesian）
//      では適切な距離計算が異なるため分岐させる
enum class BlendTree2DType : int {
    SimpleDirectional  = 0, // 正規化方向ベースの Gradient Band + 大きさ軸
    FreeformCartesian  = 1, // XY 直交座標そのまま Gradient Band
};

// Float パラメーター 2 本で N クリップを 2D 平面上でブレンドする BlendTree
struct BlendTree2D {
    std::str                     paramX;  // 横軸 Float パラメーター名（例: "VelocityX"）
    std::str                     paramY;  // 縦軸 Float パラメーター名（例: "VelocityZ"）
    BlendTree2DType              type    = BlendTree2DType::SimpleDirectional;
    std::vector<BlendTreeMotion> motions; // posX / posY で平面上に配置
};
```

### AnimationStateMode（ステートの動作モード）

```cpp
// AnimationState が何を再生するかを区別するタグ
// WHY: optional<BlendTree1D> / optional<BlendTree2D> を持つより、
//      タグ + union-like フィールドのほうがシリアライズと Inspector 描画が単純になる
enum class AnimationStateMode : int {
    Clip        = 0, // 既存の単クリップ再生（後方互換）
    BlendTree1D = 1,
    BlendTree2D = 2,
};
```

### AnimationState（変更後）

既存フィールドはすべて維持し、`mode` と BlendTree フィールドを追加する。

```cpp
struct AnimationState {
    std::str name;
    AnimationStateMode mode      = AnimationStateMode::Clip;

    // --- Clip モード専用 ---
    std::str clipName;
    int      clipIndex = -1;

    // --- 共通 ---
    float    speed    = 1.0f;
    bool     loop     = true;
    float    ikWeight = 1.0f;

    std::vector<AnimationTransition> transitions;

    // --- BlendTree モード専用（mode で選択）---
    BlendTree1D blendTree1D;
    BlendTree2D blendTree2D;
};
```

### AnimatorComponent（追加フィールド）

```cpp
struct AnimatorComponent {
    // ... 既存フィールドは変更なし ...

    // AnyState 遷移: 現ステートを問わず毎フレーム評価される遷移リスト
    // WHY: Dead / HitReaction など「どこからでも割り込む」遷移を
    //      全ステートに手動で書かずに済む
    std::vector<AnimationTransition> anyStateTransitions;
};
```

---

## アルゴリズム設計

### 1D BlendTree の重み計算

```
param value = animator.GetFloat(paramName)
motions は threshold の昇順にソート済み

value <= motions[0].threshold → w[0] = 1.0（左端クランプ）
value >= motions[N-1].threshold → w[N-1] = 1.0（右端クランプ）
それ以外:
  i を探す: motions[i].threshold <= value <= motions[i+1].threshold
  t = (value - threshold[i]) / (threshold[i+1] - threshold[i])
  w[i] = 1 - t,  w[i+1] = t,  他は 0
```

**常に高々 2 クリップのみ非ゼロウェイト**になるが、評価経路を統一するため
`EvaluateNBlendedNodeRecursive` に渡す。

### 2D BlendTree の重み計算（Gradient Band 法）

Unity が採用するアルゴリズム。三角分割不要で任意のモーション配置に対応できる。

```
p = (px, py) = 現在のパラメーター値

各モーション i について:
  w[i] = min_{j ≠ i} [ dot(p - p_j, p_i - p_j) / |p_i - p_j|² ]
  w[i] = max(0, w[i])

正規化: w[i] /= Σ w[k]
```

**直感的な意味**: モーション `i` は「現在点 `p` から見て、他のすべてのモーション `j` より  
自分の方向にある度合い」をスコアとして持つ。最も不利な比較でのスコアを採用することで  
境界が滑らかになる。

#### SimpleDirectional の特殊処理

方向ベクトルの大きさ（magnitude）も意味を持つ場合（Idle が原点にある場合）：

```
magnitude = sqrt(px² + py²)

① magnitude < ε のとき:
   原点に最も近いモーション（Idle 相当）に w = 1.0

② magnitude >= ε のとき:
   pNorm = p / magnitude で方向成分を正規化
   各方向クリップ（原点以外）に Gradient Band を適用 → 方向ウェイト w_dir[i]
   原点クリップとの混合: w_origin = max(0, 1 - magnitude), w_dir *= magnitude
   全ウェイトを正規化
```

#### FreeformCartesian

```
方向正規化なし。p をそのまま Gradient Band に入力する。
原点クリップの特殊処理もなし。
```

### N クリップのブレンド評価

Clip / 1D / 2D / クロスフェードを共通のNクリップ評価へ統合する。

```cpp
// N クリップ・N ウェイトを受け取り、1 ノード分の TRS をブレンドして
// palette と nodeGlobals を更新する。
// WHY: Matrix4 直接補間は回転精度が落ちるため、TRS 分解後に Lerp/Slerp する。
//      Translation / Scale は加重線形和、Rotation は累積 Slerp。
void EvaluateNBlendedNodeRecursive(
    const asset::Skeleton&           skeleton,
    const std::vector<WeightedClip>& clips, // clip / ticks / weight をまとめて保持
    int                              nodeIndex,
    const math::Matrix4&             parentGlobal,
    std::vector<math::Matrix4>&      palette,
    std::vector<math::Matrix4>&      nodeGlobals);
```

#### Rotation の累積 Slerp

```
q = SampleRotation(clips[0], ticks[0])
accumulated = weights[0]

for i = 1 .. N-1:
    q_i = SampleRotation(clips[i], ticks[i])
    accumulated += weights[i]
    t = weights[i] / accumulated      // 現クリップの寄与割合
    q = Slerp(q, q_i, t).Normalized()
```

**WHY**: 球面上の重み付き平均を厳密に求めるには反復法が必要だが、  
ゲームのアニメーションブレンドでは累積 Slerp により回転補間の品質を維持する。  
N が小さい（通常 2〜5）ため誤差も実用上無視できる。

### ステート時間の管理（BlendTree ステート）

BlendTree ステートでも既存の `stateTime`（秒）をそのまま使う。  
`stateTime` 自体を `state.speed * animator.speed` で進め、
各クリップのティック変換時には `motion.speed` を適用する。

```
stateTime    += dt * state.speed * animator.speed
clipTicks[i] = WrapOrClamp(stateTime * motion.speed) * clip.ticksPerSecond
```

**WHY**: クリップ長が異なると再生位置がずれるが、これは Unity デフォルトと同じ挙動  
（モーション同期（Sync）は別途追加機能として扱う）。  
クリップを完全同期させる場合は正規化時間ベースに変更する必要があるが、  
ポートフォリオ目標の範囲では非同期で十分。

#### GetNormalizedTime（BlendTree ステート）

```
weightedDuration = Σ (duration[i] * w[i])  // 現在のブレンドウェイトで加重平均
normalizedTime   = stateTime / weightedDuration
```

---

## AnimatorSystem への変更

### UpdateStateMachine の AnyState 対応

```
① 遷移中（blendToState 非空）のとき:
   既存通り（AnyState は評価しない。簡単のため）

② 通常フレーム:
   [現ステートの transitions を評価]  ← 既存
   ↓ 発火なし
   [anyStateTransitions を評価]       ← 追加
   ↓ 発火なし
   → 何もしない
```

**Unity との差異**: Unity では AnyState が現ステートより高優先だが、  
本実装では低優先（現ステートの遷移が優先）とする。  
WHY: 現ステートの「自分の遷移を先に評価する」直感に反しない設計にするため。

### RunStateMachineAnimatorPath のブレンド評価分岐

```
BlendTree ステートかどうかで評価関数を切り替える:

mode == Clip
  → 1 要素の加重クリップ集合を生成

mode == BlendTree1D
  → Compute1DWeights() で w 計算
  → 非ゼロの 2 クリップを EvaluateNBlendedNodeRecursive に渡す

mode == BlendTree2D
  → ComputeGradientBandWeights() で N ウェイト計算
  → 非ゼロクリップを EvaluateNBlendedNodeRecursive に渡す
```

**クロスフェード中の BlendTree**: 遷移元・先のどちらかが BlendTree の場合も、  
それぞれのポーズ（EvaluateNBlendedNodeRecursive 結果）を通常通り Lerp/Slerp する。  
すなわち「BlendTree から単クリップへのフェード」も既存の blendWeight 機構で動く。

---

## シリアライズ設計

`SceneSerializer` で `AnimationState` を直接変換している箇所に追記する。

### AnimationState の追加フィールド

```toml
[[states]]
name = "Move"
mode = 1          # 1 = BlendTree1D

[states.blendTree1D]
paramName = "Speed"

[[states.blendTree1D.motions]]
threshold = 0.0
clipName  = "Idle"

[[states.blendTree1D.motions]]
threshold = 0.5
clipName  = "Walk"
speed     = 1.0

[[states.blendTree1D.motions]]
threshold = 1.0
clipName  = "Run"
speed     = 1.2
```

```toml
[[states]]
name = "Locomotion"
mode = 2          # 2 = BlendTree2D

[states.blendTree2D]
paramX = "VelocityX"
paramY = "VelocityZ"
type   = 0        # 0 = SimpleDirectional

[[states.blendTree2D.motions]]
posX = 0.0
posY = 0.0
clipName = "Idle"

[[states.blendTree2D.motions]]
posX = 0.0
posY = 1.0
clipName = "RunForward"

[[states.blendTree2D.motions]]
posX =  1.0
posY = 0.0
clipName = "RunRight"

[[states.blendTree2D.motions]]
posX = -1.0
posY = 0.0
clipName = "RunLeft"
```

### anyStateTransitions のシリアライズ

```toml
[[anyStateTransitions]]
toStateName        = "Dead"
transitionDuration = 0.1
hasExitTime        = false

[[anyStateTransitions.conditions]]
paramName = "IsDead"
op        = 4     # True
threshold = 0.0
```

### 遷移の優先順位と Exit Time

- 同一ステートの遷移は配列の先頭から評価し、最初に成立した遷移だけを開始する。
- `hasExitTime = false` は条件成立時に即時遷移するため、入力応答を優先する遷移に使う。
- `hasExitTime = true` は正規化再生位置が `exitTime` に達した後で条件を評価する。
- 着地では `Speed > 0.1` の即時遷移を先頭に置き、停止中のみ Exit Time 遷移で着地モーションを再生する。

---

## ScriptAnimatorProxy API 拡張

BlendTree 追加による Script API 変更は最小限。既存 API はそのまま使える。

| 追加 API | 説明 |
|---|---|
| `GetCurrentBlendWeights()` | 現ステートの BlendTree 各クリップウェイトを返す（デバッグ用） |

```cpp
// ScriptAnimatorProxy.hpp に追加
// 現ステートが BlendTree のとき、clipName → weight のマップを返す。
// Clip ステートのときは空マップ。デバッグ・エフェクト連動用。
std::vector<std::pair<std::string, float>> GetCurrentBlendWeights() const;
```

---

## AnimationGraphPanel への影響

詳細は `Docs/Editor/animation_graph_editor.md` を参照。  
ここでは BlendTree 追加に伴うエディタ側の差分のみ記述する。

### ノード表示の変更

| AnimationStateMode | ノード内表示 |
|---|---|
| Clip | clipName / speed / loop（既存） |
| BlendTree1D | `[1D] Speed` + モーション閾値リスト |
| BlendTree2D | `[2D] VelX / VelZ` + モーションをドット配置（Preview ウィンドウ） |

### BlendTree 専用 Inspector ペイン

ノードを選択して詳細ペインに BlendTree 編集フォームを表示する。

```
1D: paramName コンボ → motions リスト（threshold / clipName / speed）
    [+ Motion] [ソート] → motions を threshold で昇順ソートするボタン

2D: paramX / paramY コンボ → 2D プレビュー（100×100 ドット図）
    各モーションを XY 座標の点として表示
    現在のパラメーター値（ランタイム時）をリアルタイム更新
```

実装済みの操作性改善:

- Float 型パラメーターだけを候補表示するコンボ選択
- ロード済み AnimationClip のコンボ選択
- モーションの折り畳み表示、複製、削除
- 2D プレビュー上でのモーション座標ドラッグ編集
- 未設定パラメーター、空クリップ、重複閾値・座標の警告
- ランタイム値と最終ブレンドウェイトのパーセント表示
- ホイールによるカーソル位置基準ズーム
- Shift / Alt + ホイールと中ボタンドラッグによるキャンバス移動
- `F` キーによる選択ステートへのフォーカス

### AnyState ノード

キャンバス上に固定の `[Any State]` ノードを追加。  
ここから出力ピンをドラッグすると `anyStateTransitions` に追加される。

---

## 実装フェーズ

| 完了 | Phase | 内容 | 完了条件 |
|---|---|---|---|
| [x] | **1** | データ構造追加（`BlendTreeMotion`, `BlendTree1D`, `BlendTree2D`, `AnimationStateMode`） | 実装完了。ビルド確認は Visual Studio で実施する |
| [x] | **2** | シリアライズ対応（`SceneSerializer` の `AnimationState` 変換を拡張） | BlendTree / AnyState の読み書きを実装 |
| [x] | **3** | 1D BlendTree 評価（`Compute1DWeights` + `RunStateMachineAnimatorPath` の分岐） | 閾値ソート・端点クランプ・2モーション補間を実装 |
| [x] | **4** | AnyState 遷移（`anyStateTransitions` + `UpdateStateMachine` の追加評価） | 通常遷移の後に AnyState を評価する経路を実装 |
| [x] | **5** | N クリップブレンド関数（`EvaluateNBlendedNodeRecursive`） | Translation / Scale 加重和と Rotation 累積 Slerp を実装 |
| [x] | **6** | 2D BlendTree 評価（`ComputeGradientBandWeights` + FreeformCartesian 分岐） | SimpleDirectional / FreeformCartesian を実装 |
| [x] | **7** | AnimationGraphPanel の BlendTree UI（Inspector ペイン + AnyState ノード） | モーション編集、ランタイムウェイト表示、Any State ノードを実装 |
| [x] | **8** | `GetNormalizedTime` / `GetCurrentBlendWeights` のBlendTree 対応 | Script API と加重平均再生時間を実装 |

---

## 変更対象ファイル

```
Projects/Engine/
  include/Engine/Scene/Components/
    AnimatorComponent.hpp     ← BlendTree 構造体・anyStateTransitions 追加
  src/Scene/Systems/
    AnimatorSystem.cpp        ← Compute*Weights / EvaluateNBlended / AnyState 評価
  src/Scene/
    SceneSerializer.cpp       ← BlendTree / anyStateTransitions のシリアライズ
  src/Scene/
    ScriptProxies.cpp         ← GetCurrentBlendWeights 実装

Projects/Editor/
  src/Panels/
    AnimationGraphPanel.cpp   ← BlendTree Inspector ペイン / AnyState ノード
  src/Panels/Inspector/
    InspectorAnimation.cpp    ← BlendTree モードの Inspector リスト表示

Docs/System/Animation/
  blend_tree.md               ← 本ドキュメント
```

---

## 隣接ドキュメント・コード

- [overview.md](overview.md) — AnimatorSystem / IKSystem 全体概要
- [../../../Docs/Editor/animation_graph_editor.md](../../Editor/animation_graph_editor.md) — Animation Graph エディタ設計
- `Projects/Engine/include/Engine/Scene/Components/AnimatorComponent.hpp` — ランタイムデータ定義
- `Projects/Engine/src/Scene/Systems/AnimatorSystem.cpp` — ステートマシン・評価ロジック
- `Projects/Engine/src/Scene/SceneSerializer.cpp` — シリアライズ実装
