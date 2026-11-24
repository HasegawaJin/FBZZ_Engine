/// @file    ConvexHullSupport.hpp
/// @brief   凸包の入力間引きと衝突判定で共有する支持点探索。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstddef>
#include <span>
#include <vector>

namespace fbzz::physics::convex_hull_detail
{
    /// @pre points は空でない。
    /// @note 射影が同じなら入力順で先の点を選ぶ。
    [[nodiscard]] std::size_t FindFarthestPointIndex(std::span<const math::Vector3> points,
                                                      const math::Vector3& direction);

    /// @pre points は空でなく、pointBudget は正。
    [[nodiscard]] std::vector<math::Vector3> SelectExtremePoints(std::span<const math::Vector3> points,
                                                                int pointBudget);
}
