/// @file    HeightFieldCollider.cpp
/// @brief   HeightFieldCollider の BVH 構築と Transform 同期。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#include <Physics/HeightFieldCollider.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace fbzz::physics
{

HeightFieldCollider::HeightFieldCollider(const std::vector<float>& heights,
                                         int rows, int cols,
                                         float cellSize, float maxHeight)
    : m_rows(rows)
    , m_cols(cols)
    , m_cellSize(cellSize)
    , m_maxHeight(maxHeight)
    , m_heights(heights)
{
    assert(rows >= 2 && cols >= 2);
    assert(static_cast<int>(heights.size()) == rows * cols);
    // WHY: BVH 三角形はワールド座標で格納するため、m_worldPos が確定する
    //      UpdateWithScale() の初回呼び出しまで構築を遅延する。
    //      ここで RebuildBVH() すると m_worldPos={0,0,0} のまま原点に三角形が配置され、
    //      その後の UpdateWithScale() が「BVH あり」と判断して位置修正をスキップしてしまう。
}

void HeightFieldCollider::Update(const math::Vector3& worldPos,
                                 const math::Quaternion& worldRot)
{
    UpdateWithScale(worldPos, worldRot, m_worldScale);
}

void HeightFieldCollider::UpdateWithScale(const math::Vector3&    worldPos,
                                          const math::Quaternion& worldRot,
                                          const math::Vector3&    worldScale)
{
    constexpr float EPS = 1e-5f;
    const bool posChanged   = (worldPos   - m_worldPos  ).LengthSq() > EPS * EPS;
    const bool scaleChanged = (worldScale - m_worldScale).LengthSq() > EPS * EPS;
    const float dotRot = std::abs(math::Quaternion::Dot(worldRot, m_worldRot));
    const bool rotChanged = dotRot < (1.0f - EPS);

    m_worldPos   = worldPos;
    m_worldRot   = worldRot;
    m_worldScale = worldScale;

    // WHY: 初回呼び出し時は transform 変化がなくても BVH を構築する必要がある。
    //      コンストラクタで構築しないため、ここが唯一の初期化パスになる。
    if (m_bvh.triangles.empty()) {
        RebuildBVH();
        return;
    }

    if (!posChanged && !rotChanged && !scaleChanged) return;

    // WHY 全再構築しないか: heightData が同じなら «どの三角形がどの葉に入るか» は
    //      transform を掛けても妥当な空間分割のままで、作り直す必要があるのは境界だけ。
    //      重心ソートを省いた分、13 万三角形でも一巡で済む。
    //
    //      以前はここで AABB «だけ» を測り直していたが、その計算は BVH に入っている
    //      ワールド座標の頂点をそのまま舐めるだけで、新しい transform をどこにも通して
    //      いなかった。結果として三角形も AABB も古い位置に居座り、エディタで地形を
    //      動かすと当たり判定だけが元の場所に残っていた。
    RefitTransform();
}

void HeightFieldCollider::Rebuild(const std::vector<float>& heights,
                                   int rows, int cols,
                                   float cellSize, float maxHeight)
{
    assert(rows >= 2 && cols >= 2);
    assert(static_cast<int>(heights.size()) == rows * cols);
    m_rows      = rows;
    m_cols      = cols;
    m_cellSize  = cellSize;
    m_maxHeight = maxHeight;
    m_heights   = heights;
    RebuildBVH();
}

void HeightFieldCollider::RebuildBVH()
{
    if (m_heights.empty() || m_rows < 2 || m_cols < 2) return;

    const int triCount = (m_rows - 1) * (m_cols - 1) * 2;
    std::vector<Triangle> tris;
    tris.reserve(static_cast<size_t>(triCount));

    m_worldAABB.min = { std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max() };
    m_worldAABB.max = { std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest() };

    uint32_t triIdx = 0;
    for (int z = 0; z < m_rows - 1; ++z) {
        for (int x = 0; x < m_cols - 1; ++x) {
            const math::Vector3 v00 = ToWorld(LocalVertex(x,     z    ));
            const math::Vector3 v10 = ToWorld(LocalVertex(x + 1, z    ));
            const math::Vector3 v01 = ToWorld(LocalVertex(x,     z + 1));
            const math::Vector3 v11 = ToWorld(LocalVertex(x + 1, z + 1));

            // 下三角 (00, 01, 10)
            {
                Triangle tri;
                tri.v[0] = v00; tri.v[1] = v01; tri.v[2] = v10;
                tri.index = triIdx++;
                const math::Vector3 e0 = tri.v[1] - tri.v[0];
                const math::Vector3 e1 = tri.v[2] - tri.v[0];
                const math::Vector3 cross = math::Vector3::Cross(e0, e1);
                if (cross.LengthSq() >= 1e-10f) {
                    tri.normal = cross.Normalized();
                    for (int k = 0; k < 3; ++k) {
                        m_worldAABB.min.x = std::min(m_worldAABB.min.x, tri.v[k].x);
                        m_worldAABB.min.y = std::min(m_worldAABB.min.y, tri.v[k].y);
                        m_worldAABB.min.z = std::min(m_worldAABB.min.z, tri.v[k].z);
                        m_worldAABB.max.x = std::max(m_worldAABB.max.x, tri.v[k].x);
                        m_worldAABB.max.y = std::max(m_worldAABB.max.y, tri.v[k].y);
                        m_worldAABB.max.z = std::max(m_worldAABB.max.z, tri.v[k].z);
                    }
                    tris.push_back(tri);
                }
            }

            // 上三角 (10, 01, 11)
            {
                Triangle tri;
                tri.v[0] = v10; tri.v[1] = v01; tri.v[2] = v11;
                tri.index = triIdx++;
                const math::Vector3 e0 = tri.v[1] - tri.v[0];
                const math::Vector3 e1 = tri.v[2] - tri.v[0];
                const math::Vector3 cross = math::Vector3::Cross(e0, e1);
                if (cross.LengthSq() >= 1e-10f) {
                    tri.normal = cross.Normalized();
                    for (int k = 0; k < 3; ++k) {
                        m_worldAABB.min.x = std::min(m_worldAABB.min.x, tri.v[k].x);
                        m_worldAABB.min.y = std::min(m_worldAABB.min.y, tri.v[k].y);
                        m_worldAABB.min.z = std::min(m_worldAABB.min.z, tri.v[k].z);
                        m_worldAABB.max.x = std::max(m_worldAABB.max.x, tri.v[k].x);
                        m_worldAABB.max.y = std::max(m_worldAABB.max.y, tri.v[k].y);
                        m_worldAABB.max.z = std::max(m_worldAABB.max.z, tri.v[k].z);
                    }
                    tris.push_back(tri);
                }
            }
        }
    }

    m_bvh.Build(std::move(tris));
}

math::Vector3 HeightFieldCollider::LocalVertex(int x, int z) const
{
    const float h = m_heights[static_cast<size_t>(z) * static_cast<size_t>(m_cols) + static_cast<size_t>(x)]
                    * m_maxHeight;
    return {
        static_cast<float>(x) * m_cellSize,
        h,
        static_cast<float>(z) * m_cellSize
    };
}

math::Vector3 HeightFieldCollider::ToWorld(const math::Vector3& local) const
{
    const math::Vector3 scaled = {
        local.x * m_worldScale.x,
        local.y * m_worldScale.y,
        local.z * m_worldScale.z
    };
    return m_worldPos + m_worldRot * scaled;
}

void HeightFieldCollider::RefitTransform()
{
    if (m_bvh.triangles.empty() || m_heights.empty() || m_rows < 2 || m_cols < 2) return;

    const int      cellsX  = m_cols - 1;
    const uint32_t cellMax = static_cast<uint32_t>(cellsX) * static_cast<uint32_t>(m_rows - 1);

    // WHY index からセル位置を戻せるか: RebuildBVH は 1 セルにつき «下三角 → 上三角» の順で
    //     index を振り、縮退三角形を捨てる場合でもカウンタは進める。BVHTree::Build は
    //     triangles の並びを変えず、葉が持つインデックスだけを並べ替える。
    //     つまり index は heightData の格子と 1:1 のままで、頂点を引き直せる。
    for (Triangle& tri : m_bvh.triangles) {
        const uint32_t cell = tri.index / 2u;
        if (cell >= cellMax) continue;

        const int x = static_cast<int>(cell % static_cast<uint32_t>(cellsX));
        const int z = static_cast<int>(cell / static_cast<uint32_t>(cellsX));

        if ((tri.index % 2u) == 0u) {
            // 下三角 (00, 01, 10)
            tri.v[0] = ToWorld(LocalVertex(x,     z    ));
            tri.v[1] = ToWorld(LocalVertex(x,     z + 1));
            tri.v[2] = ToWorld(LocalVertex(x + 1, z    ));
        } else {
            // 上三角 (10, 01, 11)
            tri.v[0] = ToWorld(LocalVertex(x + 1, z    ));
            tri.v[1] = ToWorld(LocalVertex(x,     z + 1));
            tri.v[2] = ToWorld(LocalVertex(x + 1, z + 1));
        }

        const math::Vector3 cross = math::Vector3::Cross(tri.v[1] - tri.v[0], tri.v[2] - tri.v[0]);
        // 縮退したものは BVH に入っていないが、スケール 0 を通ると一時的にここへ来る。
        // 前の法線を残す方が «向きが未定義の面» より扱いを間違えにくい。
        if (cross.LengthSq() >= 1e-10f) tri.normal = cross.Normalized();
    }

    m_bvh.Refit();
    m_worldAABB = m_bvh.nodes.empty() ? AABB{} : m_bvh.nodes[0].aabb;
}

} // namespace fbzz::physics
