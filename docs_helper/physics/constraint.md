# Physics / Constraint システム

剛体間の距離・角度・ピボットを拘束する。

---

## Constraint 基底

```cpp
namespace fbzz::physics {

class Constraint {
public:
    virtual ~Constraint() = default;

    // Step 3: 力として加算 (SpringConstraint が使用)
    virtual void ApplyForces(float dt) = 0;

    // Step 6: 積分後に位置を直接補正 (Rope / Distance / Hinge / Chain が使用)
    virtual void SolvePositions(float dt) = 0;

    bool m_enabled = true;
};

} // namespace fbzz::physics
```

---

## SpringConstraint

Hooke 則バネ。引っ張り・圧縮どちらも力を発生させる。

```cpp
class SpringConstraint : public Constraint {
public:
    void ApplyForces(float dt) override;
    void SolvePositions(float /*dt*/) override {}

    std::shared_ptr<RigidBody> m_bodyA;
    std::shared_ptr<RigidBody> m_bodyB;    // nullptr のとき m_anchorWorld を固定点として使う
    math::Vector3              m_anchorWorld;

    float m_restLength = 1.0f;
    float m_stiffness  = 10.0f;   // k (バネ定数)
    float m_damping    = 0.5f;    // b (減衰係数)
};
```

```
// ApplyForces の計算
posA = bodyA.GetPosition()
posB = (bodyB != nullptr) ? bodyB.GetPosition() : anchorWorld
dir  = posB - posA
len  = |dir|

if (len < 1e-6f) return;  // 同座標のとき normalize が NaN になるのを防ぐ

stretch  = len - m_restLength
vRel     = (bodyB != nullptr) ? bodyB.GetVelocity() - bodyA.GetVelocity() : -bodyA.GetVelocity()
vDamp    = dot(vRel, normalize(dir))

F = (m_stiffness * stretch + m_damping * vDamp) * normalize(dir)
bodyA.ApplyForce( F)
if (bodyB != nullptr) bodyB.ApplyForce(-F)
```

---

## RopeConstraint

最大距離のみ拘束。弛緩中は何もしない。

```cpp
class RopeConstraint : public Constraint {
public:
    void ApplyForces(float /*dt*/) override {}
    void SolvePositions(float dt) override;

    std::shared_ptr<RigidBody> m_bodyA;
    std::shared_ptr<RigidBody> m_bodyB;
    math::Vector3              m_anchorWorld;  // bodyB == nullptr のとき使用

    float m_maxLength = 1.0f;
};
```

```
// SolvePositions の計算
posA = bodyA.GetPosition()
posB = (bodyB != nullptr) ? bodyB.GetPosition() : anchorWorld
dir  = posA - posB
dist = |dir|

if (dist <= m_maxLength) return;  // 弛緩中はスキップ

invMassB   = (bodyB != nullptr) ? bodyB.GetInverseMass() : 0.0f  // anchor は infinite mass
invMassSum = bodyA.GetInverseMass() + invMassB
if (invMassSum == 0.0f) return;  // 両方 static なら補正不可

excess     = dist - m_maxLength
correction = normalize(dir) * (excess / invMassSum)

bodyA.SetPosition(posA - bodyA.GetInverseMass() * correction)
if (bodyB != nullptr) bodyB.SetPosition(posB + invMassB * correction)
```

---

## DistanceConstraint

固定距離拘束 (剛体ロッド)。引っ張り・圧縮どちらも補正。

```cpp
class DistanceConstraint : public Constraint {
public:
    void ApplyForces(float /*dt*/) override {}
    void SolvePositions(float dt) override;

    std::shared_ptr<RigidBody> m_bodyA;
    std::shared_ptr<RigidBody> m_bodyB;

    float m_distance = 1.0f;
};
```

RopeConstraint と同じ補正式だが `if (dist <= m_distance) return;` を削除し
両方向で補正する。

---

## ChainConstraint

N ノード鎖。各隣接ペアを DistanceConstraint で繋ぎ Gauss-Seidel で反復解く。

```cpp
class ChainConstraint : public Constraint {
public:
    void ApplyForces(float /*dt*/) override {}
    void SolvePositions(float dt) override;  // m_solverIterations 回反復

    // m_links は World にも AddBody() 済みであること
    std::vector<std::shared_ptr<RigidBody>> m_links;
    float m_linkLength       = 0.5f;
    int   m_solverIterations = 5;
};
```

```
// SolvePositions
if (m_links.size() < 2) return;  // size_t アンダーフロー防止

for (int iter = 0; iter < m_solverIterations; ++iter) {
    for (size_t i = 0; i < m_links.size() - 1; ++i) {
        // links[i] と links[i+1] の間で DistanceConstraint 相当の補正
        SolvePair(*m_links[i], *m_links[i+1], m_linkLength);
    }
}
```

先頭ノード (`m_links[0]`) を `m_isStatic = true` にすると天井から吊り下げた鎖になる。

---

## HingeConstraint

2 剛体をピボット点で接続し、1 軸のみの回転を許す。

```cpp
class HingeConstraint : public Constraint {
public:
    void ApplyForces(float /*dt*/) override {}
    void SolvePositions(float dt) override;

    std::shared_ptr<RigidBody> m_bodyA;
    std::shared_ptr<RigidBody> m_bodyB;  // nullptr のときワールド固定

    math::Vector3 m_pivotA;   // ボディ A ローカル座標でのピボット点
    math::Vector3 m_pivotB;   // ボディ B ローカル座標でのピボット点
    math::Vector3 m_axisA;    // ボディ A ローカル座標でのヒンジ軸
};
```

```
// SolvePositions (位置拘束のみ、角度軸拘束は近似)
worldPivotA = bodyA.GetRotation().RotateVector(m_pivotA) + bodyA.GetPosition()
worldPivotB = (bodyB != nullptr)
              ? bodyB.GetRotation().RotateVector(m_pivotB) + bodyB.GetPosition()
              : m_pivotB  // bodyB が null のとき m_pivotB をワールド座標の固定点として扱う

delta      = worldPivotA - worldPivotB
invMassB   = (bodyB != nullptr) ? bodyB.GetInverseMass() : 0.0f
invMassSum = bodyA.GetInverseMass() + invMassB
if (invMassSum == 0.0f) return;

correction = delta / invMassSum
bodyA.SetPosition(bodyA.GetPosition() - bodyA.GetInverseMass() * correction)
if (bodyB != nullptr) bodyB.SetPosition(bodyB.GetPosition() + invMassB * correction)
```
