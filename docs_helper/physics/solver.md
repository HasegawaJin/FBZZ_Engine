# Physics / PhysicsSolver

衝突検出 (Broad/Narrow フェーズ) と衝突解決 (インパルスベース)。

---

## データ構造

```cpp
namespace fbzz::physics {

struct CollisionPair {
    std::shared_ptr<Collider> a;
    std::shared_ptr<Collider> b;
};

struct ContactPoint {
    math::Vector3              point;    // 衝突点 (ワールド座標)
    math::Vector3              normal;   // b → a 方向の法線
    float                      depth;    // 貫通深度 (正の値)
    std::shared_ptr<RigidBody> bodyA;
    std::shared_ptr<RigidBody> bodyB;
};

} // namespace fbzz::physics
```

---

## PhysicsSolver クラス

```cpp
namespace fbzz::physics {

class PhysicsSolver {
public:
    // BroadPhase: 全ボディの AABB を O(n²) でチェック
    void BroadPhase(const std::vector<std::shared_ptr<RigidBody>>& bodies,
                    std::vector<CollisionPair>& outPairs);

    // NarrowPhase: 形状ごとに詳細判定 → ContactPoint 生成
    void NarrowPhase(const std::vector<CollisionPair>& pairs,
                     std::vector<ContactPoint>& outContacts);

    // Resolve: インパルス + Baumgarte 位置補正
    void Resolve(std::vector<ContactPoint>& contacts);

private:
    bool TestSphereSphere(const SphereCollider& a, const SphereCollider& b,
                          ContactPoint& out);
    bool TestAABBAABB(const AABBCollider& a, const AABBCollider& b,
                      ContactPoint& out);
    bool TestSphereAABB(const SphereCollider& s, const AABBCollider& b,
                        ContactPoint& out);
    bool TestSphereCapsule(const SphereCollider& s, const CapsuleCollider& c,
                           ContactPoint& out);

    void ResolveVelocity(const ContactPoint& cp);
    void ResolvePosition(const ContactPoint& cp);

    static constexpr float SLOP      = 0.01f;
    static constexpr float BAUMGARTE = 0.2f;
};

} // namespace fbzz::physics
```

---

## NarrowPhase 判定式

### Sphere vs Sphere

```
d = |posA - posB|
衝突: d < rA + rB
normal = (posA - posB).Normalized()
depth  = rA + rB - d
point  = posB + normal * rB
```

### AABB vs AABB (SAT 簡略版)

```
overlap_x = min(maxA.x, maxB.x) - max(minA.x, minB.x)
overlap_y = min(maxA.y, maxB.y) - max(minA.y, minB.y)
overlap_z = min(maxA.z, maxB.z) - max(minA.z, minB.z)

最小 overlap の軸 → 法線・貫通深度
```

### Sphere vs AABB

```
closest = clamp(sphereCenter, aabbMin, aabbMax)
d = |sphereCenter - closest|
衝突: d < radius
```

### Sphere vs Capsule

```
// カプセルの中心軸線分 (top, bottom) に対する球中心の最近傍点を求める
t = clamp(dot(sphereCenter - bottom, top - bottom) / |top - bottom|², 0, 1)
closest = bottom + t * (top - bottom)
d = |sphereCenter - closest|
衝突: d < radius + capsule.radius
```

---

## NarrowPhase 形状ディスパッチ

`dynamic_cast` は禁止のため `ColliderType` enum で分岐する。

```cpp
for (auto& pair : pairs) {
    ColliderType tA = pair.a->GetType();
    ColliderType tB = pair.b->GetType();
    ContactPoint cp;
    bool hit = false;

    if (tA == SPHERE && tB == SPHERE)
        hit = TestSphereSphere(...);
    else if (tA == AABB && tB == AABB)
        hit = TestAABBAABB(...);
    else if ((tA == SPHERE && tB == AABB) || (tA == AABB && tB == SPHERE))
        hit = TestSphereAABB(...);  // 順序を正規化して呼ぶ
    else if ((tA == SPHERE && tB == CAPSULE) || (tA == CAPSULE && tB == SPHERE))
        hit = TestSphereCapsule(...);
    // 未対応の組み合わせはスキップ

    if (hit) outContacts.push_back(cp);
}
```

---

## Resolve: インパルスベース (回転込み)

```
// 両ボディが static のとき (invMass 合計 = 0) はスキップ
if (invMassA + invMassB + angTermA + angTermB == 0.0f) return;

// 接触点から重心へのオフセット
rA = contact.point - bodyA.GetPosition()
rB = contact.point - bodyB.GetPosition()

// 接触点での相対速度 (回転成分を含む)
vA_contact = vA + cross(ωA, rA)
vB_contact = vB + cross(ωB, rB)
vRel  = vA_contact - vB_contact
vRelN = dot(vRel, normal)

// 離れているなら解決不要
if (vRelN > 0) return;

e = PhysicsMaterial::CombineRestitution(bodyA.m_material, bodyB.m_material)

// 分母に回転の寄与 (慣性テンソル経由) を加算
// bodyA.ApplyInvInertia(v) = R * (invInertiaDiag ⊙ (Rᵀ * v))
angTermA = dot(cross(bodyA.ApplyInvInertia(cross(rA, normal)), rA), normal)
angTermB = dot(cross(bodyB.ApplyInvInertia(cross(rB, normal)), rB), normal)

j = -(1 + e) * vRelN / (invMassA + invMassB + angTermA + angTermB)

// 線形速度を更新
vA += j * invMassA * normal
vB -= j * invMassB * normal

// 角速度を更新
ωA += bodyA.ApplyInvInertia(cross(rA, j * normal))
ωB -= bodyB.ApplyInvInertia(cross(rB, j * normal))
```

### Baumgarte 位置補正

線形のみ。回転補正は計算コストに対して効果が薄いため省略。

```
invMassSum = invMassA + invMassB
correction = max(depth - SLOP, 0) / invMassSum * BAUMGARTE * normal

posA += invMassA * correction
posB -= invMassB * correction
```

| 定数 | 値 | 意味 |
|------|-----|------|
| `SLOP` | `0.01f` | 小さな貫通を無視する閾値 |
| `BAUMGARTE` | `0.2f` | 位置補正の割合 (大きいと振動しやすい) |
