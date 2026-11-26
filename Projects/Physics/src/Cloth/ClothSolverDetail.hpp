/// @file    ClothSolverDetail.hpp
/// @brief   布に共通する幾何の許容値、入力検証、空間格子の変換。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once

#include <Math/LinearAlgebra/Vector3.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fbzz::physics::cloth_detail {

using math::Vector3;

inline constexpr float GEOMETRY_EPSILON = 1.0e-8f;
/// @note 0 で衝突を無効にし、正の離隔距離は格子のセル幅として使える範囲 [m] に制限する。
inline constexpr float MIN_COLLISION_DISTANCE = 1.0e-6f;
inline constexpr float MAX_COLLISION_DISTANCE = 100.0f;
/// @note 候補走査と連続判定の計算上限。超過時は呼び出し側でステップを巻き戻す。
inline constexpr std::size_t MAX_COLLISION_CANDIDATES = 2000000;

inline bool IsFiniteVector(const Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
inline bool IsFiniteNonnegative(float value) { return std::isfinite(value) && value >= 0.0f; }
inline uint64_t ParticlePairKey(uint32_t firstParticle, uint32_t secondParticle)
{
    return (static_cast<uint64_t>(std::min(firstParticle, secondParticle)) << 32) | std::max(firstParticle, secondParticle);
}

/// @note 整数変換と隣接セルの加算を安全な範囲に限定する。
inline bool TryGetCollisionCell(const Vector3& position, float cellSize, std::array<int64_t, 3>& outCell)
{
    const double coordinates[]{
        std::floor(static_cast<double>(position.x) / cellSize),
        std::floor(static_cast<double>(position.y) / cellSize),
        std::floor(static_cast<double>(position.z) / cellSize)
    };
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(coordinates[axis]) || std::abs(coordinates[axis]) > 1.0e15) {
            return false;
        }
        outCell[axis] = static_cast<int64_t>(coordinates[axis]);
    }
    return true;
}

bool ComputeDihedral(const std::array<Vector3, 4>& positions, float& outAngle,
                     std::array<Vector3, 4>& outGradient);

}
