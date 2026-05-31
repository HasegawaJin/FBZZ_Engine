# Terrain System 設計 — 全体概要

## 目的

地形（ハイトマップベースのメッシュ）の生成・描画・編集を行うシステム。
屋外シーンに起伏のある地面を配置し、エディタで直感的に形状を彫刻・テクスチャを塗れることを目標とする。

---

## ユースケース

| ユースケース | 担当コンポーネント / ツール |
|---|---|
| シーンに広い起伏地形を置く | `TerrainComponent` |
| 描画（マルチレイヤーテクスチャ） | `TerrainRenderSystem` |
| エディタで高さを彫刻する | `TerrainTool` (Sculpt モード) |
| エディタでテクスチャを塗る | `TerrainTool` (Paint モード) |
| 物理コリジョン | `TerrainCollider` (Phase 7) |
| 草・木の配置 | `FoliageSystem` (Phase 8) |

---

## アーキテクチャ全体図

```
┌─────────────────────────────────────────────────────┐
│ Editor Layer                                        │
│  TerrainTool (Sculpt / Paint)                       │
│    ↓ heightDirty / splatDirty を立てる              │
└─────────────────────────────────────────────────────┘
          ↓ reads & writes
┌─────────────────────────────────────────────────────┐
│ Scene Layer                                         │
│  GameObject + TerrainComponent                      │
│    ・heightData[]  — CPU 高さ配列 [0,1]             │
│    ・splatData[]   — CPU スプラットマップ RGBA8     │
│    ・TerrainLayer[4] — テクスチャ定義               │
└─────────────────────────────────────────────────────┘
          ↓ SceneView<TerrainComponent, Transform>
┌─────────────────────────────────────────────────────┐
│ System Layer                                        │
│  TerrainRenderSystem                                │
│    ・dirty → チャンクメッシュ再構築 (CPU→GPU)       │
│    ・フラスタムカリング                              │
│    ・DrawCall 発行                                   │
│                  ↓ ResourceManager 経由              │
│  IRenderer (DX11 具体実装に非依存)                  │
└─────────────────────────────────────────────────────┘
```

---

## 主要クラス一覧

| クラス | 層 | 役割 |
|---|---|---|
| `TerrainLayer` | Scene / Component | テクスチャ 1 レイヤーの定義（拡散光テクスチャ・法線マップ・タイリング） |
| `TerrainComponent` | Scene / Component | 地形の全データ（ハイトマップ・スプラットマップ・レイヤー・パラメータ） |
| `TerrainChunk` | System 内部 | チャンク 1 枚の GPU バッファと AABB（TerrainRenderSystem が所有） |
| `TerrainRenderSystem` | System | シーン内の TerrainComponent を走査し描画コマンドを発行 |
| `TerrainTool` | Editor | ブラシ UI・レイキャスト・高さ編集・テクスチャペイント |

---

## 依存関係

```
TerrainTool (Editor)
    ↓ TerrainComponent を読み書き
TerrainComponent (Scene/Components)
    ↓ SceneView 経由で参照
TerrainRenderSystem (Scene/Systems)
    ↓ ResourceManager / IRenderer 経由
GPU (DX11)
```

エンジン既定の依存方向 `sandbox → engine → physics → math` を維持する。
TerrainTool は Editor レイヤーに属し、engine には逆依存しない。

---

## 実装フェーズ計画

| Phase | 内容 | 完了条件 |
|---|---|---|
| **1** | `TerrainComponent` 定義 + `TerrainRenderSystem` 基本実装（全頂点 1 チャンク・単色） | シーンに地形が表示される |
| **2** | 法線計算（有限差分）+ シングルテクスチャ | ライティングが正しく当たる |
| **3** | チャンク分割 + フラスタムカリング | 大規模地形でもフレームレートが落ちない |
| **4** | スプラットマップ 4 レイヤーブレンド | マルチテクスチャ描画が正しく見える |
| **5** | `TerrainTool` Sculpt モード（Raise / Lower / Smooth / Flatten） | エディタで高さを彫刻できる |
| **6** | `TerrainTool` Paint モード（スプラットマップ編集） | エディタでテクスチャを塗れる |
| **7** | `TerrainCollider`（Physics 統合） | キャラクターが地面の上を歩ける |
| **8** | `FoliageSystem`（草・木の散布） | インスタンス描画で草・木を配置できる |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/
    Scene/
      Components/
        TerrainComponent.hpp     ← TerrainLayer + TerrainComponent 定義
      Systems/
        TerrainRenderSystem.hpp  ← システムシグネチャ
  src/Scene/Systems/
    TerrainRenderSystem.cpp      ← チャンク生成・描画ロジック

Projects/Editor/
  src/Tools/
    TerrainTool.hpp              ← ブラシ UI・Sculpt / Paint ロジック
    TerrainTool.cpp

Assets/Shaders/
  Terrain.hlsl                   ← 頂点・ピクセルシェーダー（4 レイヤーブレンド）
```

---

## 隣接ドキュメント

- [rendering.md](rendering.md) — TerrainComponent データ設計・チャンク・法線・シェーダー詳細
- [tool.md](tool.md) — TerrainTool（エディタ）のブラシ設計・アルゴリズム詳細
