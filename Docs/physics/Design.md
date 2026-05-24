# Physics モジュール設計書

`fbzz::physics` — C++20 自作物理エンジン。外部依存は `fbzz::math` のみ。

---

## 依存関係

```
fbzz_physics (static library)
│
├── 外部依存
│    └── fbzz_math  (Vector3 / Vector4 / Quaternion / Matrix4)
│
├── 内部依存順 (下が上を知る)
│    PhysicsMaterial
│    RigidBody  (Collider を所有しない。Transform + ColliderComponent から同期)
│    Collider → SphereCollider / AABBCollider / CapsuleCollider
│    ContactPoint / CollisionPair
│    Volume → ColliderVolume (Trigger Collider + VolumeComponent の内部表現)
│    Constraint → SpringConstraint / RopeConstraint / DistanceConstraint
│                 ChainConstraint / HingeConstraint
│    PhysicsSolver
│    World  ← 唯一の統合点。上記すべてを知る
│
└── engine/ が参照してよいヘッダ
     World.hpp / RigidBody.hpp / Collider 系 / ColliderVolume / Constraint 系
```

### インクルード依存表

| ヘッダ | インクルードするもの |
|--------|---------------------|
| `RigidBody.hpp` | `<math/Vector3.hpp>` `<math/Quaternion.hpp>` |
| `Collider.hpp` | `Math` |
| `SphereCollider.hpp` | `Collider.hpp` |
| `AABBCollider.hpp` | `Collider.hpp` |
| `CapsuleCollider.hpp` | `Collider.hpp` |
| `ContactPoint.hpp` | `RigidBody.hpp` |
| `CollisionPair.hpp` | `Collider.hpp` `PhysicsMaterial.hpp` |
| `Volume.hpp` | `RigidBody.hpp` |
| `ColliderVolume.hpp` | `Volume.hpp` `Collider.hpp` |
| `Constraint.hpp` | `RigidBody.hpp` |
| `[各 Constraint].hpp` | `Constraint.hpp` |
| `PhysicsSolver.hpp` | `ContactPoint.hpp` `CollisionPair.hpp` |
| `World.hpp` | 上記すべて + `<vector>` `<set>` |

---

## モジュール構成

| クラス群 | 役割 |
|---------|------|
| `World` | シミュレーション全体の管理・Step 実行 |
| `RigidBody` | 剛体の状態・力・積分。重力源フラグ・電荷を内包 |
| `Collider` 系 | 衝突形状 (Sphere / AABB / Capsule)。Scene の `ColliderComponent` が所有する |
| `ColliderVolume` | Trigger Collider + `VolumeComponent` から生成される空間効果 |
| `Constraint` 系 | バネ・ひも・剛体ロッド・鎖・ヒンジ |
| `PhysicsMaterial` | 反発・摩擦・密度のプリセット。Scene の `ColliderComponent` が値で保持 |
| `PhysicsSolver` | Broad/Narrow フェーズ衝突検出 + インパルス解決 |
| `ContactPoint` | 衝突接触点データ構造 |
| `CollisionPair` | 衝突候補ペアデータ構造 |

---

## シミュレーションループ (World::Step)

```
World::Step(dt)
│
├─ 1. RemoveExpiredVolumes()
│       IsExpired() == true の ColliderVolume を削除
│
├─ 2. Volume 適用 + 有効 dt 計算 (per body)
│       for body in m_bodies:
│         timeScale = Π GetTimeScale() of all containing Volumes
│         effectiveDt[body] = dt * timeScale
│         for vol in m_volumes where vol.Contains(body.pos):
│           vol.Apply(body, effectiveDt[body])
│
├─ 3. ApplyConstraintForces(dt)
│       SpringConstraint: F = -k*(len-rest) - b*vRel を body.force に加算
│
├─ 4. ApplyGravitationalAttraction(dt)
│       m_isGravitationalSource == true のペア全組み合わせに
│       F = G * m₁ * m₂ / r² を双方向適用
│
├─ 5. IntegrateBodies(effectiveDt per body)
│       半陰的オイラー法。m_isStatic == true はスキップ
│
├─ 6. SolveConstraintPositions(dt)
│       Rope / Distance / Hinge の位置直接補正
│       ChainConstraint は Gauss-Seidel で m_solverIterations 回反復
│
├─ 7. UpdateColliders()
│       PhysicsSystem が Transform から同期済みの Collider を使用
│
├─ 8. BroadPhase()       O(n²) AABB 重なり判定 → m_collisionPairs
├─ 9. NarrowPhase()      詳細形状判定 → ContactPoint 生成
├─ 10. Resolve()         インパルス + Baumgarte 位置補正
└─ 11. ClassifyCollisions()
          前フレームのペアセットと比較して Enter / Stay / Exit 分類
```

---

## Volume システム

空間上の領域で剛体に継続的な力・インパルス・時間スケールを与える。
Unity と同じく、Volume は GameObject を継承しない。`Transform + ColliderComponent(isTrigger=true) + VolumeComponent` の組み合わせで表現する。

| VolumeComponent type | 効果 | 形状 |
|--------|------|------|
| `Gravity` | 領域内で重力方向・強さを上書き | Trigger Collider |
| `Vortex` | 螺旋吸引 + 上昇力 | Trigger Collider |
| `Buoyancy` | 浮力 + 水中抵抗 | Trigger Collider |
| `Explosion` | 放射状インパルス、duration で寿命管理 | Trigger Collider |
| `TimeDilation` | 局所タイムスケール変更 | Trigger Collider |
| `Magnetic` | Lorentz 力 F = charge * (v × B) | Trigger Collider |

---

## Constraint システム

剛体間の距離・角度を拘束する。

| クラス | 挙動 | 解法タイミング |
|--------|------|---------------|
| `SpringConstraint` | Hooke 則バネ | Step 3 (力) |
| `RopeConstraint` | 最大距離のみ拘束 (弛緩可) | Step 6 (位置補正) |
| `DistanceConstraint` | 固定距離 (剛体ロッド) | Step 6 (位置補正) |
| `ChainConstraint` | N ノード鎖 (Gauss-Seidel 反復) | Step 6 (位置補正) |
| `HingeConstraint` | ピボット点共有 + 軸固定 | Step 6 (位置補正) |

詳細 API: [docs_helper/physics/constraint.md](../../docs_helper/physics/constraint.md)

---

## 慣性テンソル方針 (対角テンソル近似)

回転動力学には **ボディ空間の対角慣性テンソル** を使う。
`RigidBody::m_invInertiaDiag` (Vector3) に逆数を格納し、`SetMass()` / `SetInertiaFromCollider()` で自動計算。

```
// ワールド空間適用: ApplyInvInertia(v) = R * (invInertiaDiag ⊙ (Rᵀ * v))
```

- 球 → 等方的なので近似誤差なし
- AABB → 軸整合状態では正確、回転すると近似になる (Step 4 では許容)
- Capsule → 円柱で近似

衝突解決の回転インパルスも `ApplyInvInertia` を経由して計算する。
詳細: [solver.md](../../docs_helper/physics/solver.md)

---

## RigidBody の特殊プロパティ

| プロパティ | 説明 |
|-----------|------|
| `m_charge` | Magnetic VolumeComponent の Lorentz 力用。0 なら無効 |
| `m_isGravitationalSource` | true のとき他の重力源ボディと N 体引力を計算 |
| `m_gravitationalMass` | 慣性質量 (m_mass) と独立した重力質量 |
| `m_mass < 0` | 負質量。力の方向が逆になり反重力的な動きをする |

---

## ファイル構成

```
physics/
├── include/physics/
│   ├── World.hpp
│   ├── PhysicsMaterial.hpp
│   ├── RigidBody.hpp
│   ├── Collider.hpp
│   ├── SphereCollider.hpp
│   ├── AABBCollider.hpp
│   ├── CapsuleCollider.hpp
│   ├── Volume.hpp
│   ├── ColliderVolume.hpp
│   ├── Constraint.hpp
│   ├── SpringConstraint.hpp
│   ├── RopeConstraint.hpp
│   ├── DistanceConstraint.hpp
│   ├── ChainConstraint.hpp
│   ├── HingeConstraint.hpp
│   ├── ContactPoint.hpp
│   ├── CollisionPair.hpp
│   └── PhysicsSolver.hpp
└── src/
    ├── World.cpp
    ├── PhysicsMaterial.cpp
    ├── RigidBody.cpp
    ├── Collider.cpp
    ├── SphereCollider.cpp
    ├── AABBCollider.cpp
    ├── CapsuleCollider.cpp
    ├── ColliderVolume.cpp
    ├── SpringConstraint.cpp
    ├── RopeConstraint.cpp
    ├── DistanceConstraint.cpp
    ├── ChainConstraint.cpp
    ├── HingeConstraint.cpp
    └── PhysicsSolver.cpp
```

---

## 実装順

`feature/physics` ブランチを長期運用する方針のため、**Step 4 最小セット**と**後回し**に分けて管理する。

### Step 4 最小セット — 今回実装する (重力 + 衝突が動く最小構成)

依存の方向に沿って順番に実装すること。上が完成していないと下はコンパイルできない。

| # | ファイルペア | 依存 |
|---|------------|------|
| 1 | `PhysicsMaterial.hpp` / `.cpp` | なし |
| 2 | `RigidBody.hpp` / `.cpp` | `math::Vector3` / `math::Quaternion` |
| 3 | `Collider.hpp` / `.cpp` | `Math` |
| 4 | `SphereCollider.hpp` / `.cpp` | `Collider` |
| 5 | `AABBCollider.hpp` / `.cpp` | `Collider` |
| 6 | `ContactPoint.hpp` (実装なし) | `RigidBody` |
| 7 | `CollisionPair.hpp` (実装なし) | `Collider` 系 |
| 8 | `PhysicsSolver.hpp` / `.cpp` | `ContactPoint` / `CollisionPair` / `Collider` 系 |
| 9 | `World.hpp` / `.cpp` | 上記すべて |

### 後回し — feature/physics で継続開発

Step 4 マージ後、`develop` を定期的に取り込みながら追加していく。

| グループ | ファイル | 先送り理由 |
|---------|---------|-----------|
| 形状追加 | `CapsuleCollider.hpp` / `.cpp` | Sphere/AABB で動作確認後に追加 |
| Volume 系 | `Volume.hpp` / `ColliderVolume.hpp` / Scene `VolumeComponent` | Unity-style Trigger Collider 前提 |
| Constraint 系 | `Constraint.hpp` / `SpringConstraint` / `RopeConstraint` / `DistanceConstraint` / `HingeConstraint` / `ChainConstraint` | 同上 |

---

## 定数

| 定数 | 値 | 用途 |
|------|-----|------|
| `G` | `6.674e-11f` | 万有引力定数 (ゲームスケールでは拡大して使う) |
| `SLOP` | `0.01f` | Baumgarte 補正の貫通無視閾値 |
| `BAUMGARTE` | `0.8f` | Baumgarte 位置補正割合 |
| `VELOCITY_ITER` | `10` | 速度インパルスの反復回数 (PhysicsSolver::Resolve) |
| `SUBSTEPS` | `4` | World::Step 内サブステップ数 |
| `REST_THRESHOLD` | `0.5f` | 相対速度がこれ未満のとき反発係数を 0 に落とす (resting contact ジッター対策) |

---

## ソルバーの既知制限と今後の改善

### 現状の制限

現在の `PhysicsSolver` は **基本的な Sequential Impulse ソルバー** であり、以下の問題が残る。

- **積み重ねオブジェクトの沈み込み**: コンタクトが毎フレーム破棄・再生成されるため、累積インパルスが持続しない。複数コンタクト間の位置補正が干渉し、スタック安定性が低い。
- `BAUMGARTE` / `REST_THRESHOLD` / `SUBSTEPS` はこのシーンスケール向けのチューニング値であり、汎用ではない。

### 今後の改善計画 (feature/physics で対応)

**累積インパルス管理 (ウォームスタート)**を実装することで根本解決できる。

| 変更点 | 内容 |
|--------|------|
| `ContactPoint` に `accumulatedImpulse` 追加 | インパルス累積値を保持 |
| `World` がコンタクトをフレーム間で持続管理 | `m_contacts` を毎フレーム捨てない |
| BodyPair + 接触点近傍でコンタクト照合 | 前フレームの累積インパルスを引き継ぐ |
| インパルスを `[0, ∞)` にクランプ | 引き寄せ方向のインパルスを物理的に禁止 |

実装規模は 50〜100 行 + `ContactPoint` / `World` / `PhysicsSolver` の設計変更。
