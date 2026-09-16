/// @file    BVHNode.hpp
/// @brief   AABB バウンディングボリューム階層 (TriangleMeshCollider 用)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
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

    // BVH ツリー本体。再帰ポインタではなくフラット配列で管理し、再構築時の所有を単純にする。
    struct BVHTree
    {
        std::vector<BVHNode> nodes;
        std::vector<Triangle> triangles;

        // ワールド変換済み三角形リストから中点分割で BVH を構築する。
        // maxLeafTris は、葉ノードに格納する最大三角形数。
        void Build(std::vector<Triangle>&& tris, int maxLeafTris = 8);

        // triangles の頂点を書き換えた後に、木の構造はそのままノード AABB だけ測り直す。
        // WHY Build と分けるか: transform だけが変わったとき «どの三角形がどの葉に入るか» は
        //     依然として妥当な空間分割で、作り直す必要があるのは境界だけ。Build は
        //     重心ソートを伴う O(n log n) だが、こちらは葉から根への一巡で済む。
        void Refit();

        // AABB と重なる三角形だけを列挙する。非再帰 DFS にして深いメッシュでも C++ の呼び出しスタックを使わない。
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
                    {
                        // WHY: 葉ノードは複数三角形をまとめて持つため、葉 AABB が当たっても
                        //      個別三角形は queryAABB から大きく外れている場合がある。
                        //      Terrain のような大規模メッシュでは、この軽い AABB 判定で
                        //      Capsule/Triangle の最近傍計算まで進む候補数を抑える。
                        const Triangle& tri = triangles[ti];
                        AABB triAABB;
                        triAABB.min = tri.v[0];
                        triAABB.max = tri.v[0];
                        for (int k = 1; k < 3; ++k)
                        {
                            const math::Vector3& v = tri.v[k];
                            if (v.x < triAABB.min.x) triAABB.min.x = v.x;
                            if (v.y < triAABB.min.y) triAABB.min.y = v.y;
                            if (v.z < triAABB.min.z) triAABB.min.z = v.z;
                            if (v.x > triAABB.max.x) triAABB.max.x = v.x;
                            if (v.y > triAABB.max.y) triAABB.max.y = v.y;
                            if (v.z > triAABB.max.z) triAABB.max.z = v.z;
                        }

                        if (triAABB.Overlaps(queryAABB))
                            predicate(tri);
                    }
                }
                else
                {
                    // 固定長スタックで上限を明示する。maxLeafTris=8 の通常分割では深さ 64 に到達しない想定。
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
