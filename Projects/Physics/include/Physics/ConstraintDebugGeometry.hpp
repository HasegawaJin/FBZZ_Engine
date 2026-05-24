// FBZZ Engine
// ConstraintDebugGeometry.hpp | fbzz::physics
// Constraint debug wire geometry generation
#pragma once
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/Constraint.hpp>
#include <vector>

namespace fbzz::physics
{
    struct ConstraintDebugGeometry
    {
        std::vector<DebugLine> lines;
    };

    ConstraintDebugGeometry BuildConstraintDebugGeometry(const Constraint& constraint);

} // namespace fbzz::physics
