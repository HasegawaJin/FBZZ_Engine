/// @file    ColliderDebugBVH.cpp
/// @brief   BVH を視点からたどり、近景の三角形と遠景の境界箱を生成する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "ColliderDebugGeometryDetail.hpp"
#include <Physics/Collision/BroadPhase/BVHNode.hpp>
#include <algorithm>
#include <utility>

namespace fbzz::physics::collider_debug_detail
{
    namespace
    {
        /// @note 遠景は箱を一つの描画単位とし、上限到達前に始めた箱の稜線は揃えて出す。
        constexpr std::size_t COARSE_LINE_THRESHOLD = 4096;
        /// @note 箱の大きさと視点距離の比がこの値以下なら、遠景での分割を打ち切る。
        constexpr float COARSE_DETAIL_RATIO = 0.12f;

        float DistanceSquaredToBounds(const math::Vector3& point, const AABB& bounds)
        {
            const float distanceX = std::max({bounds.min.x - point.x, 0.0f, point.x - bounds.max.x});
            const float distanceY = std::max({bounds.min.y - point.y, 0.0f, point.y - bounds.max.y});
            const float distanceZ = std::max({bounds.min.z - point.z, 0.0f, point.z - bounds.max.z});
            return distanceX * distanceX + distanceY * distanceY + distanceZ * distanceZ;
        }

        void AppendLeafTriangles(ColliderDebugGeometry& detailGeometry, const BVHTree& bvh, const BVHNode& leaf)
        {
            for (const uint32_t triangleIndex : leaf.triIndices)
            {
                if (triangleIndex >= bvh.triangles.size()) continue;
                AppendTriangleEdges(detailGeometry, bvh.triangles[triangleIndex].v);
            }
        }

        void PushChildrenByDistance(std::vector<int>& pendingNodes, const BVHTree& bvh,
                                    const BVHNode& parent, const math::Vector3& cameraPosition)
        {
            int firstOnStack = parent.left;
            int lastOnStack = parent.right;
            const int nodeCount = static_cast<int>(bvh.nodes.size());
            /// @note 遠い子を先に積み、近い子から詳細線を出す。同距離なら右の子を先に処理する。
            if (firstOnStack >= 0 && firstOnStack < nodeCount && lastOnStack >= 0 && lastOnStack < nodeCount &&
                DistanceSquaredToBounds(cameraPosition, bvh.nodes[firstOnStack].aabb) <
                DistanceSquaredToBounds(cameraPosition, bvh.nodes[lastOnStack].aabb))
            {
                std::swap(firstOnStack, lastOnStack);
            }
            pendingNodes.push_back(firstOnStack);
            pendingNodes.push_back(lastOnStack);
        }
    }

    ColliderDebugGeometry BuildBVHLodGeometry(const BVHTree& bvh, const ColliderDebugView& view)
    {
        if (bvh.nodes.empty() || bvh.triangles.empty()) return {};

        ColliderDebugGeometry detailGeometry;
        ColliderDebugGeometry coarseGeometry;
        detailGeometry.lines.reserve(std::min(MAX_MESH_DEBUG_LINES, bvh.triangles.size() * 3));
        const float detailRadiusSquared = view.detailRadius * view.detailRadius;
        constexpr float COARSE_RATIO_SQUARED = COARSE_DETAIL_RATIO * COARSE_DETAIL_RATIO;

        std::vector<int> pendingNodes;
        pendingNodes.reserve(64);
        pendingNodes.push_back(0);
        while (!pendingNodes.empty())
        {
            const bool hasDetailBudget = detailGeometry.lines.size() < MAX_MESH_DEBUG_LINES;
            const bool hasCoarseBudget = coarseGeometry.lines.size() < COARSE_LINE_THRESHOLD;
            if (!hasDetailBudget && !hasCoarseBudget) break;

            const int nodeIndex = pendingNodes.back();
            pendingNodes.pop_back();
            if (nodeIndex < 0 || nodeIndex >= static_cast<int>(bvh.nodes.size())) continue;

            const BVHNode& node = bvh.nodes[nodeIndex];
            const math::Vector3 halfExtents = node.aabb.Extents();
            const float distanceSquared = DistanceSquaredToBounds(view.cameraPosition, node.aabb);

            /// @note 詳細線の予算を使い切った手前のノードも、残りの箱の予算で表せる。
            const bool drawDetail = distanceSquared <= detailRadiusSquared && hasDetailBudget;
            if (!drawDetail && (node.IsLeaf() || halfExtents.LengthSq() <= distanceSquared * COARSE_RATIO_SQUARED))
            {
                if (hasCoarseBudget)
                    AppendBoundsBox(coarseGeometry, node.aabb.Center(), halfExtents);
                continue;
            }

            if (node.IsLeaf())
                AppendLeafTriangles(detailGeometry, bvh, node);
            else
                PushChildrenByDistance(pendingNodes, bvh, node, view.cameraPosition);
        }

        detailGeometry.detailLineCount = detailGeometry.lines.size();
        detailGeometry.lines.insert(detailGeometry.lines.end(), coarseGeometry.lines.begin(), coarseGeometry.lines.end());
        return detailGeometry;
    }
}
