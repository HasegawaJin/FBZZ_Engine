# Physics / RigidBody

`fbzz::physics::RigidBody`。位置・速度・質量・回転を持つ剛体。

---

## クラス定義

```cpp
namespace fbzz::physics {

class RigidBody {
public:
    // 力・インパルスの適用
    void ApplyForce(const math::Vector3& force);
    void ApplyForceAtPoint(const math::Vector3& force, const math::Vector3& point);
    void ApplyImpulse(const math::Vector3& impulse);
    void ApplyTorque(const math::Vector3& torque);

    // 積分 (半陰的オイラー法)
    void Integrate(float dt);

    // 質量管理
    void  SetMass(float mass);
    float GetMass()        const { return m_mass; }
    float GetInverseMass() const { return m_invMass; }
    bool  IsStatic()       const { return m_isStatic; }

    // 状態アクセス
    math::Vector3       GetPosition()        const { return m_position; }
    math::Vector3       GetVelocity()        const { return m_velocity; }
    math::Quaternion GetRotation()        const { return m_rotation; }
    math::Vector3       GetAngularVelocity() const { return m_angularVelocity; }

    void SetPosition(const math::Vector3& pos);
    void SetVelocity(const math::Vector3& vel);
    void SetRotation(const math::Quaternion& rot);

    // 物理マテリアル
    float m_restitution = 0.3f;   // 反発係数 (0=完全非弾性, 1=完全弾性)
    float m_friction    = 0.5f;   // 摩擦係数

    bool  m_isStatic    = false;  // true のとき積分をスキップ

private:
    math::Vector3       m_position;
    math::Vector3       m_velocity;
    math::Vector3       m_force;
    math::Quaternion m_rotation;
    math::Vector3       m_angularVelocity;
    math::Vector3       m_torque;

    float m_mass    = 1.0f;
    float m_invMass = 1.0f;   // m_isStatic のとき 0
};

} // namespace fbzz::physics
```

---

## 積分法: 半陰的オイラー法

速度を先に更新し、その速度で位置を更新することで数値安定性を上げる。

```
// 力 → 加速度
a = F / m + gravity

// 半陰的オイラー法
v(t+dt) = v(t) + a * dt
x(t+dt) = x(t) + v(t+dt) * dt   ← 更新後の v を使う
```

通常のオイラー法 (`x += v * dt` を先に計算) よりエネルギーが発散しにくい。

---

## 静的剛体

`m_isStatic = true` にすると `m_invMass = 0` とし、積分をスキップする。
地面・壁など動かない物体に使う。衝突解決時も動かない。

---

## 使用例

```cpp
auto body = std::make_shared<RigidBody>();
body->SetMass(1.0f);
body->SetPosition({ 0.0f, 10.0f, 0.0f });
body->ApplyForce({ 0.0f, 0.0f, 5.0f });

world.AddBody(body);
```

---

## ファイル構成

```
physics/
├── include/physics/
│   └── RigidBody.hpp
└── src/
    └── RigidBody.cpp
```

---

## 参考ドキュメント

- [半陰的オイラー法 (Wikipedia)](https://en.wikipedia.org/wiki/Semi-implicit_Euler_method) — 積分法の数値安定性の説明
- [Symplectic Euler (Wikipedia)](https://en.wikipedia.org/wiki/Symplectic_integrator) — 半陰的オイラー法の別名・より詳しい文脈
