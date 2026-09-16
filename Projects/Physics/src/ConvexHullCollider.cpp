/// @file    ConvexHullCollider.cpp
/// @brief   Quickhull による凸包構築と GJK サポート関数。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Physics/ConvexHullCollider.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace fbzz::physics
{

    namespace
    {
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

        struct HullBuildResult
        {
            std::vector<math::Vector3> vertices;
            std::vector<std::array<uint32_t, 3>> faces;
        };

        math::Vector3 FaceNormal(const std::vector<math::Vector3>& pts, const HullFace& f)
        {
            const math::Vector3 e1 = pts[f.v[1]] - pts[f.v[0]];
            const math::Vector3 e2 = pts[f.v[2]] - pts[f.v[0]];
            math::Vector3 n = math::Vector3::Cross(e1, e2);
            const float len = n.Length();
            return len > 1e-8f ? n * (1.0f / len) : math::Vector3::UP;
        }

        // centroid が内側にある前提で、三角形の法線が外を向くように頂点順を決める。
        std::array<uint32_t, 3> OrientOutward(const std::vector<math::Vector3>& pts,
                                              uint32_t a, uint32_t b, uint32_t c,
                                              const math::Vector3& centroid)
        {
            HullFace f;
            f.v[0]   = static_cast<int>(a);
            f.v[1]   = static_cast<int>(b);
            f.v[2]   = static_cast<int>(c);
            f.normal = FaceNormal(pts, f);
            if (DistToPlane(f.normal, pts[f.v[0]], centroid) > 0.0f) std::swap(b, c);
            return { a, b, c };
        }

        // 球面に散らした budget 本の方向それぞれで «最も遠い点» を拾い、その部分集合を返す。
        //
        // WHY 入力を «並び順» で削ってはいけないか: 頂点数の上限はサポート関数の計算量の
        //     ためにあり、削るべきは凸包の結果であって入力ではない。配列の先頭から切ると、
        //     どの点が残るかが «エクスポーターの吐いた頂点順» という形とは無関係なもので
        //     決まり、実際の形より小さい当たり判定が黙って出来上がる。
        //     向きで選べば、どの方向にも最外周の点が残る。出来る凸包は必ず真の凸包に
        //     内接し (= 大きくなりすぎない)、誤差も方向によらず一様になる。
        std::vector<math::Vector3> SelectExtremePoints(const std::vector<math::Vector3>& pts,
                                                       int budget)
        {
            const int n = static_cast<int>(pts.size());

            std::vector<int> picked;
            picked.reserve(static_cast<size_t>(budget));

            // フィボナッチ球。偏りの少ない準一様分布を三角関数 2 回だけで作れる。
            constexpr float GOLDEN_ANGLE = 2.39996322972865332f;   // PI * (3 - sqrt(5))
            for (int i = 0; i < budget; ++i)
            {
                const float y = 1.0f - (static_cast<float>(i) + 0.5f) * 2.0f / static_cast<float>(budget);
                const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
                const float theta = GOLDEN_ANGLE * static_cast<float>(i);
                const math::Vector3 dir = { std::cos(theta) * r, y, std::sin(theta) * r };

                // argmax <p, dir> は原点の取り方に依存しない (全点に同じ定数が乗るだけ)。
                int   best    = 0;
                float bestDot = -std::numeric_limits<float>::max();
                for (int p = 0; p < n; ++p)
                {
                    const float d = math::Vector3::Dot(pts[p], dir);
                    if (d > bestDot) { bestDot = d; best = p; }
                }

                if (std::find(picked.begin(), picked.end(), best) == picked.end())
                    picked.push_back(best);
            }

            std::vector<math::Vector3> out;
            out.reserve(picked.size());
            for (int i : picked) out.push_back(pts[static_cast<size_t>(i)]);
            return out;
        }

        // 簡易 Quickhull: 全点を処理して凸包頂点インデックスを返す
        // ここでは Incremental 法（面を追加しながら外部点を処理）を簡略実装する
        HullBuildResult SimpleQuickhull(std::vector<math::Vector3> pts)
        {
            const int n = static_cast<int>(pts.size());
            if (n <= 4) {
                HullBuildResult result;
                result.vertices = std::move(pts);
                if (n == 4) {
                    // WHY 向きを測り直すか: 4 点の並びは呼び出し元の入力順そのままで、
                    //     どちら手の四面体かは決まっていない。固定の面リストを並べるだけだと
                    //     法線が 4 枚とも内向きになる入力がある。下の主経路と同じ基準に揃える。
                    const math::Vector3 centroid = (result.vertices[0] + result.vertices[1]
                                                  + result.vertices[2] + result.vertices[3]) * 0.25f;
                    result.faces = {
                        OrientOutward(result.vertices, 0, 1, 2, centroid),
                        OrientOutward(result.vertices, 0, 2, 3, centroid),
                        OrientOutward(result.vertices, 0, 3, 1, centroid),
                        OrientOutward(result.vertices, 1, 3, 2, centroid),
                    };
                }
                return result;
            }

            // 初期四面体を構築する
            // 最遠 X 軸ペアを選ぶ
            int minX = 0, maxX = 0;
            for (int i = 1; i < n; ++i)
            {
                if (pts[i].x < pts[minX].x) minX = i;
                if (pts[i].x > pts[maxX].x) maxX = i;
            }
            if (minX == maxX) return { std::move(pts), {} };

            // 退化を判定する長さの基準。絶対値の閾値で切ると、小さいメッシュがすべて
            // 「退化」に、大きいメッシュがすべて「非退化」になる。
            math::Vector3 lower = pts[0];
            math::Vector3 upper = pts[0];
            for (const math::Vector3& p : pts)
            {
                lower = { std::min(lower.x, p.x), std::min(lower.y, p.y), std::min(lower.z, p.z) };
                upper = { std::max(upper.x, p.x), std::max(upper.y, p.y), std::max(upper.z, p.z) };
            }
            const float degenerateEps =
                std::max({ upper.x - lower.x, upper.y - lower.y, upper.z - lower.z }) * 1e-6f;

            // 直線 (minX, maxX) から最遠点
            // WHY 外積の長さではなく直線からの距離で比べるか: 外積の長さは軸の長さに比例するので、
            //     同じ閾値が形の大きさで意味を変えてしまう。
            const math::Vector3 axis       = pts[maxX] - pts[minX];
            const float         axisLength = axis.Length();
            int far1 = -1;
            float bestDist = degenerateEps;
            for (int i = 0; i < n; ++i)
            {
                if (i == minX || i == maxX) continue;
                const float dist =
                    math::Vector3::Cross(axis, pts[i] - pts[minX]).Length() / axisLength;
                if (dist > bestDist) { bestDist = dist; far1 = i; }
            }
            // 全点が一直線。三角形が作れないので面は持たず、点だけを凸包として返す
            // (サポート関数は点の集合だけで正しく解ける)。
            // WHY ここで降りるか: 進むと下の正規化が長さ 0 の外積を踏む。
            //     板ポリのメッシュや潰れたスケールから、この点群は実データで普通に来る。
            if (far1 < 0) return { std::move(pts), {} };

            // 平面 (minX, maxX, far1) から最遠点
            // WHY Normalized() ではなく NormalizedOr か: 上の閾値は «最も条件の良い 3 点目» を
            //     選ぶためのもので、外積の長さが正規化に耐えることまでは保証しない。
            //     Normalized() は長さ 0 を契約違反として assert で落とすので、ここは踏めない。
            const math::Vector3 triN = math::Vector3::Cross(axis, pts[far1] - pts[minX])
                                           .NormalizedOr(math::Vector3::ZERO);
            if (triN.LengthSq() < 0.5f) return { std::move(pts), {} };
            int far2 = -1;
            bestDist = degenerateEps;
            for (int i = 0; i < n; ++i)
            {
                if (i == minX || i == maxX || i == far1) continue;
                const float dist = std::abs(DistToPlane(triN, pts[minX], pts[i]));
                if (dist > bestDist) { bestDist = dist; far2 = i; }
            }
            // 全点が同一平面。厚みの無い四面体からは外向き法線が決まらない。
            if (far2 < 0) return { std::move(pts), {} };

            // 4 点から凸包を構築する (簡易: 全点に対して外側テスト)
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
            std::vector<int> remap(static_cast<size_t>(n), -1);
            for (int i = 0; i < n; ++i) {
                if (!used[i]) continue;
                remap[static_cast<size_t>(i)] = static_cast<int>(result.size());
                result.push_back(pts[i]);
            }

            std::vector<std::array<uint32_t, 3>> resultFaces;
            resultFaces.reserve(faces.size());
            for (const auto& f : faces) {
                const int a = remap[static_cast<size_t>(f.v[0])];
                const int b = remap[static_cast<size_t>(f.v[1])];
                const int c = remap[static_cast<size_t>(f.v[2])];
                if (a < 0 || b < 0 || c < 0) continue;
                resultFaces.push_back({
                    static_cast<uint32_t>(a),
                    static_cast<uint32_t>(b),
                    static_cast<uint32_t>(c)
                });
            }

            return { std::move(result), std::move(resultFaces) };
        }
    } // anonymous namespace

    ConvexHullCollider::ConvexHullCollider(std::vector<math::Vector3> points)
    {
        BuildHull(std::move(points));
        UpdateWorldVerts(math::Vector3::ZERO, math::Quaternion::Identity(), { 1.0f, 1.0f, 1.0f });
    }

    void ConvexHullCollider::BuildHull(std::vector<math::Vector3> points)
    {
        if (points.empty()) return;

        // 頂点数の上限は «どの点を凸包の材料にするか» で掛ける。
        // SimpleQuickhull は入力より多い頂点を返さないので、これで結果も上限に収まる。
        if (static_cast<int>(points.size()) > MAX_HULL_VERTS)
            points = SelectExtremePoints(points, MAX_HULL_VERTS);

        // WHY 後から m_localVerts だけ切り詰めないか: m_faces は切り詰め前のインデックスを
        //     持ったままになり、World.cpp の RayConvexHull が範囲チェックなしで
        //     verts[face[0]] を引くため範囲外読み取りになる。頂点と面は必ず
        //     同じ SimpleQuickhull の出力から受け取り、後から片方だけ触らない。
        HullBuildResult hull = SimpleQuickhull(std::move(points));
        m_localVerts = std::move(hull.vertices);
        m_faces      = std::move(hull.faces);
    }

    void ConvexHullCollider::Update(const math::Vector3& worldPos,
                                    const math::Quaternion& worldRot)
    {
        UpdateWithScale(worldPos, worldRot, m_worldScale);
    }

    void ConvexHullCollider::UpdateWithScale(const math::Vector3& worldPos,
                                             const math::Quaternion& worldRot,
                                             const math::Vector3& worldScale)
    {
        UpdateWorldVerts(worldPos, worldRot, worldScale);
    }

    void ConvexHullCollider::UpdateWorldVerts(const math::Vector3& pos,
                                              const math::Quaternion& rot,
                                              const math::Vector3& scale)
    {
        m_worldCenter = pos;
        m_worldRot    = rot;
        m_worldScale  = scale;
        m_worldVerts.resize(m_localVerts.size());

        m_worldAABB.min = { std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max() };
        m_worldAABB.max = { std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest() };

        for (size_t i = 0; i < m_localVerts.size(); ++i)
        {
            const math::Vector3 scaled = {
                m_localVerts[i].x * scale.x,
                m_localVerts[i].y * scale.y,
                m_localVerts[i].z * scale.z
            };
            m_worldVerts[i] = pos + rot * scaled;

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
