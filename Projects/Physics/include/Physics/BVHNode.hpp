// FBZZ Engine
// BVHNode.hpp | fbzz::physics
// AABB バウンディングボリューム階層 (TriangleMeshCollider 用)
#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <Physics/Collider.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::physics
{

    struct Triangle
    {
        math::Vector3 v[3];
        math::Vector3 normal;    // 正規化済み面法線
        uint32_t      index = 0; // 元のインデックスバッファ上の三角形番号 (デバッグ用)
    };

    struct BVHNode
    {
        AABB                  aabb;
        int                   left       = -1; // 内部ノード: 左子インデックス
        int                   right      = -1; // 内部ノード: 右子インデックス (-1 なら葉ノード)
        std::vector<uint32_t> triIndices;       // 葉ノードのみ有効

        bool IsLeaf() const { return left == -1; }
    };

    // BVH ツリー本体 (フラット配列で管理)
    struct BVHTree
    {
        std::vector<BVHNode> nodes;
        std::vector<Triangle> triangles;

        // ワールド変換済み三角形リストから中点分割で BVH を構築する
        // maxLeafTris: 葉ノードの最大三角形数
        void Build(std::vector<Triangle>&& tris, int maxLeafTris = 8);

        // AABB と重なる三角形を列挙し predicate を呼ぶ (非再帰 DFS)
        // predicate: void(const Triangle&)
        template<typename Pred>
        void Query(const AABB& queryAABB, Pred&& predicate) const
        {
            if (nodes.empty()) return;

            std::array<int, 64> stack;
            int top = 0;
            stack[top++] = 0;

            while (top > 0)
            {
                const int idx = stack[--top];
                if (idx < 0 || idx >= static_cast<int>(nodes.size())) continue;
                const BVHNode& node = nodes[idx];
                if (!node.aabb.Overlaps(queryAABB)) continue;

                if (node.IsLeaf())
                {
                    for (uint32_t ti : node.triIndices)
                        predicate(triangles[ti]);
                }
                else
                {
                    // スタックオーバーフロー防止 (深さ 64 は maxLeafTris=8 で事実上到達不可)
                    if (top + 1 < static_cast<int>(stack.size()))
                    {
                        stack[top++] = node.right;
                        stack[top++] = node.left;
                    }
                }
            }
        }

    private:
        // 再帰的なノード構築。[begin, end) の三角形インデックス範囲を処理し
        // 構築したノードのインデックスを返す
        int BuildRecursive(std::vector<uint32_t>& triOrder,
                           int begin, int end, int maxLeafTris);
    };

} // namespace fbzz::physics
