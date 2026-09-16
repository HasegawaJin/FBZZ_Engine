/// @file    ColliderDebugGeometry.hpp
/// @brief   コライダー可視化用のワイヤージオメトリ生成。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once

#include <Math/Vector3.hpp>
#include <Physics/Collider.hpp>
#include <cstddef>
#include <vector>

namespace fbzz::physics
{
    // Renderer へ直接依存しないよう、Physics は線分リストだけを返す。
    struct DebugLine
    {
        math::Vector3 from;
        math::Vector3 to;
    };

    /// 可視化の視点。BVH を持つコライダー (TriangleMesh / HeightField) の詳細度をカメラで決める。
    ///
    /// WHY 必要か: BVH コライダーは 1 個で 10 万三角形を超える。視点なしでは
    ///     「全域から等間隔に間引く」以外に選びようがなく、地形いっぱいに
    ///     繋がらない三角形が散るだけで面の形もカメラ手前の当たり判定も読めなかった。
    struct ColliderDebugView
    {
        math::Vector3 cameraPosition{};
        /// この距離までを三角形ワイヤーで描く [m]。外側は BVH ノードの箱で表す。
        float detailRadius = 80.0f;
        /// false なら視点を無視し、全域を等間隔に間引いた従来の形を返す。
        bool enabled = false;
    };

    struct ColliderDebugGeometry
    {
        std::vector<DebugLine> lines;

        /// lines の先頭 detailLineCount 本が三角形ワイヤー、残りが BVH ノードの箱。
        /// WHY 並び順で表すか: 線ごとに種別フラグを持たせると、10 万本規模で
        ///     線分リスト自体が倍近く太る。2 色に塗り分けたいだけなので境界 1 個で足りる。
        std::size_t detailLineCount = 0;
    };

    ColliderDebugGeometry BuildColliderDebugGeometry(const Collider& collider);

    /// カメラ視点つき。BVH を持たない形状では視点は無視され、上の版と同じ結果になる。
    ColliderDebugGeometry BuildColliderDebugGeometry(const Collider& collider,
                                                     const ColliderDebugView& view);

} // namespace fbzz::physics
