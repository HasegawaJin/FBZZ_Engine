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

### 重要度：高

#### `RigidBodyComponent::Reflect()` が `enabled` しか公開していない

Inspector / SceneSerializer から以下のパラメータが一切触れない：

| 未公開フィールド | 型 |
|---|---|
| `mass` | float |
| `isStatic` | bool |
| `linearDrag` / `angularDrag` | float |
| `useGravity` / `gravityScale` | bool / float |
| `useCCD` / `ccdRadius` | bool / float |
| `freezePosition` / `freezeRotation` | AxisLock |

**修正方針：** `RigidBodyComponent::Reflect()` に上記フィールドを追加し、
Inspector の編集値を `rigidBody->SetMass()` 等に反映するアダプタ処理を追加する。

---

#### 各 Collider の形状パラメータが `Reflect()` されていない

派生コンポーネントに `Reflect()` オーバーライドがなく、形状をエディタで編集・シーン保存できない。

| コンポーネント | 未公開フィールド |
|---|---|
| `AabbColliderComponent` / `BoxColliderComponent` | `size` |
| `SphereColliderComponent` | `radius` |
| `CapsuleColliderComponent` | `radius`, `halfHeight` |
| `MeshColliderComponent` / `ConvexHullColliderComponent` | `meshPath`, `meshIndex`, `useTransformScale` |

**修正方針：** 各派生コンポーネントで `Reflect()` をオーバーライドし、
`ColliderComponent::Reflect(r)` を呼んだ上で形状固有フィールドを追加 Reflect する。

---

#### OBB vs TriangleMesh / ConvexHull vs TriangleMesh の NarrowPhase がない

`TestOBBTriangleMesh` と `TestConvexHullTriangleMesh` が未実装。
`BoxColliderComponent` や `ConvexHullColliderComponent` が Terrain や静的メッシュコライダーと衝突できない。

**修正方針：**
- OBB vs Triangle: SAT（15 軸テスト）で実装（他の OBB テストと同系統）
- ConvexHull vs Triangle: 単三角形を ConvexHullCollider として扱い GJK+EPA を再利用

---

#### `World::SphereCast` / `OverlapSphere` に `radius` パラメータがない

`World.hpp` の宣言：
```cpp
bool SphereCast(const math::Vector3& origin,
                const math::Vector3& direction,
                ColliderFilter        filter = nullptr) const;

std::vector<const ColliderInstance*> OverlapSphere(
                const math::Vector3& center,
                ColliderFilter        filter = nullptr) const;
```

`ScriptPhysicsProxy` では `radius` と `dist` を受け取るが World API に渡す口がなく機能しない。

**修正方針：** `World::SphereCast(origin, direction, radius, maxDist, hit, filter)` 、
`World::OverlapSphere(center, radius, filter)` にシグネチャを変更し、
`ScriptPhysicsProxy` の実装も合わせて修正する。

---

#### 親子 Transform と RigidBody が非互換

`PhysicsSystem.cpp` の書き戻しがワールド座標を `localPosition` に直書きしている：

```cpp
// PhysicsSystem.cpp — 問題箇所
tf.localPosition = rb.rigidBody->GetPosition();  // worldPos を local に直書き
tf.position      = tf.localPosition;
tf.rotation      = tf.localRotation;
```

親 GameObject がいると parent transform の影響が無視されて座標がずれる。

**修正方針：** 書き戻し時に親の逆変換を適用してワールド座標を local に変換するか、
「RigidBody を持つ GameObject は親を持たない」ルールをドキュメント化して制約として明示する。

---

### 重要度：中

#### `HingeConstraint` に角度制限・モーターがない

`SolvePosition()` でアンカー位置を合わせるのみ。`minAngle` / `maxAngle` による可動域制限と
角速度ドライブ（モーター）がない。ドア・関節・歯車が作れない。

**修正方針：** `SolvePosition()` で角度を計算し、`[minAngle, maxAngle]` の範囲外なら
インパルスでクランプする。モーターは `ApplyForce()` で角速度目標を追う力を適用する。

---

#### `FixedConstraint`（溶接拘束）がない

2 つの剛体を相対姿勢ごと完全固定する拘束がない。
物理的に接続したい複合オブジェクト（車体+タイヤ等）の組み立てができない。

---

#### `SliderConstraint`（プリズマティック拘束）がない

1 軸方向のみ移動を許す拘束がない。引き出し・ピストン・スライドドアが作れない。

---

#### `ScriptPhysicsProxy` に不足 API

| 不足メソッド | 用途 |
|---|---|
| `GetMass()` / `SetMass(float)` | 実行時の質量変更（溶岩に落ちたら重くなる等） |
| `SetStatic(bool)` | 動的 → 静的の切り替え |
| `GetAngularVelocity()` / `SetAngularVelocity()` | 回転速度の読み書き |
| `AddForceAtPoint(force, worldPoint)` | 非中心力によるトルク付与 |
| `SetFreezePosition/Rotation(x,y,z)` | スクリプトからの軸ロック |

---

### 重要度：低（パフォーマンス・将来対応）

| 機能 | 説明 |
|---|---|
| **Sleeping 未実装** | 静止した剛体も毎フレーム積分・衝突解決を実行する。多数の静止オブジェクトで無駄なコストが発生 |
| **Island 分割なし** | 衝突グラフを独立グループに分けず全体を一括 PGS 解決。互いに無関係な剛体まで同一イテレーションに混在 |
| **毎フレーム全コライダー再構築** | `bodies`/`colliders`/`volumes` を毎フレーム `vector` 丸ごと再作成して `World::SetBodies/SetColliders` に渡す。差分管理なし |

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
| 1 | 各 Collider の `Reflect()` 追加 | 小（各コンポーネントに数行追加） |
| 2 | `RigidBodyComponent::Reflect()` 拡充 | 小（フィールド追加 + rigidBody への反映） |
| 3 | `SphereCast` / `OverlapSphere` の radius 修正 | 小（シグネチャ変更 + 実装修正） |
| 4 | OBB vs TriangleMesh NarrowPhase 追加 | 中（SAT 実装） |
| 5 | ConvexHull vs TriangleMesh NarrowPhase 追加 | 中（GJK 再利用） |
| 6 | `ScriptPhysicsProxy` API 追加 | 小 |
| 7 | 親子 Transform / RigidBody 非互換の解決または明文化 | 中〜大 |
| 8 | HingeConstraint 角度制限・モーター | 中 |
| 9 | FixedConstraint / SliderConstraint 追加 | 中 |
| 10 | Sleeping / Island 分割 | 大 |

---

## ファイル配置

```
Projects/Physics/
  include/Physics/
    RigidBody.hpp          ← 剛体状態・積分・力適用
    Collider.hpp           ← Collider 基底クラス
    PhysicsSolver.hpp      ← BroadPhase / NarrowPhase / Resolve
    World.hpp              ← Step・Raycast・イベント管理
    Constraint.hpp         ← 拘束基底
    HingeConstraint.hpp    ← ヒンジ（角度制限・モーター 未実装）
    PhysicsMaterial.hpp    ← 反発・摩擦・密度プリセット
    Layer.hpp              ← LayerMask / LayerCollisionMatrix
    Volume.hpp             ← 空間効果 Volume 基底
  src/ ...

Projects/Engine/
  include/Engine/Scene/
    Components/
      RigidBodyComponent.hpp    ← Reflect() 要拡充
      ColliderComponent.hpp     ← 派生コンポーネントの Reflect() 要追加
    ScriptProxy/
      ScriptPhysicsProxy.hpp    ← API 要追加
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

現在の `shared_ptr` 多用には以下の問題がある：

| 箇所 | 問題 |
|---|---|
| `World::SetBodies(vector<shared_ptr<RigidBody>>)` | 毎フレームの atomic refcount 操作。N体 × 60fps = 大量の atomic デクリメント |
| `CollisionPair::collider` が `shared_ptr` | NarrowPhase で大量生成される一時オブジェクトに所有権は不要 |
| `ColliderComponent::collider` が `shared_ptr` | `unique_ptr` で十分 |
| `World::m_constraints / m_volumes` が `shared_ptr` | World が単独所有するので `unique_ptr` で十分 |

また「毎フレーム全コライダーを vector ごと再構築して World に渡す」設計と合わさって、フレームごとのアロケーション・デアロケーションコストが高い。

### 決定：Physics 独自ハンドル型を導入する

```cpp
// Physics/BodyHandle.hpp — 外部依存なし、8 bytes
namespace fbzz::physics {
    struct BodyHandle     { uint32_t slot = 0; uint32_t gen = 0; bool IsValid() const { return slot != 0; } };
    struct ColliderHandle { uint32_t slot = 0; uint32_t gen = 0; bool IsValid() const { return slot != 0; } };
}
```

**理由：** Physics モジュールを他プロジェクトに持ち出せる自己完結モジュールとして保つため、
`renderer::ResourceHandle`（Engine 依存）は使わず Physics 独自のハンドル型を定義する。
`renderer::ResourceHandle` と構造は同じなので、将来エンジン全体でハンドルを共通層に昇格させる際は typedef に変えるだけで移行できる。

### 目標アーキテクチャ

```
Physics モジュール（Engine 非依存を維持）
  World が BodyPool / ColliderPool を内部管理
  World::AddBody()     → BodyHandle
  World::RemoveBody(BodyHandle)
  World::Step()        → pool 内の全 body を直接処理（vector の再構築不要）
  CollisionPair::collider = const Collider*（非所有 raw pointer）
  Constraint::bodyA/B = RigidBody*（変更なし）

Engine モジュール（PhysicsSystem）
  RigidBodyComponent::bodyHandle     = physics::BodyHandle
  ColliderComponent::colliderHandle  = physics::ColliderHandle
  PhysicsSystem が AddBody / RemoveBody を差分管理で呼ぶ
  毎フレームは Transform 同期のみ（vector 再構築なし）
```

### 移行ステップ

1. `Physics/BodyHandle.hpp` / `Physics/ColliderHandle.hpp` を追加
2. `World` に `AddBody / RemoveBody / AddCollider / RemoveCollider` API を追加し、内部 Pool を実装
3. `CollisionPair::collider` を `const Collider*` に変更
4. `World::m_constraints` / `m_volumes` を `unique_ptr` に変更
5. `RigidBodyComponent` / `ColliderComponent` をハンドルベースに変更
6. `PhysicsSystem` を差分管理（GameObject の追加・削除時のみ World API を呼ぶ）に書き換え

---

## 隣接ドキュメント

- [../../conventions/ownership.md](../../conventions/ownership.md) — shared_ptr / 依存方向のルール
- [../Trail/overview.md](../Trail/overview.md) — 同じ Scene/Physics 境界を跨ぐ設計の参考
