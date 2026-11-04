/// @file    ConvexHullCollider.hpp
/// @brief   点群から構築した凸包コライダー (GJK / EPA 使用)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include <Physics/Api.hpp>
#include <array>
#include <cstdint>
#include <vector>
#include <Physics/Collider.hpp>
#include <Physics/GJK.hpp>

namespace fbzz::physics
{

    /// @note 任意点群を凸形状として扱うためのコライダー。詳細判定は GJK/EPA に委譲する。
    class ConvexHullCollider : public Collider
    {
    public:
        /// @note 点群から Quickhull で凸包を構築する (最大頂点数 MAX_HULL_VERTS)。
        /// @note 上限を設けることでサポート点探索の線形コストを抑える。
        FBZZ_PHYSICS_API explicit ConvexHullCollider(std::vector<math::Vector3> points);

        AABB         GetAABB() const override { return m_worldAABB; }
        ColliderType GetType() const override { return ColliderType::CONVEX_HULL; }
        FBZZ_PHYSICS_API void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;
        FBZZ_PHYSICS_API void UpdateWithScale(const math::Vector3& worldPos,
                             const math::Quaternion& worldRot,
                             const math::Vector3& worldScale);

        /// @note GJK サポート関数: 方向 dir で最遠のワールド空間頂点を返す
        FBZZ_PHYSICS_API math::Vector3 SupportPoint(const math::Vector3& dir) const;
        const std::vector<math::Vector3>& GetWorldVertices() const { return m_worldVerts; }
        const std::vector<std::array<uint32_t, 3>>& GetFaces() const { return m_faces; }

        /// @note PhysicsSolver から SupportFn として渡すための static ラッパー
        FBZZ_PHYSICS_API static math::Vector3 SupportFnImpl(const void* shape, const math::Vector3& dir);

        static constexpr int MAX_HULL_VERTS = 64; ///< @note Sandbox 検証用の凸形状を安定して処理する上限

    private:
        std::vector<math::Vector3> m_localVerts;  ///< @note ローカル空間の凸包頂点
        std::vector<math::Vector3> m_worldVerts;  ///< @note Update で変換済み
        std::vector<std::array<uint32_t, 3>> m_faces;

        math::Vector3    m_worldCenter;
        math::Quaternion m_worldRot;
        math::Vector3    m_worldScale = { 1.0f, 1.0f, 1.0f };
        AABB             m_worldAABB;

        /// @note Quickhull で凸包を計算し m_localVerts に格納する
        FBZZ_PHYSICS_API void BuildHull(std::vector<math::Vector3> points);

        FBZZ_PHYSICS_API void UpdateWorldVerts(const math::Vector3& pos,
                              const math::Quaternion& rot,
                              const math::Vector3& scale);
    };

} /// @note namespace fbzz::physics
