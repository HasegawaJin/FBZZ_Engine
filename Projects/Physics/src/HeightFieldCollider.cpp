// FBZZ Engine
// HeightFieldCollider.cpp | fbzz::physics
// HeightFieldCollider の BVH 構築と Transform 同期
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

    // 地形は静的前提: transform 変化時は AABB だけ更新し BVH は再構築しない。
    // WHY: 毎フレーム BVH を再構築すると TriangleMeshCollider と同コストになる。
    //      実用上 Terrain が動く場合は Rebuild() を呼ぶことで明示的に対応する。
    m_worldAABB = ComputeWorldAABB();
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

    auto SampleLocal = [&](int x, int z) -> math::Vector3 {
        const float h = m_heights[static_cast<size_t>(z) * static_cast<size_t>(m_cols) + static_cast<size_t>(x)]
                        * m_maxHeight;
        return {
            static_cast<float>(x) * m_cellSize,
            h,
            static_cast<float>(z) * m_cellSize
        };
    };

    auto ToWorld = [&](const math::Vector3& local) -> math::Vector3 {
        const math::Vector3 scaled = {
            local.x * m_worldScale.x,
            local.y * m_worldScale.y,
            local.z * m_worldScale.z
        };
        return m_worldPos + m_worldRot * scaled;
    };

    uint32_t triIdx = 0;
    for (int z = 0; z < m_rows - 1; ++z) {
        for (int x = 0; x < m_cols - 1; ++x) {
            const math::Vector3 v00 = ToWorld(SampleLocal(x,     z    ));
            const math::Vector3 v10 = ToWorld(SampleLocal(x + 1, z    ));
            const math::Vector3 v01 = ToWorld(SampleLocal(x,     z + 1));
            const math::Vector3 v11 = ToWorld(SampleLocal(x + 1, z + 1));

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

AABB HeightFieldCollider::ComputeWorldAABB() const
{
    if (m_bvh.triangles.empty()) return {};

    AABB aabb;
    aabb.min = { std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max() };
    aabb.max = { std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest() };

    // BVH 三角形の頂点を再変換して AABB を計算する（BVH の構造は維持）
    for (const Triangle& tri : m_bvh.triangles) {
        for (int k = 0; k < 3; ++k) {
            aabb.min.x = std::min(aabb.min.x, tri.v[k].x);
            aabb.min.y = std::min(aabb.min.y, tri.v[k].y);
            aabb.min.z = std::min(aabb.min.z, tri.v[k].z);
            aabb.max.x = std::max(aabb.max.x, tri.v[k].x);
            aabb.max.y = std::max(aabb.max.y, tri.v[k].y);
            aabb.max.z = std::max(aabb.max.z, tri.v[k].z);
        }
    }
    return aabb;
}

} // namespace fbzz::physics
