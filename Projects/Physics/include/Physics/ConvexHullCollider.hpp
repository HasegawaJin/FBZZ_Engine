// FBZZ Engine
// ConvexHullCollider.hpp | fbzz::physics
// 点群から構築した凸包コライダー (GJK / EPA 使用)
#pragma once
#include <vector>
#include <Physics/Collider.hpp>
#include <Physics/GJK.hpp>

namespace fbzz::physics
{

    class ConvexHullCollider : public Collider
    {
    public:
        // 点群から Quickhull で凸包を構築する (最大頂点数 MAX_HULL_VERTS)
        explicit ConvexHullCollider(std::vector<math::Vector3> points);

        AABB         GetAABB() const override { return m_worldAABB; }
        ColliderType GetType() const override { return ColliderType::CONVEX_HULL; }
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;

        // GJK サポート関数: 方向 dir で最遠のワールド空間頂点を返す
        math::Vector3 SupportPoint(const math::Vector3& dir) const;

        // PhysicsSolver から SupportFn として渡すための static ラッパー
        static math::Vector3 SupportFnImpl(const void* shape, const math::Vector3& dir);

        static constexpr int MAX_HULL_VERTS = 64;

    private:
        std::vector<math::Vector3> m_localVerts;  // ローカル空間の凸包頂点
        std::vector<math::Vector3> m_worldVerts;  // Update で変換済み

        math::Vector3    m_worldCenter;
        math::Quaternion m_worldRot;
        AABB             m_worldAABB;

        // Quickhull で凸包を計算し m_localVerts に格納する
        void BuildHull(std::vector<math::Vector3> points);

        void UpdateWorldVerts(const math::Vector3& pos, const math::Quaternion& rot);
    };

} // namespace fbzz::physics
