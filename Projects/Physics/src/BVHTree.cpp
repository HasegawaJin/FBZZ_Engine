/// @file    BVHTree.cpp
/// @brief   三角形 BVH の分割構築と、姿勢変更後の境界の再計算。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/BVHNode.hpp>
#include <algorithm>
#include <cstddef>
#include <numeric>
#include <utility>

namespace fbzz::physics
{
    namespace
    {
        using mesh_bvh_detail::IncludePoint;
        using mesh_bvh_detail::MakeEmptyBounds;

        void IncludeTriangle(AABB& bounds, const Triangle& triangle)
        {
            for (const math::Vector3& vertex : triangle.v)
                IncludePoint(bounds, vertex);
        }

        float AxisCoordinate(const math::Vector3& point, int axis)
        {
            if (axis == 0) return point.x;
            if (axis == 1) return point.y;
            return point.z;
        }

        int LongestAxis(const math::Vector3& extent)
        {
            int axis = extent.y > extent.x ? 1 : 0;
            if (extent.z > (axis == 0 ? extent.x : extent.y)) axis = 2;
            return axis;
        }
    }

    void BVHTree::Build(std::vector<Triangle>&& worldTriangles, int maxLeafTris)
    {
        triangles = std::move(worldTriangles);
        nodes.clear();
        if (triangles.empty()) return;

        /// @note 三角形本体は元の順序で保持し、分割では添字だけを並べ替える。
        std::vector<uint32_t> triangleOrder(triangles.size());
        std::iota(triangleOrder.begin(), triangleOrder.end(), 0u);
        BuildRecursive(triangleOrder, 0, static_cast<int>(triangleOrder.size()), maxLeafTris);
    }

    int BVHTree::BuildRecursive(std::vector<uint32_t>& triangleOrder,
                                int rangeBegin, int rangeEnd, int maxLeafTris)
    {
        const int nodeIndex = static_cast<int>(nodes.size());
        nodes.emplace_back();
        BVHNode& node = nodes[nodeIndex];
        node.aabb = MakeEmptyBounds();
        for (int orderIndex = rangeBegin; orderIndex < rangeEnd; ++orderIndex)
            IncludeTriangle(node.aabb, triangles[triangleOrder[orderIndex]]);

        const int triangleCount = rangeEnd - rangeBegin;
        if (triangleCount <= maxLeafTris)
        {
            node.triIndices.reserve(static_cast<std::size_t>(triangleCount));
            for (int orderIndex = rangeBegin; orderIndex < rangeEnd; ++orderIndex)
                node.triIndices.push_back(triangleOrder[orderIndex]);
            return nodeIndex;
        }

        const int splitAxis = LongestAxis(node.aabb.max - node.aabb.min);
        const float splitCoordinate = (AxisCoordinate(node.aabb.min, splitAxis)
                                     + AxisCoordinate(node.aabb.max, splitAxis)) * 0.5f;
        const auto partitionEnd = std::partition(triangleOrder.begin() + rangeBegin,
            triangleOrder.begin() + rangeEnd, [&](uint32_t triangleIndex)
            {
                const Triangle& triangle = triangles[triangleIndex];
                const float centroidCoordinate = (AxisCoordinate(triangle.v[0], splitAxis)
                                                + AxisCoordinate(triangle.v[1], splitAxis)
                                                + AxisCoordinate(triangle.v[2], splitAxis)) / 3.0f;
                return centroidCoordinate < splitCoordinate;
            });

        int splitIndex = static_cast<int>(partitionEnd - triangleOrder.begin());
        if (splitIndex == rangeBegin || splitIndex == rangeEnd)
            splitIndex = rangeBegin + triangleCount / 2;

        /// @note 再帰中の nodes の再確保をまたぐため、親を参照で保持せず添字で引き直す。
        const int leftChild = BuildRecursive(triangleOrder, rangeBegin, splitIndex, maxLeafTris);
        const int rightChild = BuildRecursive(triangleOrder, splitIndex, rangeEnd, maxLeafTris);
        nodes[nodeIndex].left = leftChild;
        nodes[nodeIndex].right = rightChild;
        return nodeIndex;
    }

    void BVHTree::Refit()
    {
        if (nodes.empty() || triangles.empty()) return;

        const int nodeCount = static_cast<int>(nodes.size());
        /// @note 構築時は親を先に追加するため、逆順なら子の境界が先に確定する。
        for (int nodeIndex = nodeCount - 1; nodeIndex >= 0; --nodeIndex)
        {
            BVHNode& node = nodes[nodeIndex];
            node.aabb = MakeEmptyBounds();
            if (node.IsLeaf())
            {
                for (const uint32_t triangleIndex : node.triIndices)
                {
                    if (triangleIndex >= triangles.size()) continue;
                    IncludeTriangle(node.aabb, triangles[triangleIndex]);
                }
                continue;
            }

            const auto includeChildBounds = [&](int childIndex)
            {
                if (childIndex <= nodeIndex || childIndex >= nodeCount) return;
                const AABB& childBounds = nodes[childIndex].aabb;
                /// @note 反転した空の境界を合成すると親の境界が広がる。
                if (!(childBounds.min.x <= childBounds.max.x)) return;
                IncludePoint(node.aabb, childBounds.min);
                IncludePoint(node.aabb, childBounds.max);
            };
            includeChildBounds(node.left);
            includeChildBounds(node.right);
        }
    }
}
