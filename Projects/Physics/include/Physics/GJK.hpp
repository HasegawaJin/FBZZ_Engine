// FBZZ Engine
// GJK.hpp | fbzz::physics
// Gilbert-Johnson-Keerthi 距離アルゴリズム (交差判定 + 最近傍点)
#pragma once
#include <array>
#include <Math/Vector3.hpp>

namespace fbzz::physics
{

    // サポート関数: 任意形状に対して方向 dir の最遠点を返す。
    // std::function を避け、ソルバー内の小さな呼び出しを関数ポインタで固定する。
    using SupportFn = math::Vector3(*)(const void* shape, const math::Vector3& dir);

    // Minkowski 差空間の単体 (最大 4 頂点)。
    // EPA で接触点を復元するため、差分点だけでなく元形状上のサポート点も保持する。
    struct Simplex
    {
        struct Vertex
        {
            math::Vector3 point;  // Minkowski 差空間の点 (suppA - suppB)
            math::Vector3 suppA;  // shape A 上のサポート点
            math::Vector3 suppB;  // shape B 上のサポート点
        };

        std::array<Vertex, 4> verts;
        int size = 0;

        void Clear()              { size = 0; }
        void Add(const Vertex& v) { verts[size++] = v; }

        void Set(const Vertex& a)
        {
            verts[0] = a; size = 1;
        }
        void Set(const Vertex& a, const Vertex& b)
        {
            verts[0] = a; verts[1] = b; size = 2;
        }
        void Set(const Vertex& a, const Vertex& b, const Vertex& c)
        {
            verts[0] = a; verts[1] = b; verts[2] = c; size = 3;
        }
        void Set(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d)
        {
            verts[0] = a; verts[1] = b; verts[2] = c; verts[3] = d; size = 4;
        }
    };

    struct GJKResult
    {
        bool          intersects = false;
        math::Vector3 closestA;  // 非交差時の shape A 上の最近傍点
        math::Vector3 closestB;  // 非交差時の shape B 上の最近傍点
        float         distance   = 0.0f;
        Simplex       simplex;   // 交差時の最終単体 (EPA に渡す用)
    };

    // GJK 交差判定 + 最近傍点計算。
    // 交差時は EPA に渡せる Simplex を result.simplex に残す。
    // maxIter: 最大反復数 (通常 32 で収束)
    GJKResult GJK_Intersect(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        int maxIter = 32);

    // 内部使用: 単体から原点に向かう最短方向を更新し、原点を含む場合 true を返す
    bool DoSimplex(Simplex& simplex, math::Vector3& direction);

} // namespace fbzz::physics
