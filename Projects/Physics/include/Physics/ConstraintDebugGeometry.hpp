/// @file    ConstraintDebugGeometry.hpp
/// @brief   制約可視化用のワイヤージオメトリ生成。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include <Physics/Api.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/Constraint.hpp>
#include <vector>

namespace fbzz::physics
{
    /// @note Constraint の種類ごとに、接続線やヒンジ軸を Renderer 非依存の線分として返す。
    struct ConstraintDebugGeometry
    {
        std::vector<DebugLine> lines;
    };

    FBZZ_PHYSICS_API ConstraintDebugGeometry BuildConstraintDebugGeometry(const Constraint& constraint);

} /// @note namespace fbzz::physics
