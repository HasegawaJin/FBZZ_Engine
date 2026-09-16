/// @file    GJK.hpp
/// @brief   Gilbert-Johnson-Keerthi 距離アルゴリズム (交差判定 + 最近傍点)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
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
        bool intersects = false;

        // closestA / closestB / distance は GJK_Distance だけが埋める。
        // GJK_Intersect は «当たったか» しか答えないので 0 のまま残る。
        math::Vector3 closestA;            // shape A 上で shape B に最も近い点
        math::Vector3 closestB;            // shape B 上で shape A に最も近い点
        float         distance   = 0.0f;   // 2 形状の隙間 [m] (= |closestA - closestB|)

        Simplex simplex;   // 交差時の最終単体 (EPA に渡す用)。非交差時は使わない。
    };

    // GJK 交差判定。当たったかどうかだけを答える。
    // 交差時は EPA に渡せる Simplex を result.simplex に残す。
    //
    // WHY 距離を出さないか: NarrowPhase が毎フレーム呼ぶ経路で、必要なのは真偽と
    //     貫通量 (EPA の仕事) だけ。«当たっていない» と分かった時点で打ち切れるので、
    //     隙間の距離まで詰める反復を全ペアに払わせない。距離が要る場合は GJK_Distance。
    //
    // maxIter: 最大反復数 (通常 32 で収束)
    GJKResult GJK_Intersect(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        int maxIter = 32);

    // 非交差時の隙間と、両形状上の最近傍点を求める。
    // 重なっていた場合は intersects = true を返すだけで、貫通量は測らない (EPA の仕事)。
    //
    // 最近傍点は «終了時の単体の上で原点に最も近い点» の重心座標から、元形状上の
    // サポート点を同じ重みで混ぜて復元する。平らな面どうしが向かい合う配置のように
    // 最近傍点が一意に決まらない場合は、その集合の中の 1 点が返る
    // (距離は一意なので常に正しい)。
    //
    // tolerance: 距離の上界と下界の差がこれを切ったら収束とみなす [m]。
    GJKResult GJK_Distance(
        const void* shapeA, SupportFn supportA,
        const void* shapeB, SupportFn supportB,
        int   maxIter   = 32,
        float tolerance = 1.0e-4f);

    // 内部使用: 単体から原点に向かう最短方向を更新し、原点を含む場合 true を返す
    bool DoSimplex(Simplex& simplex, math::Vector3& direction);

} // namespace fbzz::physics
