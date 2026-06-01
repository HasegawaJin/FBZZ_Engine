# Water System 設計 — 全体概要

## 目的

海・湖・川などの水面を表現するシステム。TerrainSystem と統合し、
地形との接触面（岸辺）を自動検出して泡エフェクトを生成する。
Gerstner 波による頂点変位・デュアル法線マップスクロール・Fresnel 反射・
深度ベースの水色グラデーションを組み合わせてリアルな水面を実現する。

---

## ユースケース

| ユースケース | 担当コンポーネント / ツール |
|---|---|
| シーンに海・湖を配置する | `WaterComponent` |
| 水面の波アニメーション | `WaterRenderSystem` + `Water.hlsl`（Gerstner 波） |
| 岸辺の泡エフェクト | `WaterRenderSystem`（TerrainComponent から泡マスク生成） |
| 浅瀬・深部の水色グラデーション | `Water.hlsl`（シーン深度バッファ参照） |
| Fresnel 反射（水面の鏡面反射） | `Water.hlsl`（キューブマップ + Fresnel 方程式） |
| エディタでパラメータ調整 | `WaterTool`（Phase 7） |
| アセット保存・読み込み | `WaterAssetSerializer`（.fbzzwater, Phase 8） |

---

## アーキテクチャ全体図

```
┌─────────────────────────────────────────────────────┐
│ Editor Layer                                        │
│  WaterTool                                          │
│    ↓ WaterComponent パラメータを変更・meshDirty 等   │
└─────────────────────────────────────────────────────┘
          ↓ reads & writes
┌─────────────────────────────────────────────────────┐
│ Scene Layer                                         │
│  GameObject + WaterComponent                        │
│    ・グリッド解像度・広さ・水位                      │
│    ・色設定・Fresnel・透明度                         │
│    ・Gerstner 波パラメータ（最大 4 波）              │
│    ・法線マップ・泡テクスチャ設定                    │
│    ・meshDirty / foamDirty / texDirty フラグ         │
└─────────────────────────────────────────────────────┘
          ↓ SceneView<WaterComponent, Transform>
          ↓ SceneView<TerrainComponent, Transform>（泡マスク生成時）
┌─────────────────────────────────────────────────────┐
│ System Layer                                        │
│  WaterRenderSystem                                  │
│    ・dirty → 平面グリッドメッシュ再構築（CPU→GPU）  │
│    ・foamDirty → TerrainComponent を参照して         │
│      岸辺泡マスクテクスチャを生成（CPU→GPU）        │
│    ・ALPHA_BLEND で DrawCall 発行（Terrain より後）  │
│                  ↓ ResourceManager 経由              │
│  IRenderer（DX11 具体実装に非依存）                 │
└─────────────────────────────────────────────────────┘
```

---

## TerrainSystem との統合

WaterSystem は TerrainSystem に **読み取り専用で依存** する。書き込みは行わない。

| 統合ポイント | 内容 |
|---|---|
| 岸辺泡マスク生成 | `TerrainComponent::GetHeightAt()` で地形高さをサンプリングし、水面との高さ差から泡の強度を計算して CPU テクスチャに書き込む |
| 深度による水没判定 | 地形 Z バッファが水面ピクセルを自然に遮蔽（特別な処理不要） |
| レンダリング順序 | TerrainRenderSystem の後に WaterRenderSystem を実行し、水面を透明パスとして描画 |

```
フレーム描画順序:
  1. RenderSystem（不透明メッシュ）
  2. TerrainRenderSystem（地形・不透明）
  3. WaterRenderSystem（水面・半透明）← ここ
  4. PostProcessSystem
```

---

## 主要クラス一覧

| クラス | 層 | 役割 |
|---|---|---|
| `GerstnerWave` | Scene / Component | 1 つの Gerstner 波パラメータ（方向・振幅・波長・急峻度） |
| `WaterComponent` | Scene / Component | 水面の全データ（ジオメトリ・色・波・泡・テクスチャ・フラグ） |
| `WaterRenderSystem` | System | WaterComponent を走査し、グリッドメッシュ・泡マスク生成・DrawCall 発行 |
| `WaterTool` | Editor | パラメータ調整 UI（Phase 7） |
| `WaterAssetSerializer` | Engine | .fbzzwater へのシリアライズ（Phase 8） |

---

## 依存関係

```
WaterTool (Editor)
    ↓ WaterComponent を読み書き
WaterComponent (Scene/Components)
    ↓ SceneView 経由で参照
WaterRenderSystem (Scene/Systems)
    ↓ TerrainComponent::GetHeightAt() を読み取り（逆依存なし）
    ↓ ResourceManager / IRenderer 経由
GPU (DX11)
```

エンジン既定の依存方向 `sandbox → engine → physics → math` を維持する。
WaterSystem と TerrainSystem の依存は「Water が Terrain を読む」一方向のみで、
Terrain 側は Water の存在を知らない。

---

## 実装フェーズ計画

| Phase | 内容 | 完了条件 |
|---|---|---|
| **1** | `WaterComponent` 定義 + 平面グリッドメッシュ生成 + 単色表示 | 水面平面がシーンに表示される |
| **2** | デュアル法線マップスクロール + Fresnel + 環境キューブマップ反射 | 波アニメーションと鏡面反射が見える |
| **3** | Gerstner 波（頂点シェーダー変位） | 水面が 3D 的に揺れる |
| **4** | 岸辺泡マスク（TerrainComponent 統合） | 海岸線に泡が現れる |
| **5** | 深度ベース水色 + **スクリーンスペース屈折** | 浅瀬・深部で色が変わり、水中が歪んで見える |
| **6** | **水没カメラ PostProcess**（PostProcCB 拡張・Composite.hlsl 追記） | カメラが水中に入ると水中フォグ・ゆらぎが掛かる |
| **7** | **フローマップ**（WaterComponent + Water.hlsl 拡張） | 川・流れの方向 UV スクロールが機能する |
| **8** | **ダイナミックリップル**（CPU テクスチャ + `AddWaterRipple` API） | オブジェクト落下・雨で波紋が発生する |
| **9** | **コースティクス**（独立 PostProcess パス） | 水底・水中オブジェクトに光の屈折模様が投影される |
| **10** | WaterComponent チャンク分割 + フラスタムカリング | 広大な水域でも描画負荷が一定 |
| **11** | `WaterTool`（エディタ UI） | エディタで水位・色・波を調整できる |
| **12** | `WaterAssetSerializer`（.fbzzwater） | シーン保存・ロードで設定が維持される |

> **浮力について**: `VolumeComponent(type=Buoyancy)` + `BoxColliderComponent` の組み合わせで
> PhysicsSystem が既に処理する。WaterSystem 側の実装は不要。
> Gerstner 波対応の精度向上策は [features.md](features.md) を参照。

---

## ファイル配置

```
Projects/Engine/
  include/Engine/
    Scene/
      Components/
        WaterComponent.hpp        ← GerstnerWave + WaterComponent 定義
      Systems/
        WaterRenderSystem.hpp     ← システムシグネチャ
  src/Scene/
    Systems/
      WaterRenderSystem.cpp       ← グリッド生成・泡マスク生成・描画ロジック
    WaterAssetSerializer.cpp      ← Phase 8

Projects/Editor/
  src/Tools/
    WaterTool.hpp                 ← Phase 7
    WaterTool.cpp

Assets/Shaders/
  Water/
    Water.hlsl                    ← VS（Gerstner 波）+ PS（Fresnel・深度・泡）

Docs/System/Water/
  overview.md                     ← このファイル
  rendering.md                    ← データ構造・シェーダー・アルゴリズム詳細
```

---

## 隣接ドキュメント

- [rendering.md](rendering.md) — WaterComponent データ設計・シェーダー詳細・泡マスク生成アルゴリズム
- [features.md](features.md) — 追加機能詳細（屈折・水没 PostProcess・フローマップ・リップル・コースティクス・浮力統合）
- [../Terrain/overview.md](../Terrain/overview.md) — TerrainSystem（依存元）概要
- [../Terrain/rendering.md](../Terrain/rendering.md) — TerrainComponent の GetHeightAt インターフェース
