# Physics / Solver

衝突検出 (Broad / Narrow フェーズ) と衝突解決 (インパルスベース) の設計。

---

## データ構造

```cpp
namespace fbzz::physics {

// BroadPhase で生成
struct CollisionPair {
    std::shared_ptr<Collider> a;
    std::shared_ptr<Collider> b;
};

// NarrowPhase で生成
struct ContactPoint {
    math::Vector3 point;       // 衝突点 (ワールド座標)
    math::Vector3 normal;      // b から a を向く法線
    float      depth;       // 貫通深度 (正の値)
    std::shared_ptr<RigidBody> bodyA;
    std::shared_ptr<RigidBody> bodyB;
};

} // namespace fbzz::physics
```

---

## BroadPhase

全ボディの AABB をチェックし、重なる可能性があるペアを収集する。
O(n²) だが、まずはシンプルに実装する。将来は空間分割 (BVH / グリッド) に移行できる。

```cpp
void World::BroadPhase() {
    m_collisionPairs.clear();

    for (size_t i = 0; i < m_bodies.size(); ++i) {
        for (size_t j = i + 1; j < m_bodies.size(); ++j) {
            AABB a = m_bodies[i]->GetCollider()->GetAABB();
            AABB b = m_bodies[j]->GetCollider()->GetAABB();
            if (a.Overlaps(b)) {
                m_collisionPairs.push_back({ ... });
            }
        }
    }
}
```

---

## NarrowPhase

CollisionPair の形状の組み合わせに応じて詳細判定を行い `ContactPoint` を生成する。

### Sphere vs Sphere

```
d = |posA - posB|
penetration = rA + rB - d

d < rA + rB なら衝突
normal = (posA - posB).Normalized()
point  = posB + normal * rB
depth  = penetration
```

### AABB vs AABB

各軸の overlap を計算し、最小 overlap の軸を法線とする (SAT の簡略版)。

```
overlap_x = min(maxA.x, maxB.x) - max(minA.x, minB.x)
overlap_y = min(maxA.y, maxB.y) - max(minA.y, minB.y)
overlap_z = min(maxA.z, maxB.z) - max(minA.z, minB.z)

最小 overlap の軸 → 法線・貫通深度
```

### Sphere vs AABB

球の中心から AABB 上の最近傍点を求め、距離と半径を比較する。

```
closest = clamp(sphereCenter, aabbMin, aabbMax)
d = |sphereCenter - closest|
衝突: d < sphere.radius
```

---

## Resolve (インパルスベース)

ContactPoint の法線方向に沿ったインパルスを計算し、両剛体の速度を修正する。

### 相対速度の計算

```
vRel = vA - vB  (法線方向成分)
vRelN = dot(vRel, normal)

vRelN > 0 なら離れているので解決不要
```

### インパルス量

```
e = min(bodyA.restitution, bodyB.restitution)

j = -(1 + e) * vRelN
    / (invMassA + invMassB)

vA += j * invMassA * normal
vB -= j * invMassB * normal
```

### 位置補正 (スリップ補正)

インパルスだけでは貫通が残ることがあるため、位置を直接補正する。

```
correction = max(depth - SLOP, 0) / (invMassA + invMassB) * BAUMGARTE * normal

posA += invMassA * correction
posB -= invMassB * correction
```

| 定数 | 値 | 意味 |
|------|-----|------|
| `SLOP` | 0.01f | 小さな貫通を無視する閾値 |
| `BAUMGARTE` | 0.2f | 補正の割合 (大きいと振動しやすい) |

---

## PhysicsSolver クラス

```cpp
namespace fbzz::physics {

class PhysicsSolver {
public:
    void Resolve(const std::vector<ContactPoint>& contacts);

private:
    void ResolveVelocity(const ContactPoint& contact);
    void ResolvePosition(const ContactPoint& contact);

    static constexpr float SLOP       = 0.01f;
    static constexpr float BAUMGARTE  = 0.2f;
};

} // namespace fbzz::physics
```

---

## ファイル構成

```
physics/
├── include/physics/
│   ├── CollisionPair.hpp
│   ├── ContactPoint.hpp
│   └── PhysicsSolver.hpp
└── src/
    └── PhysicsSolver.cpp
```

---

## 参考ドキュメント

- [Impulse-based collision response (Wikipedia)](https://en.wikipedia.org/wiki/Collision_response) — インパルス量の導出式
- [Baumgarte stabilization (Wikipedia)](https://en.wikipedia.org/wiki/Baumgarte_stabilization_method) — 位置補正 (SLOP / BAUMGARTE 定数の意味)
- [Separating Axis Theorem (Wikipedia)](https://en.wikipedia.org/wiki/Hyperplane_separation_theorem) — SAT の数学的根拠 (NarrowPhase の基礎)
- [Randy Gaul の衝突検出シリーズ](https://gdcvault.com/browse/gdc-13) — GDC 物理実装解説 (検索: "Game Physics" Randy Gaul)
