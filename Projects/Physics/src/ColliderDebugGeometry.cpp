// FBZZ Engine
// ColliderDebugGeometry.cpp | fbzz::physics
// コライダー可視化用のワイヤージオメトリ生成
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <array>
#include <cmath>

namespace fbzz::physics
{
namespace
{
    constexpr float PI = 3.14159265358979323846f;
    constexpr int CIRCLE_SEGMENTS = 24;

    void AddLine(ColliderDebugGeometry& out, const math::Vector3& from, const math::Vector3& to)
    {
        out.lines.push_back({ from, to });
    }

    void AddBox(ColliderDebugGeometry& out, const math::Vector3& center, const math::Vector3& halfExtents)
    {
        const math::Vector3 h = halfExtents;
        const math::Vector3 c[8] = {
            center + math::Vector3{ -h.x, -h.y, -h.z },
            center + math::Vector3{  h.x, -h.y, -h.z },
            center + math::Vector3{  h.x,  h.y, -h.z },
            center + math::Vector3{ -h.x,  h.y, -h.z },
            center + math::Vector3{ -h.x, -h.y,  h.z },
            center + math::Vector3{  h.x, -h.y,  h.z },
            center + math::Vector3{  h.x,  h.y,  h.z },
            center + math::Vector3{ -h.x,  h.y,  h.z },
        };

        AddLine(out, c[0], c[1]); AddLine(out, c[1], c[2]);
        AddLine(out, c[2], c[3]); AddLine(out, c[3], c[0]);
        AddLine(out, c[4], c[5]); AddLine(out, c[5], c[6]);
        AddLine(out, c[6], c[7]); AddLine(out, c[7], c[4]);
        AddLine(out, c[0], c[4]); AddLine(out, c[1], c[5]);
        AddLine(out, c[2], c[6]); AddLine(out, c[3], c[7]);
    }

    void AddBox(ColliderDebugGeometry& out, const std::array<math::Vector3, 8>& c)
    {
        AddLine(out, c[0], c[1]); AddLine(out, c[1], c[2]);
        AddLine(out, c[2], c[3]); AddLine(out, c[3], c[0]);
        AddLine(out, c[4], c[5]); AddLine(out, c[5], c[6]);
        AddLine(out, c[6], c[7]); AddLine(out, c[7], c[4]);
        AddLine(out, c[0], c[4]); AddLine(out, c[1], c[5]);
        AddLine(out, c[2], c[6]); AddLine(out, c[3], c[7]);
    }

    void AddCircle(ColliderDebugGeometry& out,
                   const math::Vector3& center,
                   const math::Vector3& axisA,
                   const math::Vector3& axisB,
                   float radius)
    {
        for (int i = 0; i < CIRCLE_SEGMENTS; ++i)
        {
            const float a0 = (static_cast<float>(i) / CIRCLE_SEGMENTS) * PI * 2.0f;
            const float a1 = (static_cast<float>(i + 1) / CIRCLE_SEGMENTS) * PI * 2.0f;
            const math::Vector3 p0 = center
                + axisA * (std::cos(a0) * radius)
                + axisB * (std::sin(a0) * radius);
            const math::Vector3 p1 = center
                + axisA * (std::cos(a1) * radius)
                + axisB * (std::sin(a1) * radius);
            AddLine(out, p0, p1);
        }
    }

    void AddArc(ColliderDebugGeometry& out,
                const math::Vector3& center,
                const math::Vector3& axisA,
                const math::Vector3& axisB,
                float radius,
                float start,
                float end)
    {
        constexpr int ARC_SEGMENTS = CIRCLE_SEGMENTS / 2;
        for (int i = 0; i < ARC_SEGMENTS; ++i)
        {
            const float t0 = static_cast<float>(i) / ARC_SEGMENTS;
            const float t1 = static_cast<float>(i + 1) / ARC_SEGMENTS;
            const float a0 = start + (end - start) * t0;
            const float a1 = start + (end - start) * t1;
            const math::Vector3 p0 = center
                + axisA * (std::cos(a0) * radius)
                + axisB * (std::sin(a0) * radius);
            const math::Vector3 p1 = center
                + axisA * (std::cos(a1) * radius)
                + axisB * (std::sin(a1) * radius);
            AddLine(out, p0, p1);
        }
    }

    void BuildBasis(const math::Vector3& axis, math::Vector3& outX, math::Vector3& outY, math::Vector3& outZ)
    {
        outY = axis.Normalized();
        // 軸が UP に近いと外積が小さくなるため、参照軸を切り替えて安定した直交基底を作る。
        const math::Vector3 ref = std::abs(outY.y) > 0.95f ? math::Vector3::RIGHT : math::Vector3::UP;
        outX = math::Vector3::Cross(ref, outY).Normalized();
        outZ = math::Vector3::Cross(outY, outX).Normalized();
    }

    ColliderDebugGeometry BuildAABBGeometry(const Collider& collider)
    {
        ColliderDebugGeometry out;
        const AABB bounds = collider.GetAABB();
        AddBox(out, bounds.Center(), bounds.Extents());
        return out;
    }

    ColliderDebugGeometry BuildSphereGeometry(const SphereCollider& sphere)
    {
        ColliderDebugGeometry out;
        const math::Vector3 center = sphere.GetAABB().Center();
        AddCircle(out, center, math::Vector3::RIGHT, math::Vector3::UP, sphere.m_radius);
        AddCircle(out, center, math::Vector3::RIGHT, math::Vector3::FORWARD, sphere.m_radius);
        AddCircle(out, center, math::Vector3::UP, math::Vector3::FORWARD, sphere.m_radius);
        return out;
    }

    ColliderDebugGeometry BuildOBBGeometry(const OBBCollider& obb)
    {
        ColliderDebugGeometry out;
        AddBox(out, obb.GetCorners());
        return out;
    }

    ColliderDebugGeometry BuildCapsuleGeometry(const CapsuleCollider& capsule)
    {
        ColliderDebugGeometry out;
        const math::Vector3 top = capsule.GetSegmentEnd();
        const math::Vector3 bottom = capsule.GetSegmentStart();
        const math::Vector3 axis = top - bottom;
        if (axis.LengthSq() < 1e-6f)
            return BuildAABBGeometry(capsule);

        math::Vector3 x;
        math::Vector3 y;
        math::Vector3 z;
        BuildBasis(axis, x, y, z);

        AddCircle(out, top, x, z, capsule.m_radius);
        AddCircle(out, bottom, x, z, capsule.m_radius);
        AddArc(out, top, x, y, capsule.m_radius, 0.0f, PI);
        AddArc(out, top, z, y, capsule.m_radius, 0.0f, PI);
        AddArc(out, bottom, x, y, capsule.m_radius, PI, PI * 2.0f);
        AddArc(out, bottom, z, y, capsule.m_radius, PI, PI * 2.0f);

        AddLine(out, top + x * capsule.m_radius, bottom + x * capsule.m_radius);
        AddLine(out, top - x * capsule.m_radius, bottom - x * capsule.m_radius);
        AddLine(out, top + z * capsule.m_radius, bottom + z * capsule.m_radius);
        AddLine(out, top - z * capsule.m_radius, bottom - z * capsule.m_radius);
        return out;
    }
} // namespace

ColliderDebugGeometry BuildColliderDebugGeometry(const Collider& collider)
{
    switch (collider.GetType())
    {
    case ColliderType::SPHERE:
        return BuildSphereGeometry(static_cast<const SphereCollider&>(collider));
    case ColliderType::AABB:
        return BuildAABBGeometry(collider);
    case ColliderType::OBB:
        return BuildOBBGeometry(static_cast<const OBBCollider&>(collider));
    case ColliderType::CAPSULE:
        return BuildCapsuleGeometry(static_cast<const CapsuleCollider&>(collider));
    case ColliderType::TRIANGLE_MESH:
    case ColliderType::CONVEX_HULL:
        // 複雑形状は全ワイヤーを生成せず、編集時に見やすい AABB 表示へフォールバックする。
        return BuildAABBGeometry(collider);
    }

    return BuildAABBGeometry(collider);
}

} // namespace fbzz::physics
