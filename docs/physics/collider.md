# Physics / Collider

衝突形状の基底クラスと具体実装。各 `Collider` は対応する `RigidBody` に紐付く。

---

## クラス階層

```
Collider  (基底、純粋仮想)
├── SphereCollider
├── AABBCollider
└── CapsuleCollider  (将来実装)
```

---

## Collider (基底)

```cpp
namespace fbzz::physics {

struct AABB {
    math::Vector3 min;
    math::Vector3 max;

    bool Overlaps(const AABB& other) const;
    AABB Merge(const AABB& other)    const;
    math::Vector3 Center() const { return (min + max) * 0.5f; }
    math::Vector3 Extents() const { return (max - min) * 0.5f; }
};

class Collider {
public:
    virtual ~Collider() = default;

    // ブロードフェーズ用 AABB を返す
    virtual AABB GetAABB() const = 0;

    // ワールド座標に追随するため毎フレーム呼ばれる
    virtual void Update(const math::Vector3& worldPos,
                        const math::Quaternion& worldRot) = 0;

    std::shared_ptr<RigidBody> m_body;  // 紐付く剛体 (弱参照でもよい)
};

} // namespace fbzz::physics
```

---

## SphereCollider

```cpp
namespace fbzz::physics {

class SphereCollider : public Collider {
public:
    SphereCollider(float radius);

    AABB GetAABB() const override;
    void Update(const math::Vector3& worldPos,
                const math::Quaternion& worldRot) override;

    float m_radius;

private:
    math::Vector3 m_worldCenter;
};

} // namespace fbzz::physics
```

---

## AABBCollider

軸整合バウンディングボックス。回転しても形状は変わらない。

```cpp
namespace fbzz::physics {

class AABBCollider : public Collider {
public:
    AABBCollider(const math::Vector3& halfExtents);

    AABB GetAABB() const override;
    void Update(const math::Vector3& worldPos,
                const math::Quaternion& worldRot) override;

    math::Vector3 m_halfExtents;

private:
    math::Vector3 m_worldCenter;
};

} // namespace fbzz::physics
```

---

## CapsuleCollider (将来実装)

カプセル = 球 + 円柱。キャラクターの当たり判定に向く。

```cpp
// 将来実装
class CapsuleCollider : public Collider {
    float m_radius;
    float m_height;  // 端の球を除いた高さ
};
```

---

## 形状間の AABB 重なり判定

BroadPhase では AABB 同士の重なりのみ確認する (厳密な形状判定は NarrowPhase で行う)。

```cpp
bool AABB::Overlaps(const AABB& other) const {
    return (min.x <= other.max.x && max.x >= other.min.x) &&
           (min.y <= other.max.y && max.y >= other.min.y) &&
           (min.z <= other.max.z && max.z >= other.min.z);
}
```

---

## ファイル構成

```
physics/
├── include/physics/
│   ├── Collider.hpp       (基底 + AABB 構造体)
│   ├── SphereCollider.hpp
│   └── AABBCollider.hpp
└── src/
    ├── Collider.cpp
    ├── SphereCollider.cpp
    └── AABBCollider.cpp
```
