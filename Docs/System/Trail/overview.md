# Trail Renderer 設計 — 全体概要

## 目的

移動するオブジェクトの軌跡をリボン状メッシュとして描画するシステム。
剣閃・魔法弾・車のタイヤ跡など「残像として消えていく帯」を汎用的に実現する。

初稿設計から以下を改善した：

| 問題 | 改善 |
|---|---|
| `std::deque` — キャッシュ局所性が低く毎フレームのpush/popでヒープ断片化 | **固定サイズリングバッファ**（pre-alloc、インデックスのみ更新） |
| CPU 側で全頂点のカラー/幅を展開してから転送 | **GPU 側 lerp**（`age` 属性 + `TrailConstants` cbuffer）で転送量 33% 削減 |
| セグメント法線が不連続 → 継ぎ目でリボンが割れる | **マイタージョイント**（隣接法線を平均化・クランプ） |
| 固定距離閾値 → 低 FPS で点が飛び、高 FPS で密集 | **時間ベースサンプリング**（`sampleInterval` 秒ごと + 最小距離の両方で制御） |
| 動的 VB を Phase 7 に先送り → 暫定で毎フレーム VB 再生成 | **Phase 1 から `Map/Unmap` 動的 VB** を使用、暫定実装なし |

---

## ユースケース

| ユースケース | 担当コンポーネント / ツール |
|---|---|
| オブジェクトにトレイルを付ける | `TrailComponent` |
| トレイルの自動更新・頂点生成 | `TrailRenderSystem` |
| テクスチャ・アルファフェード | `Trail.hlsl` |
| スクリプトから動的生成・消去 | `TrailComponent::enabled` を切り替え |
| エディタでパラメータ調整 | `TrailTool`（Phase 6） |

---

## アーキテクチャ全体図

```
│ Editor Layer                                              │
│  TrailTool                                                │
│    ↓ TrailComponent パラメータを変更                      │
               ↓ reads & writes
│ Scene Layer                                               │
│  GameObject + TrailComponent                              │
│    ・maxPoints / duration / sampleInterval                │
│    ・widthStart / widthEnd（幅の先端・末尾）              │
│    ・colorStart / colorEnd（カラーは TrailConstants へ）  │
│    ・alignment（CameraFacing / WorldUp）                  │
│    ・固定サイズリングバッファ（TrailPoint[]）             │
               ↓ SceneView<TrailComponent, Transform>
│ System Layer                                              │
│  TrailRenderSystem                                        │
│    ・sampleInterval 経過 && 距離 >= minVertexDist → 新点 │
│    ・duration 超過の古い点を head から削除               │
│    ・マイタージョイント法線でリボン頂点を CPU 展開       │
│    ・動的 VB を Map/Unmap で更新（フレームごと）         │
│    ・TrailConstants を GPU へ Upload                      │
│    ・TRANSPARENT_LAYER で DrawCall 発行                   │
│                  ↓ ResourceManager 経由                   │
│  IRenderer（DX11 具体実装に非依存）                       │
```

---

## コアコンセプト：マイタージョイント付きリボン生成

各制御点で **隣接 2 セグメントの法線を平均化（マイタージョイント）** することで、
頂点を共有するクワッド間に隙間・歪みが生じない。

```
セグメント法線のみ（旧設計）        マイタージョイント（新設計）
  ┌──┐ ┌──┐ ┌──┐                  ┌─────────────────┐
  │  │ │  │ │  │  ← 継ぎ目の隙間   │                 │  ← 隙間なし
  └──┘ └──┘ └──┘                  └─────────────────┘

 P0──P1──P2──P3                    P0──P1──P2──P3
      ↑ 法線不連続                       ↑ miter = normalize(nL + nR)
```

アライメントモードは 2 種類：

| モード | 法線計算 | 用途 |
|---|---|---|
| `CameraFacing` | `cross(segDir, toCamera)` | 剣閃・エフェクト全般 |
| `WorldUp` | `cross(segDir, (0,1,0))` | タイヤ跡・地面エフェクト |

---

## GPU 側 lerp によるデータ削減

頂点ごとにカラーを展開する代わりに `age` ひとつを渡す。
シェーダーが `TrailConstants` の `colorStart / colorEnd` で lerp する。

```
旧設計の頂点レイアウト: position(12) + uv(8) + color(16) = 36 bytes/vertex
新設計の頂点レイアウト: position(12) + age(4) + v(4) + u(4) = 24 bytes/vertex  → 33% 削減
```

---

## 主要クラス一覧

| クラス | 層 | 役割 |
|---|---|---|
| `TrailPoint` | Scene / Component | 制御点 1 つ（位置・タイムスタンプ） |
| `TrailComponent` | Scene / Component | リングバッファ・パラメータ・GPU リソースハンドル |
| `TrailRenderSystem` | System | 点列更新・マイタージョイント展開・DrawCall 発行 |
| `TrailTool` | Editor | パラメータ調整 UI（Phase 6） |

---

## 依存関係

```
TrailTool (Editor)
    ↓ TrailComponent を読み書き
TrailComponent (Scene/Components)
    ↓ SceneView 経由で参照
TrailRenderSystem (Scene/Systems)
    ↓ ResourceManager / IRenderer 経由
GPU (DX11)
```

- 他 System への書き込み依存を持たない（`ParticleEmitter` と同様）。
- エンジン既定の依存方向 `sandbox → engine → physics → math` を維持する。

---

## フレーム描画順序における位置づけ

```
  1. RenderSystem（不透明メッシュ）
  2. TerrainRenderSystem
  3. WaterRenderSystem（半透明）
  4. TrailRenderSystem（半透明）← RenderQueue::TRANSPARENT_QUEUE + 10
  5. PostProcessSystem
```

`ParticleEmitter` も同 `TRANSPARENT_LAYER` だが、`renderQueue` 値でトレイルを粒子より手前に描く。

---

## 実装フェーズ計画

| Phase | 内容 | 完了条件 |
|---|---|---|
| **1** | `TrailComponent` + リングバッファ + `TrailRenderSystem` 骨格 + 動的 VB | コンポーネントが Attach できる・VB が毎フレーム Map/Unmap される |
| **2** | 時間ベースサンプリング + 単色マイタージョイントリボン描画 | 移動オブジェクトに継ぎ目のない帯が追従する |
| **3** | `Trail.hlsl` + `TrailConstants` cbuffer：テクスチャ UV + GPU 側 `age` lerp + UV スクロール | テクスチャが貼られ末尾が透明になる。`uvScrollSpeed` で炎・煙が流れる |
| **4** | `CameraFacing` / `WorldUp` アライメント切り替え | モード変更でリボン向きが変わる |
| **5** | Catmull-Rom サブディビジョン（`smoothSubdivisions` > 0 時） | 急カーブでも滑らかな曲線になる |
| **6** | `TrailTool`（エディタ UI） | エディタで幅・色・時間をリアルタイム調整できる |
| **7** | `Reflect()` + `SceneSerializer` 対応 | シーン保存・ロードで設定が維持される |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/
    Scene/
      Components/
        TrailComponent.hpp        ← TrailPoint + TrailComponent 定義
      Systems/
        TrailRenderSystem.hpp     ← システムシグネチャ
  src/Scene/
    Systems/
      TrailRenderSystem.cpp       ← 点列更新・マイタージョイント・描画ロジック

Assets/Shaders/
  Material/
    Effects/
      Trail.hlsl                  ← VS（パススルー）+ PS（age lerp + テクスチャ + UV スクロール）

Docs/System/Trail/
  overview.md                     ← このファイル
  rendering.md                    ← データ構造・マイタージョイント・シェーダー詳細
```

---

## 隣接ドキュメント

- [rendering.md](rendering.md) — TrailComponent データ設計・リングバッファ・マイタージョイント・シェーダー詳細
- [../Water/overview.md](../Water/overview.md) — 半透明描画パスの先行実装例（WaterRenderSystem）
