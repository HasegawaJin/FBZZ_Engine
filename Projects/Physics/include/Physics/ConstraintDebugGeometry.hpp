// FBZZ Engine
// ConstraintDebugGeometry.hpp | fbzz::physics
// 制約可視化用のワイヤージオメトリ生成
#pragma once
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/Constraint.hpp>
#include <vector>

namespace fbzz::physics
{
    // Constraint の種類ごとに、接続線やヒンジ軸を Renderer 非依存の線分として返す。
    struct ConstraintDebugGeometry
    {
        std::vector<DebugLine> lines;
    };

    ConstraintDebugGeometry BuildConstraintDebugGeometry(const Constraint& constraint);

} // namespace fbzz::physics
