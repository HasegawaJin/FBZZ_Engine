# Navigation システム 設計 — 全体概要

## 目的

NavMesh ベースのパスファインディングと AI エージェント移動を `Projects/Engine` 内のモジュールとして実装する。
`Physics` のような独立 DLL（他プロジェクトへの移植性を目的とした分離）とは異なり、
Navigation は Terrain / Collider と密結合で規模も大きくないため、`FoliageSystem` と同じ方針で
Engine 内の `Scene/Components` + `Scene/Systems` に閉じた実装とする。

ブランチ: `feature/navigation-system`

---

## スコープ

| 機能 | 内容 |
|---|---|
| NavMesh 生成 | Terrain / StaticMeshCollider からウォーカブル領域を Bake してポリゴンメッシュを構築 |
| パスファインディング | NavMesh ポリゴン上の A* 探索 + Funnel Algorithm によるパス平滑化 |
| NavMeshAgent | パスに沿った自律移動・回避・到達判定を行う Component |
| Editor 連携 | NavMesh Bake ボタン、ウォーカブル領域のデバッグ表示、Agent パスの可視化 |

将来拡張（このフェーズでは扱わない）: Off-Mesh Link（ジャンプ・梯子）、動的障害物の Carve、群衆回避（RVO/ORCA）。

---

## アーキテクチャ

`FoliageSystem` が Bake 系（`FoliageBakeSystem`）と毎フレーム系（`FoliageCullSystem` / `FoliageRenderSystem`）
を分けているのに倣い、Navigation も Bake 系と毎フレーム更新系を別 System に分割する。

```
│ NavMeshBakeSystem（Editor 操作時のみ実行）                          │
│  NavMeshVolumeComponent → Bake 入力収集                             │
│    1. TerrainComponent の高さフィールドを走査                       │
│    2. Static ColliderComponent（TriangleMesh/Box/ConvexHull）を収集 │
│    3. NavMeshObstacleComponent 付き Collider を除外領域として記録   │
│    4. Voxelize → RegionGeneration → PolygonMeshGeneration           │
│    5. 結果ポリゴンメッシュを NavMeshVolumeComponent のランタイム    │
│       キャッシュへ格納（Scene へ非保存）                            │
                                                                       │
│ NavigationSystem（毎フレーム実行）                                  │
│  NavMeshAgentComponent ごとに:                                      │
│    1. 目的地設定時のみ AStar + FunnelAlgorithm でパスを再計算       │
│    2. パスのウェイポイントへ Steering（速度・回転）を計算           │
│    3. Agent 位置を最寄りポリゴン上へ射影（NavMesh 外逸脱防止）      │
│    4. 近接 Agent との簡易回避ベクトルを加算                         │
│    5. Transform へ位置・回転を書き戻し                              │
│    6. 到達判定 → OnDestinationReached コールバック発火              │
```

---

## 実装方針

### NavMesh 生成（NavMeshBakeSystem）

| 項目 | 方針 |
|---|---|
| 入力 | `TerrainComponent`（高さフィールド） + `ColliderComponent`（Static のみ、TriangleMesh/Box/ConvexHull） |
| 除外 | `NavMeshObstacleComponent` を付けた Collider は非ウォーカブルとして除外（穴を開ける） |
| 解像度 | `NavMeshVolumeComponent::cellSize` / `cellHeight` で Bake 単位を指定 |
| 移動可能判定 | 法線と Up 軸の角度が `maxSlopeAngle` 以下、かつ `agentRadius` / `agentHeight` でクリアランス確保できる領域 |
| 出力 | 凸ポリゴンの配列 + ポリゴン間の隣接エッジ情報（Detour 方式に近い軽量実装） |
| 実行タイミング | Editor 上の明示的な Bake 操作（Runtime 自動 Bake は対象外） |

### パスファインディング（NavigationSystem 内）

| 項目 | 方針 |
|---|---|
| 探索 | ポリゴン中心間コストの A*（ヒューリスティックは直線距離） |
| パス平滑化 | Funnel Algorithm でポリゴン境界をまたぐ最短直線パスへ変換 |
| 再計算タイミング | `NavMeshAgentComponent::SetDestination()` 呼び出し時のみ。毎フレーム探索はしない |

### NavMeshAgent（Agent 移動）

| 項目 | 方針 |
|---|---|
| 移動 | パスのウェイポイントへ向け `maxSpeed` / `acceleration` で接近、到達半径内で次ウェイポイントへ |
| 回転 | 進行方向へ `angularSpeed` で補間回転 |
| 停止判定 | 最終ウェイポイントから `stoppingDistance` 以内で `OnDestinationReached` |
| NavMesh 上の補正 | Agent 位置を最寄りポリゴン上に射影し、NavMesh 外への逸脱を防止 |
| 動的障害物回避 | 同一 Volume 内の他 Agent との距離が近い場合に進行方向を簡易的に逸らす（ORCA 等の厳密解は将来拡張） |

---

## コンポーネント

| Component | 役割 |
|---|---|
| `NavMeshVolumeComponent` | Bake 範囲・解像度・スロープ角度上限などの Bake 設定を保持。Bake 結果（ポリゴンメッシュ）はランタイムキャッシュで Scene へ非保存 |
| `NavMeshObstacleComponent` | Collider を NavMesh 生成時に除外（穴を開ける）対象としてマーク |
| `NavMeshAgentComponent` | 移動速度・加速度・停止距離・半径・高さ、および現在パス（ランタイムキャッシュ）を保持 |

Physics の `BodyHandle` のような独自ハンドル層は導入しない。Navigation は Engine 内に閉じており
モジュール間の所有権分離が不要なため、`NavMeshAgentComponent` が状態を直接保持する
（`FoliageComponent` が Bake 結果を直接保持する方式と同じ）。

---

## Editor 連携

| 機能 | 内容 |
|---|---|
| Bake ボタン | `NavMeshVolumeComponent` の Inspector に配置。Bake 実行で `NavMeshBakeSystem::Bake()` を呼びランタイムキャッシュを更新 |
| デバッグ表示 | Viewport Overlay にウォーカブルポリゴンのワイヤーフレームと法線を表示（既存 [Viewport スナップオーバーレイ](../../Editor/MapEditingMode.md) と同じ Overlay 機構に相乗り） |
| パス可視化 | 選択中の `NavMeshAgentComponent` の現在パスをラインで表示 |

---

## ファイル配置（予定）

```
Projects/Engine/
  include/Engine/Scene/
    Components/
      NavMeshVolumeComponent.hpp     ← Bake 設定 + ポリゴンメッシュキャッシュ
      NavMeshObstacleComponent.hpp   ← 除外領域マーク
      NavMeshAgentComponent.hpp      ← 移動パラメータ + 現在パスキャッシュ
    Systems/
      NavMeshBakeSystem.hpp           ← Voxelize → Region → PolygonMesh
      NavigationSystem.hpp            ← A* + Funnel + Agent Steering（毎フレーム）
  src/Scene/Systems/
    NavMeshBakeSystem.cpp
    NavigationSystem.cpp

Projects/Editor/
  include/Editor/Panels/
    （既存 InspectorPanel に Bake ボタンを追加）
  src/Panels/
    ViewportPanel.cpp                 ← NavMesh / パス可視化 Overlay を追加

Docs/System/Navigation/
  overview.md                         ← このファイル
```

---

## 実装順序（提案）

| 順序 | 項目 |
|---|---|
| 1 | `NavMeshVolumeComponent` / `NavMeshObstacleComponent` + `NavMeshBakeSystem`（Voxelize → Region → PolygonMesh） |
| 2 | `NavigationSystem` 内 A* + Funnel Algorithm（パス計算のみ、移動なし） |
| 3 | `NavMeshAgentComponent` + Steering（パス追従・回転・到達判定） |
| 4 | 近接 Agent 回避（簡易版） |
| 5 | Editor: Bake ボタン・デバッグ表示・パス可視化 |

---

## 隣接ドキュメント

- [../Physics/overview.md](../Physics/overview.md) — 独自モジュール分離の判断基準（Navigationは規模的に非該当と判断）
- [../Detail/overview.md](../Detail/overview.md) / [../Foliage/overview.md](../Foliage/overview.md) — Terrain 連携・Bake/毎フレーム System 分割の参考
- [../../Editor/MapEditingMode.md](../../Editor/MapEditingMode.md) — Viewport Overlay 機構の参考
