/// @file    Collider.hpp
/// @brief   コライダー形状の定義 (Sphere / AABB / OBB / Capsule / Cylinder / Mesh / ConvexHull / HeightField)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>

namespace fbzz::physics 
{
    /// BroadPhase 用の軸整合境界。形状ごとの詳細判定より先に粗い重なりを調べる。
    struct AABB {
        math::Vector3 min;
        math::Vector3 max;

        bool          Overlaps(const AABB& other) const;
        AABB          Merge(const AABB& other)    const;
        math::Vector3 Center()  const { return (min + max) * 0.5f; }
        math::Vector3 Extents() const { return (max - min) * 0.5f; }
    };

    enum class ColliderType { SPHERE, AABB, OBB, CAPSULE, CYLINDER, TRIANGLE_MESH, CONVEX_HULL, HEIGHT_FIELD };

    /// World は Collider を所有しない。Scene 側の ColliderComponent が共有所有し、World は参照して使う。
    class Collider {
    public:
        virtual ~Collider() = default;

        virtual AABB         GetAABB() const = 0;
        virtual ColliderType GetType() const = 0;

        /// 形状の体積。PhysicsMaterial::density から質量を求めるのに使う。
        ///
        /// @note 既定を AABB 体積にする理由: 三角メッシュや凸包の厳密な体積は符号付き四面体積分が要り
        ///       開いたメッシュでは破綻するため、外接箱で近似する。厳密さが要る基本形状だけが override し、
        ///       純粋仮想にしないのは形状追加のたびに体積計算を強制しないため。
        [[nodiscard]] virtual float ComputeVolume() const;

        /// World::UpdateColliders() から毎フレーム呼ばれ、剛体の Transform を形状へ同期する。
        virtual void Update(const math::Vector3& worldPos,
                            const math::Quaternion& worldRot) = 0;
    };

} // namespace fbzz::physics
