# マテリアル統一化設計ドキュメント
## Particle / Trail / MeshTrail を `.mat` ベースに移行する

---

## 1. 現状整理 — 何が既に対応済みか

まず「大規模修正」の実際の範囲を把握するため、現状を整理する。

| コンポーネント | マテリアル管理 | 状態 |
|---|---|---|
| `MeshRenderer` | `MaterialComponent`（`.mat` パス） | ✅ 対応済み |
| `SkinnedMeshRenderer` | `MaterialComponent`（`.mat` パス） | ✅ 対応済み |
| `TerrainComponent` | `layerMaterials[4]`（`.mat` パス×4） | ✅ 対応済み |
| `ParticleEmitter` | `materialPath`（`.mat`） | ✅ 対応済み |
| `TrailComponent` | `materialPath`（`.mat`） | ✅ 対応済み |
| `MeshTrailComponent` | `materialPath`（`.mat`） | ✅ 対応済み |

**結論:** MeshRenderer・SkinnedMeshRenderer・Terrain は既に `.mat` 対応済み。
ParticleEmitter・TrailComponent・MeshTrailComponent も `.mat` を正本とし、全対象が
`.mat` ベースの統一経路へ移行済み。

---

## 2. 移行の方針

### 2-1. 重要な原則：「描画設定」と「シミュレーション設定」を分ける

`.mat` はあくまで**レンダリング側の設定**（シェーダー・テクスチャ・ブレンドモード）を管理するものであり、
Particle や Trail 特有の**シミュレーション設定**（寿命・発生率・幅・重力）は引き続き Component に残す。

```
.mat が担う          Component が担い続ける
─────────────────    ─────────────────────────────
shader               emitRate / lifetime / duration
textures             gravity / emitVelocity
blendMode            colorStart / colorEnd
renderQueue          sizeStart / sizeEnd
render_path          widthStart / widthEnd
sortMode (Particle)  sampleInterval / maxPoints
```

`colorStart / colorEnd` は「マテリアルの色」ではなく「パーティクルの生存時間による色変化」なので、
`.mat` ではなく Component に残すのが正しい。

### 2-2. 採用する移行パターン

```cpp
// 変更前 (ParticleEmitter)
ParticleBlendMode blendMode;

// 変更後
std::string materialPath;  // .mat ファイルへの参照（新規追加）
// 直接テクスチャパスは保持しない。描画テクスチャは MaterialAsset が所有する。
// materialPath が空のときは白テクスチャと既定値を使用する
```

- `materialPath` が設定済み → `.mat` の shader / texture / blendMode を使用

### 2-3. `.mat` の `render_path` / `mesh_type` 相当フィールド

Particle・Trail 用の `.mat` は通常の Surface シェーダーに割り当てることを防ぐため、
専用の用途識別が必要。現状の `MaterialAsset` には `RenderPath` と `MeshType` がある。
`MeshType::Any/Surface/Skinned` に倣って `ParticleType` を追加するか、
より汎用的に `render_path = "particle"` / `render_path = "trail"` を活用する。

```toml
# DemoGame/Assets/Materials/Particles/SwordSpark.mat (例)
version = 1
shader = "Assets/Shaders/Rendering/Particle/SparkParticle.hlsl"
render_path = "particle"
blend_mode = "Additive"
render_queue = 3000

[textures]
albedo = "Assets/Textures/Particles/spark.fztex"
```

---

## 3. 変更スコープ（ファイル一覧）

### Phase 1 — `ParticleEmitter` のみ対応（推奨優先度: 高）

| ファイル | 変更内容 | 規模 |
|---|---|---|
| `Engine/include/Engine/Scene/Components/ParticleEmitter.hpp` | `materialPath` と Reflect() を定義 | S |
| `Engine/include/Engine/Asset/MaterialAsset.hpp` | `RenderPath::Particle` を追加 (or `mesh_type = "particle"` 追加) | XS |
| Particle レンダーパス .cpp（1〜2 ファイル） | `materialPath` が存在する場合に AssetManager 経由で .mat をロードし、テクスチャ・ブレンド・シェーダーを差し替える | M |
| `ScriptParticleProxy.cpp` | マテリアル経由の設定に統一 | S |
| `Engine/src/Scene/SceneSerializer.cpp` | `materialPath` フィールドの読み書きを追加 | S |
| `Editor/src/Panels/Inspector/` (Particle Inspector) | `materialPath` の Inspector UI（ファイルピッカー）を追加 | S |

**Phase 1 合計: 約 6 ファイル変更。**

---

### Phase 2 — `TrailComponent` / `MeshTrailComponent` 対応

| ファイル | 変更内容 | 規模 |
|---|---|---|
| `Engine/include/Engine/Scene/Components/TrailComponent.hpp` | `materialPath` 追加、blendMode を `.mat` から読むように変更 | S |
| `Engine/include/Engine/Scene/Components/MeshTrailComponent.hpp` | `materialPath` 追加、`doubleSided` を `.mat` から読むように変更 | S |
| Trail レンダーパス .cpp（1〜2 ファイル） | materialPath 対応 | M |
| MeshTrail レンダーパス .cpp（1〜2 ファイル） | materialPath 対応 | M |
| `ScriptTrailProxy.cpp` / `ScriptMeshTrailProxy.cpp` | マテリアル設定 API 対応 | S |
| `SceneSerializer.cpp` | Trail・MeshTrail の materialPath 読み書き追加 | S |
| Inspector パネル (Trail・MeshTrail 各 1 ファイル) | materialPath UI | S |

**Phase 2 合計: 約 8 ファイル変更。**

---

### 移行対象外（触らなくてよいもの）

- `WaterComponent` — 既に `materialPath` を持つ（CausticsPass が AssetManager 経由でロード済み）
- `TerrainComponent` — 既に `layerMaterials[4]` 対応済み
- `FoliageComponent` / `DecalComponent` — 独自のマテリアル管理があり別途検討

---

## 4. リスク評価

| リスク | 内容 | 対策 |
|---|---|---|
| シーンファイルの形式 | `materialPath` が未設定のシーン | 白テクスチャとコンポーネント既定値で描画する |
| Inspector の複雑化 | 描画設定とシミュレーション設定の混同 | 描画設定は `.mat` に限定する |
| ScriptParticleProxy API | 直接テクスチャ変更の扱い | Material API のスロット設定へ統一する |
| Particle .mat の mesh_type 誤割り当て | Surface シェーダー用 .mat を Particle に割り当てる誤操作 | `render_path = "particle"` チェックをレンダーパスで行い、不正アサインを警告 |

---

## 5. 現実性の判断

**Phase 1（ParticleEmitter のみ）: 着手可能・工数は小〜中。**

- 変更ファイルは 6 件、1〜2 日で完了可能
- 未設定値は明確な既定値で描画し、形式を一つに保つ
- `DemoGame/Assets/Materials/Particles/` ディレクトリを新設し、
  `.mat` テンプレートを数個用意するだけで新しいワークフローに移行できる

**Phase 2（Trail・MeshTrail）: Phase 1 完了後に着手が妥当。**

- MeshTrail は `doubleSided` / `colorStart/End` の意味論整理が必要
- Trail の `blendMode` は現状 Additive 固定が多いため、
  `.mat` 化で他の合成モードを試せるようになり表現幅が広がる

**やらなくてよいこと:**

- 専用アセット形式（`.pfx`・`.trail` 等）の新設は過剰設計。
  既存の `.mat` 形式に `render_path` を追加するだけで十分。
- `colorStart / colorEnd` の `.mat` 移行は不要。
  これはシミュレーションパラメータであり、マテリアルの概念ではない。

---

## 6. 推奨実装順序

```
1. MaterialAsset に RenderPath::Particle を追加
2. ParticleEmitter.hpp に materialPath を追加
3. Particle レンダーパスを materialPath 優先ロジックに更新
4. SceneSerializer の読み書き追加
5. Inspector の materialPath ピッカー追加
6. DemoGame 用 Particle .mat テンプレートを数種類作成
   → ここまでで Phase 1 完了

7. TrailComponent / MeshTrailComponent で同じパターンを繰り返す（Phase 2）
```

---

## 7. 参考：既存の `.mat` フォーマット

```toml
# 現行 Player_Toon.mat
version = 1
shader = "Assets/Shaders/Material/Skinned/SkinnedToon.hlsl"
blend_mode = "Opaque"
double_sided = false
render_queue = 2000

[textures]
albedo = ""
normal = ""

[params]
albedo = [1.0, 1.0, 1.0, 1.0]
```

Particle 用は `render_path = "particle"` と `blend_mode = "Additive"` を加えるだけで既存フォーマットに乗れる。

```toml
# 想定 Particle 用 .mat
version = 1
shader = "Assets/Shaders/Rendering/Particle/SparkParticle.hlsl"
render_path = "particle"
blend_mode = "Additive"
render_queue = 3000

[textures]
albedo = "Assets/Textures/Particles/spark.fztex"
```
