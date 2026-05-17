# Physics / Volume システム

空間上の領域で剛体に力・インパルス・時間スケール変更を与える。
Volume は `World::Step` の中でのみ使用される内部システム。

---

## VolumeType 列挙型

`World::Step` が GravityVolume を識別するために使う (`dynamic_cast` の代替)。

```cpp
namespace fbzz::physics {

enum class VolumeType { GRAVITY, VORTEX, BUOYANCY, EXPLOSION, TIME_DILATION, MAGNETIC };

} // namespace fbzz::physics
```

---

## Volume 基底

```cpp
namespace fbzz::physics {

class Volume {
public:
    virtual ~Volume() = default;

    // 毎フレーム body に効果を適用 (dt は TimeDilation 適用済みの有効 dt)
    virtual void Apply(RigidBody& body, float dt) const = 0;

    // 点がこの Volume の内部にあるか
    virtual bool Contains(const math::Vector3& point) const = 0;

    // true を返すと World が次フレームに自動削除する
    virtual bool IsExpired() const { return false; }

    // TimeDilationVolume のみ 1.0f 以外を返す
    virtual float GetTimeScale() const { return 1.0f; }

    // World が GravityVolume を識別するために使う
    virtual VolumeType GetType() const = 0;

    bool m_enabled = true;
};

} // namespace fbzz::physics
```

---

## GravityVolume

領域内で重力を上書きする。グローバル重力と独立して設定可。

```cpp
class GravityVolume : public Volume {
public:
    void Apply(RigidBody& body, float dt) const override;
    bool Contains(const math::Vector3& point) const override;

    math::Vector3 m_center;
    math::Vector3 m_halfExtents;      // Box 形状
    math::Vector3 m_gravity = { 0.0f, -9.81f, 0.0f };
};
```

`Apply`: `body.ApplyForce(m_gravity * body.GetMass())` を毎フレーム加算。
World のグローバル重力はこの Volume が存在する領域では適用されない
(World 側でフラグ管理)。

---

## VortexVolume

渦・竜巻。接線方向・上昇方向・吸引方向の3成分で構成。

```cpp
class VortexVolume : public Volume {
public:
    void Apply(RigidBody& body, float dt) const override;
    bool Contains(const math::Vector3& point) const override;

    math::Vector3 m_center;
    math::Vector3 m_axis   = { 0.0f, 1.0f, 0.0f };  // 渦の上方向
    float m_radius         = 5.0f;
    float m_height         = 10.0f;
    float m_tangential     = 5.0f;   // 接線方向 (軌道速度)
    float m_uplift         = 2.0f;   // 軸方向上昇力
    float m_suction        = 3.0f;   // 軸への吸引力
};
```

```
// Apply の計算
offset    = bodyPos - center
radialDir = offset - axis * dot(offset, axis)   // 軸への投影を除いた平面成分
d         = |radialDir|

if (d < 1e-6f) {
    // 軸上にいるとき: 吸引・接線は 0、上昇力のみ
    body.ApplyForce(axis * m_uplift)
    return
}

tangentialDir = normalize(axis × radialDir)   // 接線
F_tangential  = tangentialDir * m_tangential
F_uplift      = axis * m_uplift
F_suction     = -normalize(radialDir) * m_suction / d

body.ApplyForce(F_tangential + F_uplift + F_suction)
```

---

## BuoyancyVolume

流体領域。密度差に基づく浮力と速度減衰を与える。

```cpp
class BuoyancyVolume : public Volume {
public:
    void Apply(RigidBody& body, float dt) const override;
    bool Contains(const math::Vector3& point) const override;

    math::Vector3 m_center;
    math::Vector3 m_halfExtents;
    float m_fluidDensity  = 1.0f;   // 流体密度 (水 = 1.0, 空気 ≈ 0.001)
    float m_drag          = 0.5f;   // 速度減衰係数
    float m_surfaceY      = 0.0f;   // 水面 Y 座標 (部分水没の計算用)
};
```

```
// 浮力: Archimedes
// Collider の AABB から body の底面 Y と高さを取得
AABB  aabb       = body.GetCollider()->GetAABB()
float bodyBottom = aabb.min.y
float bodyHeight = aabb.max.y - aabb.min.y
if (bodyHeight < 1e-6f) return;

submergedFraction = clamp((m_surfaceY - bodyBottom) / bodyHeight, 0.0f, 1.0f)
buoyancy = m_fluidDensity * body.GetMass() * 9.81f * submergedFraction * UP
drag     = -m_drag * body.GetVelocity()

body.ApplyForce(buoyancy + drag)
```

`body.GetCollider()` が nullptr のときは浮力を適用しない (Apply 先頭でガード)。

---

## ExplosionVolume

1 フレームだけ放射状インパルスを与えて即失効する。

```cpp
class ExplosionVolume : public Volume {
public:
    void Apply(RigidBody& body, float dt) const override;
    bool Contains(const math::Vector3& point) const override;
    bool IsExpired() const override { return m_fired; }

    math::Vector3 m_center;
    float m_radius     = 5.0f;
    float m_peakForce  = 500.0f;  // 爆心直下でのインパルス強度

private:
    mutable bool m_fired = false;
};
```

```
// Apply の計算
delta = bodyPos - m_center
dist  = |delta|

if (dist < 1e-6f) { m_fired = true; return; }  // 爆心と同座標は方向不定のためスキップ

dir = normalize(delta)
t   = max(1.0f - dist / m_radius, 0.0f)  // 距離による減衰 (0〜1)
body.ApplyImpulse(dir * m_peakForce * t * t)

m_fired = true  // 次フレームに World が削除
```

---

## TimeDilationVolume

領域内の時間スケールを変更する。`GetTimeScale()` を override し、
World が per-body の有効 dt 計算に使う。`Apply()` は何もしない。

```cpp
class TimeDilationVolume : public Volume {
public:
    void  Apply(RigidBody& /*body*/, float /*dt*/) const override {}
    bool  Contains(const math::Vector3& point) const override;
    float GetTimeScale() const override { return m_timeScale; }

    math::Vector3 m_center;
    float m_radius    = 3.0f;
    float m_timeScale = 0.2f;  // 0.0=停止, 1.0=通常, 2.0=2倍速
};
```

`m_timeScale` が複数の TimeDilationVolume を重複して受けると **乗算**される。
例: 0.5 × 0.5 = 0.25 (4 分の 1 速)。

---

## MagneticVolume

Lorentz 力: `F = charge * (v × B)`。電荷を持つ剛体のみ影響を受ける。

```cpp
class MagneticVolume : public Volume {
public:
    void Apply(RigidBody& body, float dt) const override;
    bool Contains(const math::Vector3& point) const override;

    math::Vector3 m_center;
    math::Vector3 m_halfExtents;
    math::Vector3 m_field = { 0.0f, 1.0f, 0.0f };  // 磁束密度ベクトル B
};
```

```
// Apply の計算
if (body.m_charge == 0.0f) return;

F = body.m_charge * cross(body.GetVelocity(), m_field)
body.ApplyForce(F)
```

速度と磁場の向きが同じとき力は 0。速度に直交する方向に曲がる。
