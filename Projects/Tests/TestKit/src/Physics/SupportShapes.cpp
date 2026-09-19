/// @file    SupportShapes.cpp
/// @brief   テスト用支持関数の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Physics/SupportShapes.hpp>

namespace fbzz::testkit {

math::Vector3 SupportSphere::Support(const void* shape, const math::Vector3& dir)
{
    const auto& sphere = *static_cast<const SupportSphere*>(shape);
    /// @note 方向が退化した呼び出しは GJK の初期化で普通に起きる。止めずに任意の一点を返す。
    return sphere.center + dir.NormalizedOr(math::Vector3::RIGHT) * sphere.radius;
}

math::Vector3 SupportBox::Support(const void* shape, const math::Vector3& dir)
{
    const auto& box = *static_cast<const SupportBox*>(shape);
    return {box.center.x + (dir.x >= 0.0f ? box.halfExtents.x : -box.halfExtents.x),
            box.center.y + (dir.y >= 0.0f ? box.halfExtents.y : -box.halfExtents.y),
            box.center.z + (dir.z >= 0.0f ? box.halfExtents.z : -box.halfExtents.z)};
}

math::Vector3 SupportPoint::Support(const void* shape, const math::Vector3&)
{
    return static_cast<const SupportPoint*>(shape)->position;
}

} // namespace fbzz::testkit
