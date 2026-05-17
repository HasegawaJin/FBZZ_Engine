# Physics / RigidBody

`fbzz::physics::RigidBody` — 剛体の状態・力・積分を管理する。

---

## クラス定義

```cpp
namespace fbzz::physics {

class RigidBody {
public:
    // 力・インパルスの適用
    void ApplyForce(const math::Vector3& force);
    void ApplyForceAtPoint(const math::Vector3& force, const math::Vector3& worldPoint);
    void ApplyImpulse(const math::Vector3& impulse);
    void ApplyTorque(const math::Vector3& torque);

    // 積分 (半陰的オイラー法、World::Step から呼ばれる)
    void Integrate(float dt);

    // 質量管理
    void  SetMass(float mass);         // 負値可 (反重力挙動)
    float GetMass()        const { return m_mass;    }
    float GetInverseMass() const { return m_invMass; }
    bool  IsStatic()       const { return m_isStatic; }

    // 状態アクセス
    math::Vector3    GetPosition()        const { return m_position;        }
    math::Vector3    GetVelocity()        const { return m_velocity;        }
    math::Quaternion GetRotation()        const { return m_rotation;        }
    math::Vector3    GetAngularVelocity() const { return m_angularVelocity; }

    void SetPosition(const math::Vector3& pos);
    void SetVelocity(const math::Vector3& vel);
    void SetRotation(const math::Quaternion& rot);

    // 物理マテリアル (反発・摩擦・密度)
    // 詳細: docs_helper/physics/material.md
    PhysicsMaterial m_material;

    // 動作制御
    bool  m_isStatic    = false;  // true のとき積分・衝突解決をスキップ

    // Collider 紐付け
    void SetCollider(std::shared_ptr<Collider> collider);
    std::shared_ptr<Collider> GetCollider() const { return m_collider; }

    // --- 拡張プロパティ ---

    // MagneticVolume 用: Lorentz 力 F = m_charge * (v × B)
    // 0 のとき磁場の影響を受けない
    float m_charge = 0.0f;

    // N 体重力引力用: World::ApplyGravitationalAttraction で使用
    // true のとき同フラグを持つ他ボディと F = G*m₁*m₂/r² を双方向で受ける
    bool  m_isGravitationalSource = false;
    float m_gravitationalMass     = 1.0f;  // 慣性質量 m_mass と独立して設定可

    // engine 側コンポーネントへのポインタ (衝突コールバック用)
    void* m_userData = nullptr;

private:
    math::Vector3    m_position;
    math::Vector3    m_velocity;
    math::Vector3    m_force;
    math::Quaternion m_rotation;
    math::Vector3    m_angularVelocity;
    math::Vector3    m_torque;

    float m_mass    = 1.0f;
    float m_invMass = 1.0f;  // m_isStatic == true のとき 0
                              // m_mass < 0 のとき負になる (反重力挙動)

    // 対角慣性テンソルの逆数 (ボディ空間)
    // SetMass() / SetCollider() 呼び出し時に自動再計算される
    // m_isStatic == true のとき {0,0,0}
    math::Vector3 m_invInertiaDiag = { 1.0f, 1.0f, 1.0f };

    std::shared_ptr<Collider> m_collider;

    void RecomputeInertia();  // SetMass / SetCollider から呼ばれる

public:
    // ワールド空間で I⁻¹ * v を計算する
    // = R * (m_invInertiaDiag ⊙ (Rᵀ * v))
    // PhysicsSolver::Resolve から使用するため public
    math::Vector3 ApplyInvInertia(const math::Vector3& v) const;
};

} // namespace fbzz::physics
```

---

## 積分法: 半陰的オイラー法

### 線形

```
a = (m_force + gravity) / m_mass

v(t+dt) = v(t) + a * dt          // 速度を先に更新
x(t+dt) = x(t) + v(t+dt) * dt   // 更新後の v を使う ← 半陰的
```

### 回転

```
// 1. ワールドトルクをボディ空間に変換
τ_body = m_rotation.Conjugate().RotateVector(m_torque)

// 2. ボディ空間で角加速度を計算 (対角テンソルなので成分ごと積)
α_body = { m_invInertiaDiag.x * τ_body.x,
           m_invInertiaDiag.y * τ_body.y,
           m_invInertiaDiag.z * τ_body.z }

// 3. ワールド空間に戻す
α_world = m_rotation.RotateVector(α_body)

// 4. 角速度を更新 (半陰的)
ω(t+dt) = ω(t) + α_world * dt

// 5. クォータニオン積分
spin = Quaternion(0, ω(t+dt)) * m_rotation
m_rotation = (m_rotation + spin * 0.5 * dt).Normalized()

// 6. 力・トルクをリセット (毎フレーム蓄積させない)
m_force  = Vector3::ZERO
m_torque = Vector3::ZERO
```

`.Normalized()` は数値誤差によるクォータニオンのドリフトを防ぐため毎フレーム必須。

---

## 慣性テンソルの自動計算

`SetMass()` または `SetCollider()` 呼び出し時に `RecomputeInertia()` が走る。
両方設定済みの場合のみ計算する (どちらかが未設定なら {1,1,1} のまま)。

| Collider 形状 | invInertiaDiag の計算式 |
|--------------|------------------------|
| `SphereCollider` (r) | `{ 5/(2mr²), 5/(2mr²), 5/(2mr²) }` |
| `AABBCollider` (hx,hy,hz) | `{ 3/(m(hy²+hz²)), 3/(m(hx²+hz²)), 3/(m(hx²+hy²)) }` |
| `CapsuleCollider` (r,h) | 円柱近似: `{ 12/(m(3r²+4h²)), 2/(mr²), 12/(m(3r²+4h²)) }` |

`m_isStatic == true` のとき `m_invInertiaDiag = {0,0,0}` とし回転を固定する。

---

## 負質量 (反重力)

`SetMass(-1.0f)` のように負の値を渡すと `m_invMass` も負になる。

- 重力 (F=mg 下向き) に対して **上向き**に加速する
- 押しつけると逃げていく (衝突解決のインパルスも逆向きに作用)
- `m_isStatic = false` のまま使う

---

## 静的剛体

`m_isStatic = true` にすると `m_invMass = 0` とし、積分・衝突インパルスをスキップ。
地面・壁などに使う。Collider は持てる (衝突判定の対象になる)。
