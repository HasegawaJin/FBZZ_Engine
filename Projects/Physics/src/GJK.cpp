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
        // 交差確定後、EPA 用に simplex を四面体まで拡張する
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

            // 線分 → 三角形: ab に垂直な軸でサポート点を追加
            if (s.size == 2)
            {
                const math::Vector3 ab = s.verts[0].point - s.verts[1].point;
                const math::Vector3 aux = (std::abs(ab.x) < 0.57f)
                                        ? math::Vector3::RIGHT : math::Vector3::UP;
                s.Add(MakeVertex(math::Vector3::Cross(ab, aux)));
            }

            // 三角形 → 四面体: 面法線方向でサポート点を追加
            if (s.size == 3)
            {
                const math::Vector3 ab = s.verts[1].point - s.verts[0].point;
                const math::Vector3 ac = s.verts[2].point - s.verts[0].point;
                const math::Vector3 n  = math::Vector3::Cross(ab, ac);
                Simplex::Vertex v = MakeVertex(n);
                // 同一平面上なら逆方向を試す
                if (std::abs(math::Vector3::Dot(v.point - s.verts[0].point, n)) < 1e-6f)
                    v = MakeVertex({ -n.x, -n.y, -n.z });
                s.Add(v);
            }
        }


        // 3D の線分単体 (2 頂点) から原点への最短方向を求める
        bool DoSimplexLine(Simplex& s, math::Vector3& dir)
        {
            const math::Vector3 b = s.verts[0].point;
            const math::Vector3 a = s.verts[1].point; // 最後に追加した点
            const math::Vector3 ab = b - a;
            const math::Vector3 ao = -a;

            if (math::Vector3::Dot(ab, ao) > 0.0f)
            {
                const math::Vector3 perp = math::Vector3::Cross(math::Vector3::Cross(ab, ao), ab);
                // ab と ao が平行 (原点がセグメント上) → 内包
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

        // 三角形単体 (3 頂点) から原点への最短方向を求める
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

        // 四面体単体 (4 頂点) から原点を含むか判定し含まなければ方向を更新する
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
            return true; // 原点が四面体の内部
        }
    } // anonymous namespace

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

        // 初期方向: A - B (ゼロの場合は UP)
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
                // 新しいサポート点が原点より遠ければ交差なし
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

} // namespace fbzz::physics
