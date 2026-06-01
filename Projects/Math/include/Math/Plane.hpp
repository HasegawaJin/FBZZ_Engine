// FBZZ Engine
// Plane.hpp | fbzz::math
// 平面定義 (法線 + 原点からの符号付き距離)
#pragma once

#include "Vector3.hpp"

// Windows 系ヘッダや外部 SDK が Plane をマクロ定義した場合、fbzz::math::Plane の宣言が壊れる。
// WHY: Math は engine の最下層なので、上位レイヤー由来のプリプロセッサ汚染をここで遮断する。
#ifdef Plane
#undef Plane
#endif

namespace fbzz::math {

struct Plane {
    Vector3 normal   = { 0.0f, 1.0f, 0.0f };
    float   distance = 0.0f; // dot(normal, p) + distance = 0

    constexpr Plane() = default;
    constexpr Plane(const Vector3& n, float d) : normal(n), distance(d) {}

    // 法線・距離を正規化した平面を返す
    Plane Normalized() const;

    // 点と平面の符号付き距離 (正 = 法線側)
    float SignedDistanceTo(const Vector3& point) const;

    // 点が法線側 (正の半空間) にあるか
    bool IsOnPositiveSide(const Vector3& point) const;

    // 3点から平面を生成 (p0→p1, p0→p2 の外積が法線)
    static Plane FromPoints(const Vector3& p0, const Vector3& p1, const Vector3& p2);

    // 法線と通過点から平面を生成
    static Plane FromNormalAndPoint(const Vector3& normal, const Vector3& point);
};

} // namespace fbzz::math
