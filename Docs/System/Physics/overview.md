# Physics システム 設計 — 全体概要

## 目的

カスタム剛体シミュレーション + 衝突検出 + 拘束解決を `Projects/Physics` として独立モジュールで実装。
`PhysicsSystem.cpp` が Scene と physics::World の橋渡しを行い、依存方向を `Scene → Physics` の一方向に保つ。

---

## 実装済み機能

### RigidBody

| 機能 | 状態 |
|---|---|
| 半陰的オイラー積分 | ✅ |
| ApplyForce / ApplyForceAtPoint / ApplyImpulse / ApplyTorque | ✅ |
| Static 剛体（invMass=0） | ✅ |
| 軸ロック（FreezePosition / FreezeRotation） | ✅ |
| 対角慣性テンソル近似 + SetInertiaFromCollider | ✅ |
| CCD（高速・小型オブジェクトのトンネリング防止） | ✅ |
| 負質量（反重力挙動） | ✅ |
| N 体重力引力（m_isGravitationalSource） | ✅ |
| Lorentz 力（磁場 + 電荷） | ✅ |

### Collider 形状

| 形状 | 状態 |
|---|---|
| AABB（AabbColliderComponent） | ✅ |
| OBB / Box（BoxColliderComponent） | ✅ |
| Sphere（SphereColliderComponent） | ✅ |
| Capsule（CapsuleColliderComponent） | ✅ |
| TriangleMesh — 静的凹形状（MeshColliderComponent） | ✅ |
| ConvexHull — GJK+EPA（ConvexHullColliderComponent） | ✅ |
| Terrain 自動ビルド（TerrainComponent → TriangleMeshCollider） | ✅ |

### NarrowPhase 実装済みペア

| A \ B | Sphere | AABB | OBB | Capsule | TriangleMesh | ConvexHull |
|---|---|---|---|---|---|---|
| **Sphere** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **AABB** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **OBB** | ✅ | ✅ | ✅ | ✅ | ❌ | ✅ |
| **Capsule** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **ConvexHull** | ✅ | ✅ | ✅ | ✅ | ❌ | ✅ |

### Solver

| 機能 | 状態 |
|---|---|
| PGS（Projected Gauss-Seidel）インパルスベース解決 | ✅ |
| Warm Starting（前フレーム蓄積インパルス引き継ぎ） | ✅ |
| Baumgarte 貫通補正 | ✅ |
| 摩擦インパルス（コーン制約） | ✅ |

### 拘束（Constraint）

| 種類 | 状態 |
|---|---|
| DistanceConstraint（固定距離） | ✅ |
| SpringConstraint（バネ力） | ✅ |
| ChainConstraint（チェーン） | ✅ |
| RopeConstraint（ロープ） | ✅ |
| HingeConstraint（ヒンジ位置解決のみ） | ✅（不完全） |

### その他

| 機能 | 状態 |
|---|---|
| Trigger / OnTriggerEnter/Stay/Exit | ✅ |
| OnCollisionEnter/Stay/Exit Script コールバック | ✅ |
| Raycast / RaycastAll | ✅ |
| SphereCast / OverlapSphere（World API） | ✅（シグネチャ不備あり） |
| レイヤー衝突行列（LayerCollisionMatrix） | ✅ |
| Volume（浮力・磁場・爆発・時間スケール等） | ✅ |
| Physics Material プリセット（Default/Rubber/Ice/Metal/Wood/Stone） | ✅ |
| ContactCache + Warm Starting | ✅ |
| サブステップ（m_substeps=4 デフォルト） | ✅ |

---

## 不足している機能・バグ

### 修正済み

| 項目 | 対応内容 |
|---|---|
| `RigidBodyComponent::Reflect()` が `enabled` しか公開していない | `mass` / `isStatic` / `velocity` / `angularVelocity` / `freezePosition` / `freezeRotation` / `useGravity` / `gravityScale` / `linearDrag` / `angularDrag` / `useCCD` / `ccdRadius` / 電荷 / 重力源を Reflect に追加 |
| `linearDrag` / `angularDrag` / `useGravity` / `gravityScale` が RigidBody に存在しない | `RigidBody` に追加し、`World::ApplyForcesAndVolumes()` と `RigidBody::Integrate()` で反映 |
| 各 Collider の形状パラメータが `Reflect()` されていない | 派生 Collider で `Reflect()` をオーバーライドし、`size` / `radius` / `halfHeight` / `meshPath` / `meshIndex` / `useTransformScale` を公開 |
| Primitive Collider の保存が collider 実体依存 | SceneSerializer で Component フィールドを正として `shape` を保存 |
| `World::SphereCast` / `OverlapSphere` の radius 宣言不一致 | `World.hpp` / `World.cpp` / `ScriptPhysicsProxy` は radius + maxDistance 対応済み |
| Capsule `SphereCast` の最近ヒット選択バグ | `t = -1` 初期状態でも正のヒット距離を採用するよう修正 |
| 親子 Transform と RigidBody の書き戻し非互換 | PhysicsSystem で world pose を親ローカルへ逆変換して `localPosition` / `localRotation` に戻す |
| `ScriptPhysicsProxy` API 不足 | `GetMass` / `SetMass` / `SetStatic` / `GetAngularVelocity` / `SetAngularVelocity` / `AddForceAtPoint` / `SetFreezePosition` / `SetFreezeRotation` を追加 |
| OBB vs TriangleMesh NarrowPhase がない | OBB をローカル AABB 空間に変換し、既存 AABB-vs-Triangle SAT を再利用する `TestOBBTriangleMesh()` を追加 |
| ConvexHull vs TriangleMesh NarrowPhase がない | BVH で候補三角形を絞り、三角形を GJK サポート形状として扱う `TestConvexHullTriangleMesh()` を追加 |
| `HingeConstraint` に角度制限・モーターがない | 初期姿勢を基準角 0 とし、`SetLimits()` / `SetMotor()` / `Clear*()` を追加 |
| `FixedConstraint`（溶接拘束）がない | `FixedConstraint` を追加し、初期相対位置・相対回転を保持する |
| `SliderConstraint`（プリズマティック拘束）がない | `SliderConstraint` を追加し、1 軸移動と任意の距離制限を提供 |
| Sleeping がない | `RigidBody` に Sleep/Wake 状態を追加し、低速継続で Sleep、外力・接触で Wake する |
| Island 分割なし | `PhysicsSolver::Resolve()` で Contact graph を island 化し、独立接触群ごとに PGS を実行 |
| Physics 独自ハンドル + 差分管理への移行 | `BodyHandle` / `ColliderHandle` / `VolumeHandle` と `World::BeginSceneSync/SyncBody/SyncCollider/SyncVolume/EndSceneSync` を追加し、`PhysicsSystem` を handle touch 方式へ切り替え |

---

### 残課題（将来最適化）

| 機能 | 説明 |
|---|---|
| **互換 API の整理** | `World::SetBodies/SetColliders` は既存テスト・外部呼び出しのため残している。完全移行後に deprecated 化して削除する |

---

## アーキテクチャ

```
│ Scene Layer                                                         │
│  PhysicsSystem（Scene ↔ physics::World の橋渡し）                  │
│    1. Transform → RigidBody::SetPosition/SetRotation（書き込み）   │
│    2. ColliderComponent → ColliderInstance 構築                     │
│    3. VolumeComponent → physics::Volume 構築                        │
│    4. World::SetBodies / SetColliders / SetVolumes                  │
│    5. World::Step(dt)                                               │
│    6. RigidBody::GetPosition → Transform 書き戻し                  │
│    7. CollisionEvent → Script::OnCollision/OnTrigger コールバック   │
               ↓ 依存方向: Scene → Physics（逆依存なし）
│ Physics Layer                                                       │
│  World::Step(dt)                                                    │
│    ├ CCDPhase（高速物体のトンネリング防止）                         │
│    ├ ApplyForcesAndVolumes（重力・Volume 効果）                     │
│    ├ ApplyConstraintForces（バネ等の力ベース拘束）                  │
│    ├ IntegrateBodies（半陰的オイラー積分）                          │
│    ├ UpdateColliders（Collider をワールド空間へ変換）                │
│    ├ BroadPhase（AABB オーバーラップでペア絞り込み）                │
│    ├ NarrowPhase（形状別詳細判定 → ContactPoint 生成）              │
│    ├ Resolve（PGS インパルス解決 6 iter + Baumgarte 補正）          │
│    ├ SolveConstraintPositions（ロープ・ヒンジの位置拘束）           │
│    └ ClassifyCollisions（Enter/Stay/Exit イベント分類）             │
```

---

## 修正優先順位まとめ

| 優先度 | 項目 | 工数目安 |
|---|---|---|
| 1 | `World::SetBodies/SetColliders/SetVolumes` 互換 API の deprecated 化 | 小 |

---

## ファイル配置

```
Projects/Physics/
  include/Physics/
    RigidBody.hpp          ← 剛体状態・積分・力適用
    Collider.hpp           ← Collider 基底クラス
    PhysicsSolver.hpp      ← BroadPhase / NarrowPhase / Resolve
    BodyHandle.hpp         ← BodyHandle / ColliderHandle / VolumeHandle
    World.hpp              ← Step・Raycast・イベント管理・Scene 同期
    Constraint.hpp         ← 拘束基底
    HingeConstraint.hpp    ← ヒンジ（角度制限・モーター対応）
    FixedConstraint.hpp    ← 溶接拘束
    SliderConstraint.hpp   ← 1 軸スライダー拘束
    PhysicsMaterial.hpp    ← 反発・摩擦・密度プリセット
    Layer.hpp              ← LayerMask / LayerCollisionMatrix
    Volume.hpp             ← 空間効果 Volume 基底
  src/ ...

Projects/Engine/
  include/Engine/Scene/
    Components/
      RigidBodyComponent.hpp    ← RigidBody 設定の Reflect
      ColliderComponent.hpp     ← 派生 Collider の Reflect
    ScriptProxy/
      ScriptPhysicsProxy.hpp    ← Script 物理 API
    Systems/
      PhysicsSystem.hpp
  src/Scene/Systems/
    PhysicsSystem.cpp           ← Scene ↔ World 同期・コールバック発火

Docs/System/Physics/
  overview.md                   ← このファイル
```

---

## 設計決定：shared_ptr 廃止と Physics 独自ハンドルへの移行

### 背景

従来の `shared_ptr` 多用には以下の問題があった：

| 箇所 | 問題 |
|---|---|
| `World::SetBodies(vector<shared_ptr<RigidBody>>)` | 毎フレームの atomic refcount 操作。N体 × 60fps = 大量の atomic デクリメント |
| `CollisionPair::collider` が `shared_ptr` | NarrowPhase で大量生成される一時オブジェクトに所有権は不要 |
| `ColliderComponent::collider` が `shared_ptr` | `unique_ptr` で十分 |
| `World::m_constraints / m_volumes` が `shared_ptr` | World が単独所有するので `unique_ptr` で十分 |

また「毎フレーム全コライダーを vector ごと再構築して World に渡す」設計と合わさって、フレームごとのアロケーション・デアロケーションコストが高かった。

### 決定：Physics 独自ハンドル型を導入する

```cpp
// Physics/BodyHandle.hpp — 外部依存なし、8 bytes
namespace fbzz::physics {
    struct BodyHandle     { uint32_t slot = 0; uint32_t generation = 0; bool IsValid() const { return slot != 0; } };
    struct ColliderHandle { uint32_t slot = 0; uint32_t generation = 0; bool IsValid() const { return slot != 0; } };
    struct VolumeHandle   { uint32_t slot = 0; uint32_t generation = 0; bool IsValid() const { return slot != 0; } };
}
```

**理由：** Physics モジュールを他プロジェクトに持ち出せる自己完結モジュールとして保つため、
`renderer::ResourceHandle`（Engine 依存）は使わず Physics 独自のハンドル型を定義する。
`renderer::ResourceHandle` と構造は同じなので、将来エンジン全体でハンドルを共通層に昇格させる際は typedef に変えるだけで移行できる。

### 現在のアーキテクチャ

```
Physics モジュール（Engine 非依存を維持）
  World が BodyPool / ColliderPool を内部管理
  World::BeginSceneSync()
  World::SyncBody()      → BodyHandle
  World::SyncCollider()  → ColliderHandle
  World::SyncVolume()    → VolumeHandle
  World::EndSceneSync()  → active vector を pool から再構築
  World::Step()          → active vector を処理
  Constraint::bodyA/B = RigidBody*（変更なし）

Engine モジュール（PhysicsSystem）
  RigidBodyComponent::bodyHandle     = physics::BodyHandle
  ColliderComponent::colliderHandle  = physics::ColliderHandle
  VolumeComponent::volumeHandle      = physics::VolumeHandle
  PhysicsSystem が BeginSceneSync → Sync* → EndSceneSync で Scene と World を同期
  削除・コピーされた Component は handle の generation と touched 状態で検出
```

### 実装済み範囲

1. `Physics/BodyHandle.hpp` に `BodyHandle` / `ColliderHandle` / `VolumeHandle` を追加
2. `World` に `BeginSceneSync / SyncBody / SyncCollider / SyncVolume / EndSceneSync` を追加
3. `RigidBodyComponent` / `ColliderComponent` / `VolumeComponent` に physics handle を保持
4. `PhysicsSystem` を handle touch 方式へ切り替え

### 残る移行余地

`World::SetBodies/SetColliders/SetVolumes` は既存テストと外部コードの互換 API として残している。

---

## 隣接ドキュメント

- [../../conventions/ownership.md](../../conventions/ownership.md) — shared_ptr / 依存方向のルール
- [../Trail/overview.md](../Trail/overview.md) — 同じ Scene/Physics 境界を跨ぐ設計の参考
