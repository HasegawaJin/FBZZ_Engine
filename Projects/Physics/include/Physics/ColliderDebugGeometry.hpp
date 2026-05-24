// FBZZ Engine
// ColliderDebugGeometry.hpp | fbzz::physics
// Collider debug wire geometry generation
#pragma once
#include <Math/Vector3.hpp>
#include <Physics/Collider.hpp>
#include <vector>

namespace fbzz::physics
{
    struct DebugLine
    {
        math::Vector3 from;
        math::Vector3 to;
    };

    struct ColliderDebugGeometry
    {
        std::vector<DebugLine> lines;
    };

    ColliderDebugGeometry BuildColliderDebugGeometry(const Collider& collider);

} // namespace fbzz::physics
