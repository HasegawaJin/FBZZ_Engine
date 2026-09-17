/// @file    GJK.cpp
/// @brief   Gilbert-Johnson-Keerthi アルゴリズムの実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Physics/GJK.hpp>
#include <cmath>
#include <limits>

namespace fbzz::physics
{

    namespace
    {
        /// 交差確定後、EPA 用に simplex を四面体まで拡張する
        void ExpandToTetrahedron(Simplex& s,
                                  const void* shapeA, SupportFn supportA,
                                  const void* shapeB, SupportFn supportB)
        {
            auto MakeVertex = [&](const math::Vector3& d) -> Simplex::Vertex
            {
                const float len = d.Length();
                const math::Vector3 dn = len > 1e-10f ? d * (1.0f / len) : math::Vector3::UP;
                Simplex::Vertex v;
                v.suppA = supportA(shapeA, dn);
                v.suppB = supportB(shapeB, { -dn.x, -dn.y, -dn.z });
                v.point = v.suppA - v.suppB;
                return v;
            };

            /// @note 線分 → 三角形: ab に垂直な軸でサポート点を追加
            if (s.size == 2)
            {
                const math::Vector3 ab = s.verts[0].point - s.verts[1].point;
                const math::Vector3 aux = (std::abs(ab.x) < 0.57f)
                                        ? math::Vector3::RIGHT : math::Vector3::UP;
                s.Add(MakeVertex(math::Vector3::Cross(ab, aux)));
            }

            /// @note 三角形 → 四面体: 面法線方向でサポート点を追加
            if (s.size == 3)
            {
                const math::Vector3 ab = s.verts[1].point - s.verts[0].point;
                const math::Vector3 ac = s.verts[2].point - s.verts[0].point;
                const math::Vector3 n  = math::Vector3::Cross(ab, ac);
                Simplex::Vertex v = MakeVertex(n);
                /// @note 同一平面上なら逆方向を試す
                if (std::abs(math::Vector3::Dot(v.point - s.verts[0].point, n)) < 1e-6f)
                    v = MakeVertex({ -n.x, -n.y, -n.z });
                s.Add(v);
            }
        }


        /// 3D の線分単体 (2 頂点) から原点への最短方向を求める
        bool DoSimplexLine(Simplex& s, math::Vector3& dir)
        {
            const math::Vector3 b = s.verts[0].point;
            /// @note 最後に追加した点
            const math::Vector3 a = s.verts[1].point;
            const math::Vector3 ab = b - a;
            const math::Vector3 ao = -a;

            if (math::Vector3::Dot(ab, ao) > 0.0f)
            {
                const math::Vector3 perp = math::Vector3::Cross(math::Vector3::Cross(ab, ao), ab);
                /// @note ab と ao が平行 (原点がセグメント上) → 内包
                if (perp.LengthSq() < 1e-10f) return true;
                dir = perp;
            }
            else
            {
                s.Set(s.verts[1]);
                dir = ao;
            }
            return false;
        }

        /// 三角形単体 (3 頂点) から原点への最短方向を求める
        bool DoSimplexTriangle(Simplex& s, math::Vector3& dir)
        {
            const math::Vector3 a  = s.verts[2].point;
            const math::Vector3 b  = s.verts[1].point;
            const math::Vector3 c  = s.verts[0].point;
            const math::Vector3 ab = b - a;
            const math::Vector3 ac = c - a;
            const math::Vector3 ao = -a;
            const math::Vector3 abc = math::Vector3::Cross(ab, ac);

            if (math::Vector3::Dot(math::Vector3::Cross(abc, ac), ao) > 0.0f)
            {
                if (math::Vector3::Dot(ac, ao) > 0.0f)
                {
                    s.Set(s.verts[0], s.verts[2]);
                    dir = math::Vector3::Cross(math::Vector3::Cross(ac, ao), ac);
                }
                else
                {
                    s.Set(s.verts[1], s.verts[2]);
                    return DoSimplexLine(s, dir);
                }
            }
            else if (math::Vector3::Dot(math::Vector3::Cross(ab, abc), ao) > 0.0f)
            {
                s.Set(s.verts[1], s.verts[2]);
                return DoSimplexLine(s, dir);
            }
            else
            {
                if (math::Vector3::Dot(abc, ao) > 0.0f)
                {
                    dir = abc;
                }
                else
                {
                    s.Set(s.verts[2], s.verts[1], s.verts[0]);
                    dir = -abc;
                }
            }
            return false;
        }

        /// 四面体単体 (4 頂点) から原点を含むか判定し含まなければ方向を更新する
        bool DoSimplexTetrahedron(Simplex& s, math::Vector3& dir)
        {
            const math::Vector3 a  = s.verts[3].point;
            const math::Vector3 b  = s.verts[2].point;
            const math::Vector3 c  = s.verts[1].point;
            const math::Vector3 d  = s.verts[0].point;
            const math::Vector3 ab = b - a;
            const math::Vector3 ac = c - a;
            const math::Vector3 ad = d - a;
            const math::Vector3 ao = -a;

            const math::Vector3 abc = math::Vector3::Cross(ab, ac);
            const math::Vector3 acd = math::Vector3::Cross(ac, ad);
            const math::Vector3 adb = math::Vector3::Cross(ad, ab);

            if (math::Vector3::Dot(abc, ao) > 0.0f)
            {
                s.Set(s.verts[1], s.verts[2], s.verts[3]);
                return DoSimplexTriangle(s, dir);
            }
            if (math::Vector3::Dot(acd, ao) > 0.0f)
            {
                s.Set(s.verts[0], s.verts[1], s.verts[3]);
                return DoSimplexTriangle(s, dir);
            }
            if (math::Vector3::Dot(adb, ao) > 0.0f)
            {
                s.Set(s.verts[0], s.verts[2], s.verts[3]);
                return DoSimplexTriangle(s, dir);
            }
            /// @note 原点が四面体の内部
            return true;
        }

        /// 非交差時の最近傍点
        /// GJK が «これ以上原点に近づけない» と判断した時点の単体は、Minkowski 差空間で
        /// 原点に最も近い «特徴» (点・辺・面) そのものになっている。その上で原点に最も
        /// 近い点を重心座標で表し、同じ重みで suppA / suppB を混ぜると、元の 2 形状の上の
        /// 最近傍点が復元できる ─ 差空間の点が suppA - suppB の線形結合だから。

        struct Barycentric
        {
            float weight[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        };

        /// 線分 ab 上で原点に最も近い点の重み。
        void ClosestOnSegment(const math::Vector3& a, const math::Vector3& b,
                              float& outWa, float& outWb)
        {
            const math::Vector3 ab = b - a;
            const float lenSq = ab.LengthSq();
            if (lenSq < 1e-20f)
            {
                outWa = 1.0f;
                outWb = 0.0f;
                return;
            }

            float t = math::Vector3::Dot(-a, ab) / lenSq;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            outWa = 1.0f - t;
            outWb = t;
        }

        /// 三角形 abc 上で原点に最も近い点の重心座標 (Ericson の領域判定)。
        /// 辺や頂点の外側に落ちる場合もそのまま扱えるので、面の内外で分岐を書かずに済む。
        void ClosestOnTriangle(const math::Vector3& a, const math::Vector3& b,
                               const math::Vector3& c,
                               float& outWa, float& outWb, float& outWc)
        {
            const math::Vector3 ab = b - a;
            const math::Vector3 ac = c - a;

            const float d1 = math::Vector3::Dot(ab, -a);
            const float d2 = math::Vector3::Dot(ac, -a);
            if (d1 <= 0.0f && d2 <= 0.0f) { outWa = 1.0f; outWb = 0.0f; outWc = 0.0f; return; }

            const float d3 = math::Vector3::Dot(ab, -b);
            const float d4 = math::Vector3::Dot(ac, -b);
            if (d3 >= 0.0f && d4 <= d3) { outWa = 0.0f; outWb = 1.0f; outWc = 0.0f; return; }

            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
            {
                const float v = d1 / (d1 - d3);
                outWa = 1.0f - v; outWb = v; outWc = 0.0f;
                return;
            }

            const float d5 = math::Vector3::Dot(ab, -c);
            const float d6 = math::Vector3::Dot(ac, -c);
            if (d6 >= 0.0f && d5 <= d6) { outWa = 0.0f; outWb = 0.0f; outWc = 1.0f; return; }

            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
            {
                const float w = d2 / (d2 - d6);
                outWa = 1.0f - w; outWb = 0.0f; outWc = w;
                return;
            }

            const float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
            {
                const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                outWa = 0.0f; outWb = 1.0f - w; outWc = w;
                return;
            }

            const float sum = va + vb + vc;
            if (std::abs(sum) < 1e-20f)
            {
                /// @note 面積が潰れた三角形。最長の辺へ落として重みを決める。
                ClosestOnSegment(a, b, outWa, outWb);
                outWc = 0.0f;
                return;
            }

            const float inv = 1.0f / sum;
            outWb = vb * inv;
            outWc = vc * inv;
            outWa = 1.0f - outWb - outWc;
        }

        Barycentric ClosestBarycentric(const Simplex& s)
        {
            Barycentric result;
            switch (s.size)
            {
            case 1:
                result.weight[0] = 1.0f;
                break;

            case 2:
                ClosestOnSegment(s.verts[0].point, s.verts[1].point,
                                 result.weight[0], result.weight[1]);
                break;

            case 3:
                ClosestOnTriangle(s.verts[0].point, s.verts[1].point, s.verts[2].point,
                                  result.weight[0], result.weight[1], result.weight[2]);
                break;

            case 4:
            {
                /// @note 反復上限に達した場合だけここへ来る (原点を含む四面体は交差として抜ける)。
                ///       4 つの面のうち原点に最も近いものを採る。
                static constexpr int kFaces[4][3] = { {0,1,2}, {0,1,3}, {0,2,3}, {1,2,3} };
                float bestDistSq = std::numeric_limits<float>::max();

                for (const auto& face : kFaces)
                {
                    float w0 = 0.0f, w1 = 0.0f, w2 = 0.0f;
                    ClosestOnTriangle(s.verts[face[0]].point, s.verts[face[1]].point,
                                      s.verts[face[2]].point, w0, w1, w2);

                    const math::Vector3 point = s.verts[face[0]].point * w0
                                              + s.verts[face[1]].point * w1
                                              + s.verts[face[2]].point * w2;
                    const float distSq = point.LengthSq();
                    if (distSq < bestDistSq)
                    {
                        bestDistSq = distSq;
                        result = Barycentric{};
                        result.weight[face[0]] = w0;
                        result.weight[face[1]] = w1;
                        result.weight[face[2]] = w2;
                    }
                }
                break;
            }

            default:
                break;
            }
            return result;
        }

        /// 非交差で終わった単体から closestA / closestB / distance を埋める。
        void FillClosestPoints(const Simplex& s, GJKResult& out)
        {
            if (s.size <= 0) return;

            const Barycentric bary = ClosestBarycentric(s);

            math::Vector3 closestA = math::Vector3::ZERO;
            math::Vector3 closestB = math::Vector3::ZERO;
            for (int i = 0; i < s.size; ++i)
            {
                closestA += s.verts[i].suppA * bary.weight[i];
                closestB += s.verts[i].suppB * bary.weight[i];
            }

            out.closestA = closestA;
            out.closestB = closestB;
            out.distance = (closestA - closestB).Length();
        }
    } // namespace

    bool DoSimplex(Simplex& simplex, math::Vector3& direction)
    {
        switch (simplex.size)
        {
        case 2: return DoSimplexLine(simplex, direction);
        case 3: return DoSimplexTriangle(simplex, direction);
        case 4: return DoSimplexTetrahedron(simplex, direction);
        default: return false;
        }
    }

    GJKResult GJK_Intersect(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        int maxIter)
    {
        GJKResult result;

        /// @note 初期方向: A - B (ゼロの場合は UP)
        math::Vector3 dir = supportA(shapeA, math::Vector3::RIGHT)
                          - supportB(shapeB, math::Vector3::RIGHT);
        if (dir.LengthSq() < 1e-10f)
            dir = math::Vector3::UP;

        Simplex simplex;

        auto MakeVertex = [&](const math::Vector3& d) -> Simplex::Vertex
        {
            Simplex::Vertex v;
            v.suppA = supportA(shapeA,  d);
            v.suppB = supportB(shapeB, -d);
            v.point = v.suppA - v.suppB;
            return v;
        };

        simplex.Add(MakeVertex(dir));
        dir = -simplex.verts[0].point;

        for (int iter = 0; iter < maxIter; ++iter)
        {
            if (dir.LengthSq() < 1e-10f) break;

            const Simplex::Vertex newVert = MakeVertex(dir);
            if (math::Vector3::Dot(newVert.point, dir) < 0.0f)
            {
                /// @note 新しいサポート点が原点より遠ければ交差なし
                result.intersects = false;
                return result;
            }

            simplex.Add(newVert);

            if (DoSimplex(simplex, dir))
            {
                result.intersects = true;
                if (simplex.size < 4)
                    ExpandToTetrahedron(simplex, shapeA, supportA, shapeB, supportB);
                result.simplex = simplex;
                return result;
            }
        }

        result.intersects = false;
        return result;
    }

    GJKResult GJK_Distance(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        int maxIter, float tolerance)
    {
        /// @note 距離の反復は «離れている» 前提で収束が保証される。深く重なった配置では単体が
        ///       原点を跨いで振動し «交差していない» と誤答しうるため、真偽判定は
        ///       GJK_Intersect 一本に絞り、ここは隙間を測ることに専念する。
        GJKResult result = GJK_Intersect(shapeA, supportA, shapeB, supportB, maxIter);
        if (result.intersects)
            /// @note simplex もそのまま残るので、必要なら EPA へ渡せる
            return result;

        auto MakeVertex = [&](const math::Vector3& d) -> Simplex::Vertex
        {
            Simplex::Vertex v;
            v.suppA = supportA(shapeA,  d);
            v.suppB = supportB(shapeB, -d);
            v.point = v.suppA - v.suppB;
            return v;
        };

        Simplex simplex;
        simplex.Add(MakeVertex(math::Vector3::RIGHT));
        math::Vector3 closestPoint = simplex.verts[0].point;

        for (int iter = 0; iter < maxIter; ++iter)
        {
            const float distSq = closestPoint.LengthSq();
            /// @note 非交差は確定しているので、ここへ来るのは数値誤差で原点に載った場合だけ。
            ///       これ以上詰められないので、そのときの単体で最近傍点を出す。
            if (distSq < 1e-12f) break;

            const math::Vector3   dir = -closestPoint;
            const Simplex::Vertex w   = MakeVertex(dir);

            /// @note 現在の単体が与える上界 |v| と、サポート点が与える下界 dot(w,v)/|v| の差が
            ///       tolerance を切ったら «これ以上縮まらない» とみなす。
            ///       両辺に |v| を掛けた形で比較し、平方根を 1 回で済ませる。
            const float distance = std::sqrt(distSq);
            if (distSq - math::Vector3::Dot(w.point, closestPoint) <= tolerance * distance)
                break;

            /// @note 同じサポート点が返ったら、方向を変えても進めない (数値誤差での停滞)。
            bool duplicate = false;
            for (int i = 0; i < simplex.size; ++i)
            {
                if ((simplex.verts[i].point - w.point).LengthSq() < 1e-12f)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) break;

            simplex.Add(w);

            /// @note DoSimplex は原点を含むかを判定しつつ、単体を «原点に最も近い特徴» へ削る。
            ///       交差判定と同じ経路を使うことで、両者の «どこが最近傍か» の解釈を揃える。
            math::Vector3 unusedDirection;
            if (DoSimplex(simplex, unusedDirection))
            {
                /// @note GJK_Intersect が «離れている» と答えた後にここへ来るのは、
                ///       境界ぎわで単体が原点を含んだと判定した場合。隙間 0 として扱う。
                break;
            }

            const Barycentric bary = ClosestBarycentric(simplex);
            closestPoint = math::Vector3::ZERO;
            for (int i = 0; i < simplex.size; ++i)
                closestPoint += simplex.verts[i].point * bary.weight[i];
        }

        result.intersects = false;
        FillClosestPoints(simplex, result);
        return result;
    }

} // namespace fbzz::physics
