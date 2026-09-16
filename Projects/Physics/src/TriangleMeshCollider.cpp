/// @file    TriangleMeshCollider.cpp
/// @brief   BVH 構築と TriangleMeshCollider の Update 実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Physics/TriangleMeshCollider.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <numeric>

namespace fbzz::physics
{

    // ---------------------------------------------------------------- BVHTree

    void BVHTree::Build(std::vector<Triangle>&& tris, int maxLeafTris)
    {
        triangles = std::move(tris);
        nodes.clear();

        if (triangles.empty()) return;

        std::vector<uint32_t> order(triangles.size());
        std::iota(order.begin(), order.end(), 0u);

        BuildRecursive(order, 0, static_cast<int>(order.size()), maxLeafTris);
    }

    void BVHTree::Refit()
    {
        if (nodes.empty() || triangles.empty()) return;

        auto reset = [](AABB& box)
        {
            box.min = { std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max() };
            box.max = { std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest() };
        };
        auto expand = [](AABB& box, const math::Vector3& p)
        {
            box.min.x = std::min(box.min.x, p.x);
            box.min.y = std::min(box.min.y, p.y);
            box.min.z = std::min(box.min.z, p.z);
            box.max.x = std::max(box.max.x, p.x);
            box.max.y = std::max(box.max.y, p.y);
            box.max.z = std::max(box.max.z, p.z);
        };
        // 三角形を 1 つも含まないノードは «反転した» AABB のままにする。そのまま親へ
        // 混ぜると親が無限大に広がるため、有効な子だけを取り込む。
        auto valid = [](const AABB& box) { return box.min.x <= box.max.x; };

        const int nodeCount = static_cast<int>(nodes.size());
        // WHY 末尾から前へ舐めるか: BuildRecursive は親を emplace してから子を再帰構築する。
        //     子のインデックスは必ず親より大きいので、逆順の一巡で «子が先に確定する»
        //     葉から根への順序になる。明示スタックも再帰も要らない。
        for (int i = nodeCount - 1; i >= 0; --i)
        {
            BVHNode& node = nodes[i];
            reset(node.aabb);

            if (node.IsLeaf())
            {
                for (uint32_t ti : node.triIndices)
                {
                    if (ti >= triangles.size()) continue;
                    for (int k = 0; k < 3; ++k)
                        expand(node.aabb, triangles[ti].v[k]);
                }
                continue;
            }

            auto mergeChild = [&](int child)
            {
                if (child <= i || child >= nodeCount) return;
                const AABB& childBox = nodes[child].aabb;
                if (!valid(childBox)) return;
                expand(node.aabb, childBox.min);
                expand(node.aabb, childBox.max);
            };
            mergeChild(node.left);
            mergeChild(node.right);
        }
    }

    int BVHTree::BuildRecursive(std::vector<uint32_t>& triOrder,
                                int begin, int end, int maxLeafTris)
    {
        const int nodeIdx = static_cast<int>(nodes.size());
        nodes.emplace_back();
        BVHNode& node = nodes[nodeIdx];

        // このノードの AABB を計算する
        node.aabb.min = { std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max(),
                          std::numeric_limits<float>::max() };
        node.aabb.max = { std::numeric_limits<float>::lowest(),
                          std::numeric_limits<float>::lowest(),
                          std::numeric_limits<float>::lowest() };

        for (int i = begin; i < end; ++i)
        {
            const Triangle& tri = triangles[triOrder[i]];
            for (int k = 0; k < 3; ++k)
            {
                node.aabb.min.x = std::min(node.aabb.min.x, tri.v[k].x);
                node.aabb.min.y = std::min(node.aabb.min.y, tri.v[k].y);
                node.aabb.min.z = std::min(node.aabb.min.z, tri.v[k].z);
                node.aabb.max.x = std::max(node.aabb.max.x, tri.v[k].x);
                node.aabb.max.y = std::max(node.aabb.max.y, tri.v[k].y);
                node.aabb.max.z = std::max(node.aabb.max.z, tri.v[k].z);
            }
        }

        const int count = end - begin;

        // 葉ノード条件: 三角形数が閾値以下 or 再帰深さ制限
        if (count <= maxLeafTris)
        {
            node.triIndices.reserve(static_cast<size_t>(count));
            for (int i = begin; i < end; ++i)
                node.triIndices.push_back(triOrder[i]);
            return nodeIdx;
        }

        // 分割軸: AABB の最長軸を選択
        const math::Vector3 extent = node.aabb.max - node.aabb.min;
        int axis = 0;
        if (extent.y > extent.x) axis = 1;
        if (extent.z > (axis == 0 ? extent.x : extent.y)) axis = 2;

        // 中点分割: 重心の軸座標でソート
        const float mid = (axis == 0) ? (node.aabb.min.x + node.aabb.max.x) * 0.5f
                        : (axis == 1) ? (node.aabb.min.y + node.aabb.max.y) * 0.5f
                                      : (node.aabb.min.z + node.aabb.max.z) * 0.5f;

        auto axisCoord = [](const math::Vector3& v, int ax) -> float
        {
            if (ax == 0) return v.x;
            if (ax == 1) return v.y;
            return v.z;
        };

        auto pivot = std::partition(triOrder.begin() + begin, triOrder.begin() + end,
            [&](uint32_t ti)
            {
                const Triangle& t = triangles[ti];
                const float centroid = (axisCoord(t.v[0], axis)
                                      + axisCoord(t.v[1], axis)
                                      + axisCoord(t.v[2], axis)) / 3.0f;
                return centroid < mid;
            });

        int splitIdx = static_cast<int>(pivot - triOrder.begin());

        // 全て片側に偏った場合は均等分割にフォールバック
        if (splitIdx == begin || splitIdx == end)
            splitIdx = begin + count / 2;

        // 子を再帰構築 (nodes が push_back されると参照が無効になるため、インデックスで管理)
        const int leftIdx  = BuildRecursive(triOrder, begin,    splitIdx, maxLeafTris);
        const int rightIdx = BuildRecursive(triOrder, splitIdx, end,      maxLeafTris);

        // ここで nodes[nodeIdx] の参照を再取得する (再帰で vector が拡張されている可能性)
        nodes[nodeIdx].left  = leftIdx;
        nodes[nodeIdx].right = rightIdx;

        return nodeIdx;
    }

    // -------------------------------------------------------- TriangleMeshCollider

    TriangleMeshCollider::TriangleMeshCollider(const std::vector<math::Vector3>& positions,
                                               const std::vector<uint32_t>&      indices)
        : m_localPositions(positions)
        , m_indices(indices)
    {
        // 端数は «回復可能なエラー» として捨てる。インデックスの出どころは
        // インポートしたアセットとスクリプトの手続きメッシュで、どちらもプログラマーが
        // 書いた値ではない ── 壊れた 1 ファイルでプロセスごと落とす種類の誤りではない。
        // ここで切っておかないと BVH (indices/3) と m_indices が食い違う。
        m_indices.resize(m_indices.size() - m_indices.size() % 3);

        // 初期状態: スケール 1、原点に配置
        RebuildBVH(math::Vector3::ZERO, math::Quaternion::Identity(), {1.0f, 1.0f, 1.0f});
    }

    void TriangleMeshCollider::Update(const math::Vector3& worldPos,
                                      const math::Quaternion& worldRot)
    {
        UpdateWithScale(worldPos, worldRot, m_lastScale);
    }

    void TriangleMeshCollider::UpdateWithScale(const math::Vector3&    worldPos,
                                               const math::Quaternion& worldRot,
                                               const math::Vector3&    worldScale)
    {
        // 変化がない場合は再構築をスキップする
        constexpr float EPS = 1e-5f;
        const bool posChanged   = (worldPos   - m_lastPos  ).LengthSq()  > EPS * EPS;
        const bool scaleChanged = (worldScale - m_lastScale).LengthSq()  > EPS * EPS;
        // クォータニオンの差分は dot で近似
        const float dotRot = std::abs(math::Quaternion::Dot(worldRot, m_lastRot));
        const bool rotChanged = dotRot < (1.0f - EPS);

        if (!posChanged && !rotChanged && !scaleChanged) return;

        // スケールが変わらない移動・回転は剛体変換で、三角形どうしの相対配置は変わらない。
        // 分割をやり直す理由がないので、木はそのままに頂点を置き直して AABB を refit する。
        // 重心ソートと三角形リストの作り直しが丸ごと消え、O(n log n) が一巡の O(n) になる。
        //
        // WHY スケール変化を除くか: (1,1,1) → (1,1,100) のような変形は三角形どうしの相対配置
        //     ごと変えるため、構築時に選んだ分割軸が的外れなまま残る。さらに軸のどれかが 0 に
        //     なると縮退する三角形の集合まで変わり、BVH に «面積のない三角形» が居座る。
        if (!scaleChanged && !m_bvh.triangles.empty())
        {
            RefitTransform(worldPos, worldRot);
            return;
        }

        RebuildBVH(worldPos, worldRot, worldScale);
    }

    void TriangleMeshCollider::RefitTransform(const math::Vector3& pos, const math::Quaternion& rot)
    {
        m_lastPos = pos;
        m_lastRot = rot;

        // RebuildBVH は tri.index に «元のインデックスバッファ上の三角形番号» を入れ、
        // 縮退を捨てる場合も番号は飛ばさない。BVHTree::Build は triangles の並びを
        // 変えないので、index から頂点を引き直せる。
        for (Triangle& tri : m_bvh.triangles)
        {
            const size_t base = static_cast<size_t>(tri.index) * 3;
            if (base + 3 > m_indices.size()) continue;

            for (int k = 0; k < 3; ++k)
            {
                const uint32_t vi = m_indices[base + k];
                if (vi >= m_localPositions.size()) continue;

                const math::Vector3& local = m_localPositions[vi];
                const math::Vector3  scaled = {
                    local.x * m_lastScale.x,
                    local.y * m_lastScale.y,
                    local.z * m_lastScale.z
                };
                tri.v[k] = pos + rot * scaled;
            }

            // 剛体変換なので縮退しようがないが、頂点から引き直す方が
            // «前の法線を回す» より m_lastRot の更新順に依存しない。
            const math::Vector3 cross = math::Vector3::Cross(tri.v[1] - tri.v[0], tri.v[2] - tri.v[0]);
            if (cross.LengthSq() >= 1e-10f) tri.normal = cross.Normalized();
        }

        m_bvh.Refit();
        m_worldAABB = m_bvh.nodes.empty() ? AABB{} : m_bvh.nodes[0].aabb;
    }

    void TriangleMeshCollider::RebuildBVH(const math::Vector3&    pos,
                                          const math::Quaternion& rot,
                                          const math::Vector3&    scale)
    {
        m_lastPos   = pos;
        m_lastRot   = rot;
        m_lastScale = scale;

        const size_t triCount = m_indices.size() / 3;
        std::vector<Triangle> tris;
        tris.reserve(triCount);

        m_worldAABB.min = { std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max() };
        m_worldAABB.max = { std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest() };

        for (size_t i = 0; i < triCount; ++i)
        {
            Triangle tri;
            tri.index = static_cast<uint32_t>(i);

            for (int k = 0; k < 3; ++k)
            {
                const uint32_t vi = m_indices[i * 3 + k];
                assert(vi < m_localPositions.size());

                // スケール → 回転 → 平行移動
                const math::Vector3 scaled = {
                    m_localPositions[vi].x * scale.x,
                    m_localPositions[vi].y * scale.y,
                    m_localPositions[vi].z * scale.z
                };
                tri.v[k] = pos + rot * scaled;

                m_worldAABB.min.x = std::min(m_worldAABB.min.x, tri.v[k].x);
                m_worldAABB.min.y = std::min(m_worldAABB.min.y, tri.v[k].y);
                m_worldAABB.min.z = std::min(m_worldAABB.min.z, tri.v[k].z);
                m_worldAABB.max.x = std::max(m_worldAABB.max.x, tri.v[k].x);
                m_worldAABB.max.y = std::max(m_worldAABB.max.y, tri.v[k].y);
                m_worldAABB.max.z = std::max(m_worldAABB.max.z, tri.v[k].z);
            }

            // 面法線を計算し縮退三角形をスキップ
            const math::Vector3 e0 = tri.v[1] - tri.v[0];
            const math::Vector3 e1 = tri.v[2] - tri.v[0];
            const math::Vector3 cross = math::Vector3::Cross(e0, e1);
            if (cross.LengthSq() < 1e-10f) continue; // 縮退

            tri.normal = cross.Normalized();
            tris.push_back(tri);
        }

        m_bvh.Build(std::move(tris));
    }

} // namespace fbzz::physics
