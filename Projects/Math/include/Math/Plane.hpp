/// @file    Plane.hpp
/// @brief   平面定義 (法線 + 原点からの符号付き距離)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once

#include "Vector3.hpp"

/// @note Windows 系ヘッダや外部 SDK が Plane をマクロ定義すると fbzz::math::Plane の宣言が壊れる。
///       Math は engine の最下層なので、上位レイヤー由来のプリプロセッサ汚染をここで遮断する。
#ifdef Plane
#undef Plane
#endif

namespace fbzz::math {

struct Plane {
    Vector3 normal   = { 0.0f, 1.0f, 0.0f };
    float   distance = 0.0f; ///< dot(normal, p) + distance = 0 を満たす値。

    constexpr Plane() = default;
    constexpr Plane(const Vector3& n, float d) : normal(n), distance(d) {}

    /// @brief 法線・距離を正規化した平面を返す。
    Plane Normalized() const;

    /// @brief 点と平面の符号付き距離を返す。
    /// @return 正なら法線側。
    float SignedDistanceTo(const Vector3& point) const;

    /// @brief 点が法線側 (正の半空間) にあるかを判定する。
    bool IsOnPositiveSide(const Vector3& point) const;

    /// @brief 3 点から平面を生成する。
    /// @note p0→p1, p0→p2 の外積が法線になる。
    static Plane FromPoints(const Vector3& p0, const Vector3& p1, const Vector3& p2);

    /// @brief 法線と通過点から平面を生成する。
    static Plane FromNormalAndPoint(const Vector3& normal, const Vector3& point);
};

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。カリングで物体ごとに呼ぶためヘッダーで定義する。
inline float Plane::SignedDistanceTo(const Vector3& point) const
{
    return Vector3::Dot(normal, point) + distance;
}

inline bool Plane::IsOnPositiveSide(const Vector3& point) const
{
    return SignedDistanceTo(point) >= 0.0f;
}

} // namespace fbzz::math
