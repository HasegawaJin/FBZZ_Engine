/// @file    EPA.cpp
/// @brief   Expanding Polytope Algorithm の実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Physics/EPA.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace fbzz::physics
{

    namespace
    {
        struct EPAFace
        {
            Simplex::Vertex verts[3];
            math::Vector3   normal;  ///< @brief ポリトープ外向きの単位法線。
            float           dist;    ///< @brief 原点からの距離。
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
                /// @note 潰れた面を距離 0 にすると同じサポート点を足し続けるため、選ばれない最遠距離に置く。
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

        /// @brief EPA 拡張時に削除する面のエッジを保持する。
        struct Edge
        {
            Simplex::Vertex a, b;
        };

        void AddEdgeOrRemoveDuplicate(std::vector<Edge>& edges, const Simplex::Vertex& a, const Simplex::Vertex& b)
        {
            /// @note 逆向きの同一エッジが既にあれば両方削除 (Morrison's rule)
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

        /// @brief バリセントリック座標で接触点を補間する。
        void BarycentricContact(const EPAFace& face,
                                math::Vector3& outA,
                                math::Vector3& outB)
        {
            /// @note 原点を面に投影し、バリセントリック座標を計算する
            const math::Vector3& a = face.verts[0].point;
            const math::Vector3& b = face.verts[1].point;
            const math::Vector3& c = face.verts[2].point;
            /// @note 面への投影点
            const math::Vector3  p = face.normal * face.dist;

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

        /// @brief ポリトープが縮退して貫通量を出せなかったとみなす閾値。
        /// @note 原点が面上に乗る深さ 0 の接触は解決に使えない。
        constexpr float kDegenerateDepth = 1.0e-5f;

        /// @brief ポリトープの面数の上限。
        /// @note 退化配置の面数増加による O(E²) のエッジ探索を、反復数とは別に制限する。
        constexpr std::size_t kMaxFaces = 256;

        void RefineCardinalAxis(const void* shapeA, SupportFn supportA,
                                const void* shapeB, SupportFn supportB,
                                EPAResult& result,
                                bool force)
        {
            const float ax = std::abs(result.normal.x);
            const float ay = std::abs(result.normal.y);
            const float az = std::abs(result.normal.z);
            /// @note force = ポリトープからは深さが出なかった。軸ごとの重なりで測り直す。
            if (!force && std::max({ ax, ay, az }) >= 0.75f) return;

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
                /// @note B→A 法線の貫通量は A の反対側の面と B の軸側の面で測り、床上の体を床へ押し込まない。
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
    } /// @note anonymous namespace

    EPAResult EPA_GetContactInfo(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        const Simplex& gjkSimplex,
        int maxIter, float tolerance)
    {
        EPAResult result;

        /// @note 初期ポリトープには 4 頂点が必要。不足する単体は接触を返さない。
        if (gjkSimplex.size < 4)
        {
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

        /// @note 収束せずに打ち切ったか。true ならポリトープの出す深さは信用しない。
        bool abandoned = false;

        for (int iter = 0; iter < maxIter; ++iter)
        {
            /// @note 縮退して面数が増え続ける場合は、その時点で最も近い面から近似値を作る。
            if (faces.size() > kMaxFaces)
            {
                abandoned = true;
                break;
            }

            /// @note 原点に最も近い面を選ぶ
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

            /// @note 潰れた面しか残っていない場合は、最遠距離の番兵を貫通量として返さない。
            if (minDist == std::numeric_limits<float>::max())
                return result;

            const EPAFace& closestFace = faces[minIdx];
            const math::Vector3& n = closestFace.normal;

            /// @note 新しいサポート点を追加する
            Simplex::Vertex newVert;
            newVert.suppA = supportA(shapeA,  n);
            newVert.suppB = supportB(shapeB, -n);
            newVert.point = newVert.suppA - newVert.suppB;

            const float newDist = math::Vector3::Dot(n, newVert.point);

            /// @note 収束判定: 新しい点がほぼ面上にある
            if (newDist - minDist < tolerance)
            {
                BarycentricContact(closestFace, result.contactA, result.contactB);
                /// @note Minkowski 差 A-B の外向き法線を反転し、A を B から押し戻す B→A 契約へ揃える。
                /// @see EPAResult::normal
                result.normal = -n;
                result.depth  = minDist;
                result.valid  = true;
                RefineCardinalAxis(shapeA, supportA, shapeB, supportB, result,
                                   minDist <= kDegenerateDepth);
                return result;
            }

            /// @note 新しい点から見える面を削除し、シルエットエッジを収集する
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

            /// @note シルエットエッジから新しい面を追加する
            for (const Edge& e : edges)
                faces.push_back(MakeFace(e.a, e.b, newVert));
        }

        /// @note maxIter 到達: 最も近い面で近似する
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
            /// @note 潰れた面しか残っていない場合は近似の元が無い。無効を返す。
            if (minDist == std::numeric_limits<float>::max())
                return result;

            BarycentricContact(faces[minIdx], result.contactA, result.contactB);
            /// @note 打ち切り時の近似法線も、Minkowski 差の外向きから B→A へ反転する。
            result.normal = -faces[minIdx].normal;
            result.depth  = minDist;
            result.valid  = true;
            /// @note 打ち切りで原点が面上に乗る深さ 0 が残る場合は、軸ごとの重なりで測り直す。
            RefineCardinalAxis(shapeA, supportA, shapeB, supportB, result,
                               abandoned || minDist <= kDegenerateDepth);
        }

        return result;
    }

} /// @note namespace fbzz::physics
