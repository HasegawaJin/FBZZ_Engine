// FBZZ Engine
// Collider.hpp | fbzz::physics
// コライダー形状の定義 (Sphere / AABB / Capsule / Mesh / ConvexHull)
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>

namespace fbzz::physics 
{
    // BroadPhase 用の軸整合境界。形状ごとの詳細判定より先に粗い重なりを調べる。
    struct AABB {
        math::Vector3 min;
        math::Vector3 max;

        bool          Overlaps(const AABB& other) const;
        AABB          Merge(const AABB& other)    const;
        math::Vector3 Center()  const { return (min + max) * 0.5f; }
        math::Vector3 Extents() const { return (max - min) * 0.5f; }
    };

    enum class ColliderType { SPHERE, AABB, OBB, CAPSULE, TRIANGLE_MESH, CONVEX_HULL, HEIGHT_FIELD };

    // World は Collider を所有しない。Scene 側の ColliderComponent が共有所有し、World は参照して使う。
    class Collider {
    public:
        virtual ~Collider() = default;

        virtual AABB         GetAABB() const = 0;
        virtual ColliderType GetType() const = 0;

        // World::UpdateColliders() から毎フレーム呼ばれ、剛体の Transform を形状へ同期する。
        virtual void Update(const math::Vector3& worldPos,
                            const math::Quaternion& worldRot) = 0;
    };

} // namespace fbzz::physics
