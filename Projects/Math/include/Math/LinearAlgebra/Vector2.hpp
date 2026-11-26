/// @file    Vector2.hpp
/// @brief   2次元ベクトル (UV座標・スクリーン座標)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Math/Api.hpp>

#include "Math/Scalar/MathUtils.hpp"

namespace fbzz::math {

struct Vector2 {
    float x = 0.0f; ///< 座標または変位の X 成分。
    float y = 0.0f; ///< 座標または変位の Y 成分。

    constexpr Vector2() = default;
    constexpr Vector2(float x, float y) : x(x), y(y) {}

    Vector2  operator+(const Vector2& rhs) const;
    Vector2  operator-(const Vector2& rhs) const;
    Vector2  operator*(float s)            const;
    Vector2  operator/(float s)            const;
    Vector2& operator+=(const Vector2& rhs);
    Vector2& operator-=(const Vector2& rhs);
    bool     operator==(const Vector2& rhs) const;
    bool     operator!=(const Vector2& rhs) const;

    float   Length()     const;
    float   LengthSq()   const;
    FBZZ_MATH_API Vector2 Normalized() const;

    /// @note 単位方向との内積は、その向きに沿った長さ。負なら反対側を向いている。
    static float   Dot(const Vector2& a, const Vector2& b);
    /// @brief 2 点を結ぶ直線上の位置を求める。
    /// @param[in] a t = 0 に対応する始点。
    /// @param[in] b t = 1 に対応する終点。a と同じ座標系を使う。
    /// @param[in] t a から b へ進む比率。0 未満または 1 超なら線分の外まで進む。
    static Vector2 Lerp(const Vector2& a, const Vector2& b, float t);

    static const Vector2 ZERO; ///< 全成分が 0 のベクトル。
    static const Vector2 ONE; ///< 全成分が 1 のベクトル。
};

inline const Vector2 Vector2::ZERO = {0.0f, 0.0f};
inline const Vector2 Vector2::ONE  = {1.0f, 1.0f};

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。契約を報告しない小関数はヘッダーで定義する。
inline Vector2 Vector2::operator+(const Vector2& rhs) const { return {x + rhs.x, y + rhs.y}; }
inline Vector2 Vector2::operator-(const Vector2& rhs) const { return {x - rhs.x, y - rhs.y}; }
inline Vector2 Vector2::operator*(float s)            const { return {x * s, y * s}; }
inline Vector2 Vector2::operator/(float s)            const { return {x / s, y / s}; }

inline Vector2& Vector2::operator+=(const Vector2& rhs) {
    x += rhs.x;
    y += rhs.y;
    return *this;
}

inline Vector2& Vector2::operator-=(const Vector2& rhs) {
    x -= rhs.x;
    y -= rhs.y;
    return *this;
}

inline bool Vector2::operator==(const Vector2& rhs) const {
    return NearlyEqual(x, rhs.x) && NearlyEqual(y, rhs.y);
}
inline bool Vector2::operator!=(const Vector2& rhs) const { return !(*this == rhs); }

inline float Vector2::LengthSq() const { return x * x + y * y; }
inline float Vector2::Length()   const { return std::sqrt(LengthSq()); }

inline float Vector2::Dot(const Vector2& a, const Vector2& b) {
    return a.x * b.x + a.y * b.y;
}

inline Vector2 Vector2::Lerp(const Vector2& a, const Vector2& b, float t) {
    return a + (b - a) * t;
}

}
