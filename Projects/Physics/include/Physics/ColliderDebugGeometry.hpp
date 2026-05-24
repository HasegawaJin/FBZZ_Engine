// FBZZ Engine
// ColliderDebugGeometry.hpp | fbzz::physics
// コライダー可視化用のワイヤージオメトリ生成
#pragma once
#include <Math/Vector3.hpp>
#include <Physics/Collider.hpp>
#include <vector>

namespace fbzz::physics
{
    // Renderer へ直接依存しないよう、Physics は線分リストだけを返す。
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
