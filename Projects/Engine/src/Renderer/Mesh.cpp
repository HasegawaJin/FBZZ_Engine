/// @file    Mesh.cpp
/// @brief   Mesh ユーティリティ関数。
/// @author  Hasegawa Jin
/// @date    2026-05-31
#include "Engine/Renderer/Mesh.hpp"
#include <cmath>
#include <limits>

namespace fbzz::renderer {

void Mesh::ComputeBounds()
{
    /// @note CPU 頂点配列 (スキンドメッシュ優先) から AABB と、中心からの最大距離 (球半径) を求める。
    ///       Frustum/Occlusion カリングは AABB (6 平面比較) より分岐が少ない球テストで行うため。
    ///       スキン時はバインドポーズ球を使うため、アニメーション伸縮に対して保守的な球になりうる。

    const bool hasSkinned = !cpuSkinnedVertices.empty();
    const bool hasStatic  = !cpuVertices.empty();

    if (!hasSkinned && !hasStatic) {
        boundsCenter  = {};
        boundsRadius  = 0.0f;
        boundsExtents = {};
        return;
    }

    math::Vector3 vmin = {  std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max() };
    math::Vector3 vmax = { -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max() };

    auto expand = [&](const math::Vector3& p) {
        if (p.x < vmin.x) vmin.x = p.x;
        if (p.y < vmin.y) vmin.y = p.y;
        if (p.z < vmin.z) vmin.z = p.z;
        if (p.x > vmax.x) vmax.x = p.x;
        if (p.y > vmax.y) vmax.y = p.y;
        if (p.z > vmax.z) vmax.z = p.z;
    };

    if (hasSkinned) {
        for (const auto& v : cpuSkinnedVertices) expand(v.position);
    } else {
        for (const auto& v : cpuVertices)        expand(v.position);
    }

    boundsCenter  = (vmin + vmax) * 0.5f;
    boundsExtents = (vmax - vmin) * 0.5f;

    /// @note AABB 中心から最遠頂点までの距離を半径とする。
    float maxR2 = 0.0f;
    if (hasSkinned) {
        for (const auto& v : cpuSkinnedVertices) {
            const math::Vector3 d = v.position - boundsCenter;
            const float r2 = d.x*d.x + d.y*d.y + d.z*d.z;
            if (r2 > maxR2) maxR2 = r2;
        }
    } else {
        for (const auto& v : cpuVertices) {
            const math::Vector3 d = v.position - boundsCenter;
            const float r2 = d.x*d.x + d.y*d.y + d.z*d.z;
            if (r2 > maxR2) maxR2 = r2;
        }
    }
    boundsRadius = std::sqrt(maxR2);
}

void Mesh::ComputeBoundsExtents()
{
    const bool hasSkinned = !cpuSkinnedVertices.empty();
    const bool hasStatic  = !cpuVertices.empty();
    if (!hasSkinned && !hasStatic) {
        boundsExtents = {};
        return;
    }

    math::Vector3 vmin = {  std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max() };
    math::Vector3 vmax = { -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max(),
                            -std::numeric_limits<float>::max() };
    const auto expand = [&](const math::Vector3& p) {
        if (p.x < vmin.x) vmin.x = p.x;
        if (p.y < vmin.y) vmin.y = p.y;
        if (p.z < vmin.z) vmin.z = p.z;
        if (p.x > vmax.x) vmax.x = p.x;
        if (p.y > vmax.y) vmax.y = p.y;
        if (p.z > vmax.z) vmax.z = p.z;
    };
    if (hasSkinned) {
        for (const auto& v : cpuSkinnedVertices) expand(v.position);
    } else {
        for (const auto& v : cpuVertices)        expand(v.position);
    }

    boundsExtents = (vmax - vmin) * 0.5f;
}

} // namespace fbzz::renderer
