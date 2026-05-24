// FBZZ Engine
// ConvexHullCollider.cpp | fbzz::physics
// Quickhull による凸包構築と GJK サポート関数
#include <Physics/ConvexHullCollider.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace fbzz::physics
{

    namespace
    {
        // 点 p から直線 (a, b) への符号付き距離の 2 倍 (方向成分のみ)
        float SignedDistToLine2D(const math::Vector3& a, const math::Vector3& b,
                                 const math::Vector3& p, const math::Vector3& planeNormal)
        {
            return math::Vector3::Dot(math::Vector3::Cross(b - a, p - a), planeNormal);
        }

        // 三角形の法線方向に対して正の側にある点を返す
        float DistToPlane(const math::Vector3& planeNormal, const math::Vector3& planePoint,
                          const math::Vector3& p)
        {
            return math::Vector3::Dot(planeNormal, p - planePoint);
        }

        struct HullFace
        {
            int v[3];
            math::Vector3 normal;
        };

        math::Vector3 FaceNormal(const std::vector<math::Vector3>& pts, const HullFace& f)
        {
            const math::Vector3 e1 = pts[f.v[1]] - pts[f.v[0]];
            const math::Vector3 e2 = pts[f.v[2]] - pts[f.v[0]];
            math::Vector3 n = math::Vector3::Cross(e1, e2);
            const float len = n.Length();
            return len > 1e-8f ? n * (1.0f / len) : math::Vector3::UP;
        }

        // 簡易 Quickhull: 全点を処理して凸包頂点インデックスを返す
        // ここでは Incremental 法（面を追加しながら外部点を処理）を簡略実装する
        std::vector<math::Vector3> SimpleQuickhull(std::vector<math::Vector3> pts)
        {
            const int n = static_cast<int>(pts.size());
            if (n <= 4) return pts;

            // 初期四面体を構築する
            // 最遠 X 軸ペアを選ぶ
            int minX = 0, maxX = 0;
            for (int i = 1; i < n; ++i)
            {
                if (pts[i].x < pts[minX].x) minX = i;
                if (pts[i].x > pts[maxX].x) maxX = i;
            }
            if (minX == maxX) return pts;

            // 直線 (minX, maxX) から最遠点
            int far1 = -1;
            float bestDist = -1.0f;
            for (int i = 0; i < n; ++i)
            {
                if (i == minX || i == maxX) continue;
                const math::Vector3 d = math::Vector3::Cross(
                    pts[maxX] - pts[minX], pts[i] - pts[minX]);
                const float dist = d.Length();
                if (dist > bestDist) { bestDist = dist; far1 = i; }
            }
            if (far1 < 0) return pts;

            // 平面 (minX, maxX, far1) から最遠点
            const math::Vector3 triN = math::Vector3::Cross(
                pts[maxX] - pts[minX], pts[far1] - pts[minX]).Normalized();
            int far2 = -1;
            bestDist = -1.0f;
            for (int i = 0; i < n; ++i)
            {
                if (i == minX || i == maxX || i == far1) continue;
                const float dist = std::abs(DistToPlane(triN, pts[minX], pts[i]));
                if (dist > bestDist) { bestDist = dist; far2 = i; }
            }
            if (far2 < 0) return pts;

            // 4 点から凸包を構築する (簡易: 全点に対して外側テスト)
            const std::vector<int> initIdx = {minX, maxX, far1, far2};
            std::vector<HullFace> faces;

            // 4 面の三角形を作成する
            const int idx[4] = {minX, maxX, far1, far2};
            const int triIdx[4][3] = {{0,1,2},{0,2,3},{0,3,1},{1,3,2}};
            math::Vector3 centroid = (pts[idx[0]] + pts[idx[1]] + pts[idx[2]] + pts[idx[3]]) * 0.25f;

            for (const auto& ti : triIdx)
            {
                HullFace f;
                f.v[0] = idx[ti[0]];
                f.v[1] = idx[ti[1]];
                f.v[2] = idx[ti[2]];
                f.normal = FaceNormal(pts, f);
                // 外向き法線に修正
                if (DistToPlane(f.normal, pts[f.v[0]], centroid) > 0.0f)
                {
                    std::swap(f.v[1], f.v[2]);
                    f.normal = -f.normal;
                }
                faces.push_back(f);
            }

            // 各点を面に追加していく Incremental Quickhull
            for (int pi = 0; pi < n; ++pi)
            {
                // 既に凸包内にある点はスキップ
                bool outside = false;
                for (const auto& f : faces)
                {
                    if (DistToPlane(f.normal, pts[f.v[0]], pts[pi]) > 1e-5f)
                    {
                        outside = true;
                        break;
                    }
                }
                if (!outside) continue;

                // 見える面を削除しシルエットエッジを収集する
                struct EdgePair { int a, b; };
                std::vector<EdgePair> edges;
                for (int fi = static_cast<int>(faces.size()) - 1; fi >= 0; --fi)
                {
                    if (DistToPlane(faces[fi].normal, pts[faces[fi].v[0]], pts[pi]) > 1e-5f)
                    {
                        for (int k = 0; k < 3; ++k)
                        {
                            EdgePair ep{faces[fi].v[k], faces[fi].v[(k+1)%3]};
                            bool dup = false;
                            for (int ei = static_cast<int>(edges.size()) - 1; ei >= 0; --ei)
                            {
                                if (edges[ei].a == ep.b && edges[ei].b == ep.a)
                                {
                                    edges.erase(edges.begin() + ei);
                                    dup = true;
                                    break;
                                }
                            }
                            if (!dup) edges.push_back(ep);
                        }
                        faces.erase(faces.begin() + fi);
                    }
                }

                // シルエットエッジから新しい面を追加する
                for (const auto& e : edges)
                {
                    HullFace f;
                    f.v[0] = e.a; f.v[1] = e.b; f.v[2] = pi;
                    f.normal = FaceNormal(pts, f);
                    if (DistToPlane(f.normal, pts[f.v[0]], centroid) > 0.0f)
                    {
                        std::swap(f.v[1], f.v[2]);
                        f.normal = -f.normal;
                    }
                    faces.push_back(f);
                }

                // 重心を更新
                centroid = (centroid * static_cast<float>(faces.size() - 1) + pts[pi])
                         * (1.0f / static_cast<float>(faces.size()));
            }

            // 使用された頂点インデックスを収集して重複を排除する
            std::vector<bool> used(n, false);
            for (const auto& f : faces)
                for (int k = 0; k < 3; ++k)
                    used[f.v[k]] = true;

            std::vector<math::Vector3> result;
            result.reserve(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i)
                if (used[i]) result.push_back(pts[i]);

            return result;
        }
    } // anonymous namespace

    ConvexHullCollider::ConvexHullCollider(std::vector<math::Vector3> points)
    {
        BuildHull(std::move(points));
        UpdateWorldVerts(math::Vector3::ZERO, math::Quaternion::Identity());
    }

    void ConvexHullCollider::BuildHull(std::vector<math::Vector3> points)
    {
        if (points.empty()) return;

        // 頂点数の上限
        if (static_cast<int>(points.size()) > MAX_HULL_VERTS)
            points.resize(static_cast<size_t>(MAX_HULL_VERTS));

        m_localVerts = SimpleQuickhull(std::move(points));

        // 上限を超えた場合はさらにトリミング (精度よりも安全を優先)
        if (static_cast<int>(m_localVerts.size()) > MAX_HULL_VERTS)
            m_localVerts.resize(static_cast<size_t>(MAX_HULL_VERTS));
    }

    void ConvexHullCollider::Update(const math::Vector3& worldPos,
                                    const math::Quaternion& worldRot)
    {
        UpdateWorldVerts(worldPos, worldRot);
    }

    void ConvexHullCollider::UpdateWorldVerts(const math::Vector3& pos,
                                               const math::Quaternion& rot)
    {
        m_worldCenter = pos;
        m_worldRot    = rot;
        m_worldVerts.resize(m_localVerts.size());

        m_worldAABB.min = { std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max() };
        m_worldAABB.max = { std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest() };

        for (size_t i = 0; i < m_localVerts.size(); ++i)
        {
            m_worldVerts[i] = pos + rot * m_localVerts[i];

            m_worldAABB.min.x = std::min(m_worldAABB.min.x, m_worldVerts[i].x);
            m_worldAABB.min.y = std::min(m_worldAABB.min.y, m_worldVerts[i].y);
            m_worldAABB.min.z = std::min(m_worldAABB.min.z, m_worldVerts[i].z);
            m_worldAABB.max.x = std::max(m_worldAABB.max.x, m_worldVerts[i].x);
            m_worldAABB.max.y = std::max(m_worldAABB.max.y, m_worldVerts[i].y);
            m_worldAABB.max.z = std::max(m_worldAABB.max.z, m_worldVerts[i].z);
        }
    }

    math::Vector3 ConvexHullCollider::SupportPoint(const math::Vector3& dir) const
    {
        if (m_worldVerts.empty()) return m_worldCenter;

        float bestDot = -std::numeric_limits<float>::max();
        math::Vector3 best = m_worldVerts[0];
        for (const auto& v : m_worldVerts)
        {
            const float d = math::Vector3::Dot(v, dir);
            if (d > bestDot)
            {
                bestDot = d;
                best    = v;
            }
        }
        return best;
    }

    math::Vector3 ConvexHullCollider::SupportFnImpl(const void* shape, const math::Vector3& dir)
    {
        return static_cast<const ConvexHullCollider*>(shape)->SupportPoint(dir);
    }

} // namespace fbzz::physics
