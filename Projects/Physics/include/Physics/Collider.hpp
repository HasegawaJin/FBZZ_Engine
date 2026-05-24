// FBZZ Engine
// Collider.hpp | fbzz::physics
// コライダー形状の定義 (Sphere / AABB / Capsule)
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>

namespace fbzz::physics 
{
    struct AABB {
        math::Vector3 min;
        math::Vector3 max;

        bool          Overlaps(const AABB& other) const;
        AABB          Merge(const AABB& other)    const;
        math::Vector3 Center()  const { return (min + max) * 0.5f; }
        math::Vector3 Extents() const { return (max - min) * 0.5f; }
    };

    enum class ColliderType { SPHERE, AABB, CAPSULE, TRIANGLE_MESH, CONVEX_HULL };

    class Collider {
    public:
        virtual ~Collider() = default;

        virtual AABB         GetAABB() const = 0;
        virtual ColliderType GetType() const = 0;

        // 毎フレーム World::UpdateColliders() から呼ばれる
        virtual void Update(const math::Vector3& worldPos,
                            const math::Quaternion& worldRot) = 0;
    };

} // namespace fbzz::physics
