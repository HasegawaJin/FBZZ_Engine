/// @file CylinderCollider.hpp
/// @brief ローカル Y 軸を中心線とする円柱コライダー
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY カプセルと別形状にするか:
///   カプセルは端が丸いため、平らな面の上に立てても接地面が 1 点に縮退して倒れる。
///   樽・柱・車輪のように「平らな天面と底面を持つ」ものは、半球で近似すると
///   転がり方も積み上がり方も別物になる。縁が鋭い形状として独立させる。
#pragma once
#include <Physics/Collider.hpp>

namespace fbzz::physics
{
    /// 上下を円板で閉じた円柱。ローカル +Y が中心軸で、Update() でワールド軸へ回す。
    ///
    /// WHY 解析的な組み合わせテストを持たないか:
    ///   円柱同士や箱との接触は、側面・縁・円板の 3 種の特徴が組み合わさって場合分けが
    ///   爆発する。サポート関数は 5 行で厳密に書けるため、詳細判定は GJK / EPA に任せる。
    ///   球だけは最近点が閉じた式で出せるので PhysicsSolver 側で解析的に解く。
    class CylinderCollider : public Collider
    {
    public:
        CylinderCollider(float radius, float halfHeight);

        AABB         GetAABB() const override;
        ColliderType GetType() const override { return ColliderType::CYLINDER; }
        void Update(const math::Vector3& worldPos,
                    const math::Quaternion& worldRot) override;
        /// π r² * 2h
        [[nodiscard]] float ComputeVolume() const override;

        math::Vector3 GetCenter() const { return m_worldCenter; }
        /// 正規化済みのワールド中心軸 (ローカル +Y)
        math::Vector3 GetAxis() const { return m_worldAxis; }
        /// -Y 側の円板中心
        math::Vector3 GetSegmentStart() const { return m_worldCenter - m_worldAxis * m_halfHeight; }
        /// +Y 側の円板中心
        math::Vector3 GetSegmentEnd() const { return m_worldCenter + m_worldAxis * m_halfHeight; }

        /// GJK サポート関数: dir 方向で最遠の表面点。円柱では必ず上下いずれかの円板上に乗る。
        math::Vector3 SupportPoint(const math::Vector3& dir) const;
        /// 円柱上の最近点。point が内部にあるときは point 自身を返す。
        math::Vector3 ClosestPoint(const math::Vector3& point) const;
        bool Contains(const math::Vector3& point) const;

        /// PhysicsSolver から SupportFn として渡すための static ラッパー
        static math::Vector3 SupportFnImpl(const void* shape, const math::Vector3& dir);

        float m_radius     = 0.5f;
        float m_halfHeight = 1.0f; ///< 中心から円板までの距離

    private:
        math::Vector3 m_worldCenter;
        math::Vector3 m_worldAxis = math::Vector3::UP;
    };

} // namespace fbzz::physics
