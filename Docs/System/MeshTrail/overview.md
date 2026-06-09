# MeshTrailRenderer 設計 — 全体概要

## 目的

MeshRenderer / SkinnedMeshRenderer の形状を過去姿勢で半透明再描画する「メッシュ残像」システム。
リボン Trail は点列から帯を生成するが、キャラクターや武器そのものの形状残像はメッシュ再描画の方が破綻しにくい。

---

## 現在の実装状況

| 機能 | 実装箇所 |
|---|---|
| ワールド行列 + ボーンパレットの時間ベースサンプリング | `MeshTrailRenderSystem.cpp::CaptureSample()` |
| duration ベースのサンプル期限切れ削除 | `ExpireSamples()` |
| Static Mesh（MeshRenderer）再描画 | `DrawStaticMeshSample()` |
| Skinned Mesh（SkinnedMeshRenderer）再描画 | `DrawSkinnedMeshSample()` |
| `doubleSided` トグル（背面カリング有無） | `meshTrailDoubleSidedPSO` 切り替え |
| サンプルごとのカラーフェード（colorStart → colorEnd 線形） | `SampleColor()` |
| `clearRequested` フラグによる安全なサンプル解放 | `ExecuteMeshTrailPass()` |
| `clearOnDisable=false` 時の自然フェードアウト | `ExecuteMeshTrailPass()` |
| SkinningCB の dirty flag による初回アップロード | `MeshTrailSample::skinningCBDirty` + `EnsureSampleSkinningCB()` |
| テクスチャオーバーライド | `texturePath` + `DrawCall::textures[0]` + `MeshTrail.hlsl` |
| submesh 除外 | `excludedMeshIndices` + `IsMeshIndexExcluded()` |
| サンプルリングバッファ | `sampleHead` / `sampleCount` / `EnsureSampleStorage()` |
| Script API（SetEnabled / Clear / SetDuration / SetSampling / SetMaxSamples / SetColor / SetDoubleSided / SetTexture / AddExcludedMeshIndex） | `ScriptMeshTrailProxy.hpp` |

---

## 実装完了した不足機能・バグ

### 重要度：高

#### `enabled=false` 時に Proxy と System が乖離するバグ（完了）

`ScriptMeshTrailProxy::SetEnabled(false, clearWhenDisabled=false)` は
`clearRequested` を立てない（Proxy 実装は正しい）。
しかし `ExecuteMeshTrailPass` は `!enabled` なら無条件で `ClearSamples()` を呼ぶ：

```cpp
// MeshTrailRenderSystem.cpp — 問題箇所
if (!trail->enabled) {
    ClearSamples(*trail, resources);
    trail->clearRequested = false;
    continue;
}
```

**結果：** "無効化後もサンプルをフェードアウトさせたい" ユースケースが機能しない。
TrailRenderer と同じ構造の問題。

**修正方針：** `MeshTrailComponent` に `bool clearOnDisable` フィールドを追加するか、
`!enabled` 時はサンプリングのみ停止して `ExpireSamples` による自然消滅に任せる。

**実装：** `clearOnDisable=false` の場合はサンプリングだけ停止し、既存サンプルは `ExpireSamples()` で自然消滅する。

---

#### SkinningCB の毎フレーム再アップロード（完了）

`EnsureSampleSkinningCB` はサンプル後に変化しない bone matrices を毎フレーム `resources.Update()` している：

```cpp
// 問題：skinningCB.IsValid() チェック後も無条件で Update する
if (!sample.skinningCB.IsValid())
    sample.skinningCB = resources.CreateConstantBuffer(sizeof(SkinningCB));
// ↓ ここは毎フレーム実行される
SkinningCB cb{};
for (size_t i = 0; i < sample.boneMatrices.size(); ++i)
    cb.boneMatrices[i] = sample.boneMatrices[i];
resources.Update(sample.skinningCB, &cb, sizeof(cb));
```

**結果：** サンプル数 × フレームレート 分の CB 転送が走る。
maxSamples=12 程度では問題ないが、サンプル数を増やすと CPU→GPU 転送コストが線形増加する。

**修正方針：** `MeshTrailSample` に `bool skinningCBDirty = true` フラグを追加し、
`CreateConstantBuffer` 直後の初回のみ `Update` してフラグを落とす。

**実装：** `EnsureSampleSkinningCB()` は dirty のときだけ `resources.Update()` し、以後は同じ CB を再利用する。

---

### 重要度：中

#### マテリアル / テクスチャオーバーライド（完了）

`MeshTrailCB.trailColor` による単色ティントのみ。
カスタムシェーダーや残像テクスチャ（グラデーション・ディゾルブ）が使えない。

**修正方針：** `MeshTrailComponent` に `std::string texturePath` と対応する
`ResourceHandle<TextureTag>` を追加し、`DrawStaticMeshSample` / `DrawSkinnedMeshSample` で
`dc.textures[0]` にバインドする。

**実装：** `texturePath` を保存・Inspector・Script API に通し、空パス時は 1x1 白テクスチャを使う。

---

#### サブメッシュ選択（完了）

モデル内の全メッシュを無条件に描画する。
甲冑の残像だけ出して顔メッシュは除外する、といった制御ができない。

**修正方針：** `MeshTrailComponent` に `std::vector<int> excludedMeshIndices` を追加し、
`DrawSkinnedMeshSample` のメッシュループで skip 判定を挟む。

**実装：** `IsMeshIndexExcluded()` により SkinnedMesh の mesh loop で指定 index を skip する。

---

### 重要度：低

#### `samples.erase(begin())` が O(n)（完了）

```cpp
trail.samples.erase(trail.samples.begin());  // 全要素を左シフト
```

`maxSamples = 12` 程度なら問題ないが、大きくすると古いサンプル削除のコストが線形増加する。

**修正方針：** `samples` をリングバッファ（`head` インデックス管理）に変更する。
ただし `MeshTrailSample` は `ResourceHandle` を持つため移動セマンティクスに注意が必要。

**実装：** `samples` は固定スロットとして確保し、`sampleHead` / `sampleCount` で論理順を管理する。

---

## アーキテクチャ

```
│ Scene Layer                                                      │
│  GameObject + MeshTrailComponent                                 │
│    ・duration / sampleInterval / minVertexDist / maxSamples      │
│    ・colorStart / colorEnd                                       │
│    ・doubleSided                                                 │
│    ・samples: vector<MeshTrailSample>                            │
│        └ timestamp / position / world / boneMatrices / skinningCB│
│    ・clearRequested: bool                                        │
               ↓ ExecuteMeshTrailPass が Scene をイテレート
│ System Layer                                                     │
│  MeshTrailRenderSystem                                           │
│    1. ExpireSamples（duration 超過の古いサンプルを解放）         │
│    2. ShouldSample → CaptureSample（時間 + 距離の両条件でサンプル） │
│    3. 全サンプルをループして DrawCall 発行                       │
│       Static  → DrawStaticMeshSample（MeshRenderer 頂点 + 世界行列） │
│       Skinned → DrawSkinnedMeshSample（+ 過去 bone palette）    │
│                  ↓ ResourceManager 経由                         │
│  IRenderer（DX11 具体実装に非依存）                              │
```

---

## フレーム描画順序における位置づけ

```
  1. RenderSystem（不透明メッシュ）
  2. TerrainRenderSystem
  3. WaterRenderSystem（半透明）
  4. MeshTrailRenderSystem（半透明）← サンプル古い順から描画が理想だが現状未ソート
  5. TrailRenderSystem（半透明）
  6. ParticlePass（半透明）
  7. PostProcessSystem
```

---

## 修正優先順位まとめ

| 優先度 | 項目 | 工数目安 |
|---|---|---|
| 1 | `enabled=false` / `clearWhenDisabled=false` のバグ修正 | 小（System 1 箇所の条件分岐） |
| 2 | SkinningCB 毎フレーム再アップロード削減 | 小（dirty フラグ追加） |
| 3 | テクスチャオーバーライド | 中（Component フィールド追加 + DrawCall バインド） |
| 4 | サブメッシュ選択 | 小（excludedMeshIndices + ループ内 skip） |
| 5 | `samples` リングバッファ化 | 中（ResourceHandle の移動を伴うため慎重に） |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/
    Scene/
      Components/
        MeshTrailComponent.hpp       ← MeshTrailSample + MeshTrailComponent 定義
      ScriptProxy/
        ScriptMeshTrailProxy.hpp     ← Script API
      Systems/
        MeshTrailRenderSystem.hpp    ← システムシグネチャ
  src/Scene/
    Systems/
      MeshTrailRenderSystem.cpp      ← サンプリング・描画ロジック

Assets/Shaders/
  Material/
    Effects/
      MeshTrail.hlsl                 ← VS（スキニング対応）+ PS（trailColor ティント）
      SkinnedMeshTrail.hlsl          ← Skinned 版 VS

Docs/System/MeshTrail/
  overview.md                        ← このファイル
```

---

## 隣接ドキュメント

- [../Trail/overview.md](../Trail/overview.md) — リボン Trail の全体設計
- [../Trail/missing.md](../Trail/missing.md) — TrailRenderer の不足・改善点（共通のバグあり）
- [../Particle/overview.md](../Particle/overview.md) — 同じ半透明パスを使うパーティクルシステム
