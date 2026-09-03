/// @file    EPA.cpp
/// @brief   Expanding Polytope Algorithm の実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Physics/EPA.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace fbzz::physics
{

    namespace
    {
        struct EPAFace
        {
            Simplex::Vertex verts[3];
            math::Vector3   normal;  // ポリトープ外向き法線 (正規化済み)
            float           dist;    // 原点からの距離
        };

        EPAFace MakeFace(const Simplex::Vertex& a,
                         const Simplex::Vertex& b,
                         const Simplex::Vertex& c)
        {
            EPAFace f;
            f.verts[0] = a;
            f.verts[1] = b;
            f.verts[2] = c;
            const math::Vector3 n = math::Vector3::Cross(b.point - a.point, c.point - a.point);
            const float lenSq = n.LengthSq();
            if (lenSq < 1e-12f)
            {
                // 面積が潰れた面は法線を定義できない。
                // WHY 距離を 0 でなく最遠にするか: 0 にすると «原点に最も近い面» として
                //     毎回選ばれ、同じ方向のサポート点を足しては面を増やす無限膨張になる。
                //     軸に揃った対称な配置 (球どうしを真横に重ねる等) で実際に起きる。
                //     選ばれない距離に置き、実体のある面だけで拡張を進ませる。
                f.normal = math::Vector3::UP;
                f.dist   = std::numeric_limits<float>::max();
            }
            else
            {
                f.normal = n * (1.0f / std::sqrt(lenSq));
                f.dist   = math::Vector3::Dot(f.normal, a.point);
                if (f.dist < 0.0f)
                {
                    std::swap(f.verts[1], f.verts[2]);
                    f.normal = -f.normal;
                    f.dist = -f.dist;
                }
            }
            return f;
        }

        // ポリトープのエッジ: EPA 拡張時に削除する面のエッジを保持
        struct Edge
        {
            Simplex::Vertex a, b;
        };

        void AddEdgeOrRemoveDuplicate(std::vector<Edge>& edges, const Simplex::Vertex& a, const Simplex::Vertex& b)
        {
            // 逆向きの同一エッジが既にあれば両方削除 (Morrison's rule)
            for (int i = static_cast<int>(edges.size()) - 1; i >= 0; --i)
            {
                const auto& e = edges[i];
                if ((e.a.point - b.point).LengthSq() < 1e-10f &&
                    (e.b.point - a.point).LengthSq() < 1e-10f)
                {
                    edges.erase(edges.begin() + i);
                    return;
                }
            }
            edges.push_back({a, b});
        }

        // バリセントリック座標で接触点を補間する
        void BarycentricContact(const EPAFace& face,
                                math::Vector3& outA,
                                math::Vector3& outB)
        {
            // 原点を面に投影し、バリセントリック座標を計算する
            const math::Vector3& a = face.verts[0].point;
            const math::Vector3& b = face.verts[1].point;
            const math::Vector3& c = face.verts[2].point;
            const math::Vector3  p = face.normal * face.dist; // 面への投影点

            const math::Vector3 ab = b - a;
            const math::Vector3 ac = c - a;
            const math::Vector3 ap = p - a;

            const float d00 = math::Vector3::Dot(ab, ab);
            const float d01 = math::Vector3::Dot(ab, ac);
            const float d11 = math::Vector3::Dot(ac, ac);
            const float d20 = math::Vector3::Dot(ap, ab);
            const float d21 = math::Vector3::Dot(ap, ac);
            const float denom = d00 * d11 - d01 * d01;

            float v = 0.0f, w = 0.0f;
            if (std::abs(denom) > 1e-10f)
            {
                v = (d11 * d20 - d01 * d21) / denom;
                w = (d00 * d21 - d01 * d20) / denom;
            }
            const float u = 1.0f - v - w;

            outA = face.verts[0].suppA * u + face.verts[1].suppA * v + face.verts[2].suppA * w;
            outB = face.verts[0].suppB * u + face.verts[1].suppB * v + face.verts[2].suppB * w;
        }

        void RefineCardinalAxis(const void* shapeA, SupportFn supportA,
                                const void* shapeB, SupportFn supportB,
                                EPAResult& result)
        {
            const float ax = std::abs(result.normal.x);
            const float ay = std::abs(result.normal.y);
            const float az = std::abs(result.normal.z);
            if (std::max({ ax, ay, az }) >= 0.75f) return;

            const math::Vector3 axes[6] = {
                math::Vector3::RIGHT, -math::Vector3::RIGHT,
                math::Vector3::UP,    -math::Vector3::UP,
                math::Vector3::FORWARD, -math::Vector3::FORWARD
            };

            math::Vector3 bestNormal = result.normal;
            math::Vector3 bestA = result.contactA;
            math::Vector3 bestB = result.contactB;
            float bestDepth = std::numeric_limits<float>::max();

            for (const math::Vector3& axis : axes)
            {
                // ContactPoint::normal は「B から A」へ押し戻す向きで統一している。
                // そのため候補軸 axis の貫通量は、A の axis 反対側の面と B の axis 側の面の
                // 重なりとして測る。ここを逆にすると、床(OBB=A)の上にある Capsule(B)で
                // 浅い上向き法線を選び、ResolvePosition が Capsule を床へ押し込んでしまう。
                const math::Vector3 suppA = supportA(shapeA, -axis);
                const math::Vector3 suppB = supportB(shapeB, axis);
                const float depth = math::Vector3::Dot(suppB - suppA, axis);
                if (depth > 0.0f && depth < bestDepth)
                {
                    bestDepth = depth;
                    bestNormal = axis;
                    bestA = suppA;
                    bestB = suppB;
                }
            }

            if (bestDepth != std::numeric_limits<float>::max())
            {
                result.normal = bestNormal;
                result.depth = bestDepth;
                result.contactA = bestA;
                result.contactB = bestB;
            }
        }
    } // anonymous namespace

    EPAResult EPA_GetContactInfo(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        const Simplex& gjkSimplex,
        int maxIter, float tolerance)
    {
        EPAResult result;

        // GJK の Simplex を四面体に拡張してポリトープの初期面を作る
        // GJK 終了時の Simplex は 4 頂点でなければならない
        if (gjkSimplex.size < 4)
        {
            // 頂点が不足している場合は任意方向にサポート点を追加して補完する
            // 簡易: 代表法線方向でサポートを追加して fallback
            result.valid  = false;
            return result;
        }

        std::vector<EPAFace> faces;
        faces.reserve(64);

        const auto& v = gjkSimplex.verts;
        faces.push_back(MakeFace(v[0], v[1], v[2]));
        faces.push_back(MakeFace(v[0], v[2], v[3]));
        faces.push_back(MakeFace(v[0], v[3], v[1]));
        faces.push_back(MakeFace(v[1], v[3], v[2]));

        for (int iter = 0; iter < maxIter; ++iter)
        {
            // 原点に最も近い面を選ぶ
            int   minIdx  = 0;
            float minDist = std::numeric_limits<float>::max();
            for (int i = 0; i < static_cast<int>(faces.size()); ++i)
            {
                if (faces[i].dist < minDist)
                {
                    minDist = faces[i].dist;
                    minIdx  = i;
                }
            }

            // 実体のある面が 1 つも残っていない (すべて潰れている)。
            // ここで進むと最遠に置いた番兵の距離を貫通量として返すことになる。
            if (minDist == std::numeric_limits<float>::max())
                return result;

            const EPAFace& closestFace = faces[minIdx];
            const math::Vector3& n = closestFace.normal;

            // 新しいサポート点を追加する
            Simplex::Vertex newVert;
            newVert.suppA = supportA(shapeA,  n);
            newVert.suppB = supportB(shapeB, -n);
            newVert.point = newVert.suppA - newVert.suppB;

            const float newDist = math::Vector3::Dot(n, newVert.point);

            // 収束判定: 新しい点がほぼ面上にある
            if (newDist - minDist < tolerance)
            {
                BarycentricContact(closestFace, result.contactA, result.contactB);
                result.normal = n;
                result.depth  = minDist;
                result.valid  = true;
                RefineCardinalAxis(shapeA, supportA, shapeB, supportB, result);
                return result;
            }

            // 新しい点から見える面を削除し、シルエットエッジを収集する
            std::vector<Edge> edges;
            for (int i = static_cast<int>(faces.size()) - 1; i >= 0; --i)
            {
                const float dot = math::Vector3::Dot(faces[i].normal,
                                                     newVert.point - faces[i].verts[0].point);
                if (dot > 0.0f)
                {
                    AddEdgeOrRemoveDuplicate(edges, faces[i].verts[0], faces[i].verts[1]);
                    AddEdgeOrRemoveDuplicate(edges, faces[i].verts[1], faces[i].verts[2]);
                    AddEdgeOrRemoveDuplicate(edges, faces[i].verts[2], faces[i].verts[0]);
                    faces.erase(faces.begin() + i);
                }
            }

            // シルエットエッジから新しい面を追加する
            for (const Edge& e : edges)
                faces.push_back(MakeFace(e.a, e.b, newVert));
        }

        // maxIter 到達: 最も近い面で近似する
        if (!faces.empty())
        {
            int   minIdx  = 0;
            float minDist = std::numeric_limits<float>::max();
            for (int i = 0; i < static_cast<int>(faces.size()); ++i)
            {
                if (faces[i].dist < minDist)
                {
                    minDist = faces[i].dist;
                    minIdx  = i;
                }
            }
            // 潰れた面しか残っていない場合は近似の元が無い。無効を返す。
            if (minDist == std::numeric_limits<float>::max())
                return result;

            BarycentricContact(faces[minIdx], result.contactA, result.contactB);
            result.normal = faces[minIdx].normal;
            result.depth  = minDist;
            result.valid  = true;
            RefineCardinalAxis(shapeA, supportA, shapeB, supportB, result);
        }

        return result;
    }

} // namespace fbzz::physics
