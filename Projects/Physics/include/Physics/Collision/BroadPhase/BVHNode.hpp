/// @file    BVHNode.hpp
/// @brief   三角形メッシュと高さ場で共有する AABB 階層。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Collision/BroadPhase/BVHBounds.hpp>
#include <array>
#include <vector>
#include <cstdint>
#include <Physics/Collision/Shapes/Collider.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::physics
{

    struct Triangle
    {
        math::Vector3 v[3]; ///< 三角形の 3 頂点のワールド座標 [m]。元の頂点順で並べる。
        math::Vector3 normal;    ///< @note 正規化済み面法線
        uint32_t      index = 0; ///< @note 元のインデックスバッファ上の三角形番号 (デバッグ用)
    };

    struct BVHNode
    {
        AABB                  aabb; ///< 部分木内に含まれる三角形のワールド境界 [m]。
        int                   left       = -1; ///< @note 内部ノード: 左子インデックス
        int                   right      = -1; ///< @note 内部ノード: 右子インデックス (-1 なら葉ノード)
        std::vector<uint32_t> triIndices;       ///< @note 葉ノードのみ有効

        bool IsLeaf() const { return left == -1; }
    };

    /// @note BVH ツリー本体。再帰ポインタではなくフラット配列で管理し、再構築時の所有を単純にする。
    struct BVHTree
    {
        std::vector<BVHNode> nodes; ///< メッシュの BVH ノード。先頭は根、子は配列の添字で参照する。
        std::vector<Triangle> triangles; ///< BVH の葉が添字で参照するワールド空間の三角形。

        /// @note ワールド変換済み三角形リストから中点分割で BVH を構築する。
        /// @note maxLeafTris は、葉ノードに格納する最大三角形数。
        FBZZ_PHYSICS_API void Build(std::vector<Triangle>&& worldTriangles, int maxLeafTris = 8);

        /// @note triangles の頂点を書き換えた後に、木の構造はそのままノード AABB だけ測り直す。
        /// @note 三角形の所属する葉を保持し、葉から根への一巡で境界だけを更新する。
        FBZZ_PHYSICS_API void Refit();

        /// @note 個々の境界が検索範囲と重なる三角形を、左の子を先にたどる深さ優先順で列挙する。
        template<typename TriangleVisitor>
        void Query(const AABB& queryBounds, TriangleVisitor&& visitTriangle) const;

    private:
        /// @return [rangeBegin, rangeEnd) の三角形から構築したノードの添字。
        FBZZ_PHYSICS_API int BuildRecursive(std::vector<uint32_t>& triangleOrder,
                                          int rangeBegin, int rangeEnd, int maxLeafTris);
    };

    template<typename TriangleVisitor>
    void BVHTree::Query(const AABB& queryBounds, TriangleVisitor&& visitTriangle) const
    {
        if (nodes.empty()) {
            return;
        }

        // 固定長スタックを使い、検索でヒープ確保や再帰呼び出しを行わない。
        std::array<int, 64> pendingNodes;
        int pendingCount = 0;
        pendingNodes[pendingCount++] = 0;

        while (pendingCount > 0)
        {
            const int nodeIndex = pendingNodes[--pendingCount];
            if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size())) {
                continue;
            }

            const BVHNode& node = nodes[nodeIndex];
            if (!node.aabb.Overlaps(queryBounds)) {
                continue;
            }

            if (node.IsLeaf())
            {
                // 葉全体に当たっても、各三角形の範囲まで重なるとは限らない。
                for (const uint32_t triangleIndex : node.triIndices)
                {
                    const Triangle& triangle = triangles[triangleIndex];
                    if (mesh_bvh_detail::TriangleBounds(triangle.v).Overlaps(queryBounds)) {
                        visitTriangle(triangle);
                    }
                }
            }
            else if (pendingCount + 1 < static_cast<int>(pendingNodes.size()))
            {
                pendingNodes[pendingCount++] = node.right;
                pendingNodes[pendingCount++] = node.left;
            }
        }
    }

} /// @note namespace fbzz::physics
