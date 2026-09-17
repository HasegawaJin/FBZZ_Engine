/// @file    CylinderCollider.cpp
/// @brief   CylinderCollider.hpp の実装
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Physics/CylinderCollider.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    namespace
    {
        constexpr float PI       = 3.14159265358979323846f;
        constexpr float AXIS_EPS = 1e-12f;
    }

    CylinderCollider::CylinderCollider(float radius, float halfHeight)
        : m_radius(radius), m_halfHeight(halfHeight)
    {
    }

    float CylinderCollider::ComputeVolume() const
    {
        return PI * m_radius * m_radius * (2.0f * m_halfHeight);
    }

    AABB CylinderCollider::GetAABB() const
    {
        /// @note 軸成分 a_i が分かれば、垂直な円板が i 方向へ張る幅は r * sin(角度) = r * sqrt(1 - a_i²)
        ///       で三角関数なしに出せる。OBB のように 8 頂点を回すより緩みが無く、BroadPhase の
        ///       候補ペアがそのぶん減る。
        const math::Vector3& axis = m_worldAxis;
        const auto Extent = [&](float axisComponent) {
            const float radial = std::sqrt(std::max(0.0f, 1.0f - axisComponent * axisComponent));
            return std::abs(axisComponent) * m_halfHeight + radial * m_radius;
        };

        const math::Vector3 extents{ Extent(axis.x), Extent(axis.y), Extent(axis.z) };
        return { m_worldCenter - extents, m_worldCenter + extents };
    }

    void CylinderCollider::Update(const math::Vector3& worldPos,
                                  const math::Quaternion& worldRot)
    {
        m_worldCenter = worldPos;
        m_worldAxis   = (worldRot * math::Vector3::UP).Normalized();
    }

    math::Vector3 CylinderCollider::SupportPoint(const math::Vector3& dir) const
    {
        const float axial = math::Vector3::Dot(dir, m_worldAxis);
        math::Vector3 result =
            m_worldCenter + m_worldAxis * (axial >= 0.0f ? m_halfHeight : -m_halfHeight);

        /// @note dir から軸成分を抜いた残りが円板上での向き。軸と平行なときは円板中心がそのまま最遠点。
        const math::Vector3 radial      = dir - m_worldAxis * axial;
        const float         radialLenSq = radial.LengthSq();
        if (radialLenSq > AXIS_EPS)
            result += radial * (m_radius / std::sqrt(radialLenSq));

        return result;
    }

    math::Vector3 CylinderCollider::ClosestPoint(const math::Vector3& point) const
    {
        const math::Vector3 delta  = point - m_worldCenter;
        const float         axial  = math::Vector3::Dot(delta, m_worldAxis);
        const math::Vector3 radial = delta - m_worldAxis * axial;

        const float radialLen = radial.Length();
        const math::Vector3 clampedRadial =
            radialLen > m_radius ? radial * (m_radius / radialLen) : radial;

        return m_worldCenter
             + m_worldAxis * std::clamp(axial, -m_halfHeight, m_halfHeight)
             + clampedRadial;
    }

    bool CylinderCollider::Contains(const math::Vector3& point) const
    {
        const math::Vector3 delta = point - m_worldCenter;
        const float         axial = math::Vector3::Dot(delta, m_worldAxis);
        if (std::abs(axial) > m_halfHeight) return false;

        return (delta - m_worldAxis * axial).LengthSq() <= m_radius * m_radius;
    }

    math::Vector3 CylinderCollider::SupportFnImpl(const void* shape, const math::Vector3& dir)
    {
        return static_cast<const CylinderCollider*>(shape)->SupportPoint(dir);
    }

} // namespace fbzz::physics
