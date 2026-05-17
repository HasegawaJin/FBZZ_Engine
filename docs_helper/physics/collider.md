# Physics / Collider

衝突形状の基底クラスと具体実装。

---

## クラス階層

```
Collider  (基底)
├── SphereCollider
├── AABBCollider
└── CapsuleCollider
```

---

## AABB 構造体

```cpp
namespace fbzz::physics {

struct AABB {
    math::Vector3 min;
    math::Vector3 max;

    bool          Overlaps(const AABB& other) const;
    AABB          Merge(const AABB& other)    const;
    math::Vector3 Center()  const { return (min + max) * 0.5f; }
    math::Vector3 Extents() const { return (max - min) * 0.5f; }
};

} // namespace fbzz::physics
```

```cpp
bool AABB::Overlaps(const AABB& other) const {
    return (min.x <= other.max.x && max.x >= other.min.x) &&
           (min.y <= other.max.y && max.y >= other.min.y) &&
           (min.z <= other.max.z && max.z >= other.min.z);
}
```

---

## ColliderType 列挙型

`NarrowPhase` で `dynamic_cast` を使わずに形状を判別するために使う。

```cpp
namespace fbzz::physics {

enum class ColliderType { SPHERE, AABB, CAPSULE };

} // namespace fbzz::physics
```

---

## Collider 基底

```cpp
namespace fbzz::physics {

class Collider {
public:
    virtual ~Collider() = default;

    virtual AABB         GetAABB() const = 0;
    virtual ColliderType GetType() const = 0;

    // 毎フレーム World::UpdateColliders() から呼ばれる
    virtual void Update(const math::Vector3& worldPos,
                        const math::Quaternion& worldRot) = 0;

    std::shared_ptr<RigidBody> m_body;  // 紐付く剛体
};

} // namespace fbzz::physics
```

---

## SphereCollider

```cpp
class SphereCollider : public Collider {
public:
    explicit SphereCollider(float radius);

    AABB         GetAABB() const override;
    ColliderType GetType() const override { return ColliderType::SPHERE; }
    void Update(const math::Vector3& worldPos,
                const math::Quaternion& worldRot) override;

    float m_radius;

private:
    math::Vector3 m_worldCenter;
};
```

---

## AABBCollider

```cpp
class AABBCollider : public Collider {
public:
    explicit AABBCollider(const math::Vector3& halfExtents);

    AABB         GetAABB() const override;
    ColliderType GetType() const override { return ColliderType::AABB; }
    void Update(const math::Vector3& worldPos,
                const math::Quaternion& worldRot) override;

    math::Vector3 m_halfExtents;

private:
    math::Vector3 m_worldCenter;
};
```

---

## CapsuleCollider

カプセル = 2つの球 + 円柱。キャラクター当たり判定に向く。

```cpp
class CapsuleCollider : public Collider {
public:
    CapsuleCollider(float radius, float halfHeight);

    AABB         GetAABB() const override;
    ColliderType GetType() const override { return ColliderType::CAPSULE; }
    void Update(const math::Vector3& worldPos,
                const math::Quaternion& worldRot) override;

    float m_radius;
    float m_halfHeight;  // 端の球を除いた円柱部分の半高さ

private:
    math::Vector3 m_worldTop;
    math::Vector3 m_worldBottom;
};
```

AABB は `center ± (halfHeight + radius)` で計算。

---

## NarrowPhase 形状組み合わせ

| A \ B | Sphere | AABB | Capsule |
|-------|--------|------|---------|
| **Sphere** | ✅ | ✅ | ✅ |
| **AABB** | ✅ | ✅ | — |
| **Capsule** | ✅ | — | — |

`—` は Step 4 では未対応。ContactPoint を生成せずスキップする。
