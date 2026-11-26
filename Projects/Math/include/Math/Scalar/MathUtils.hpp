/// @file    MathUtils.hpp
/// @brief   数学定数とスカラーユーティリティ関数 (ヘッダオンリー)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include <cmath>

namespace fbzz::math {

constexpr float PI      = 3.14159265358979323846f;
constexpr float TWO_PI  = PI * 2.0f;
constexpr float HALF_PI = PI * 0.5f;
constexpr float DEG2RAD = PI / 180.0f;
constexpr float RAD2DEG = 180.0f / PI;
constexpr float EPSILON = 1e-6f;

inline float ToRad(float deg) { return deg * DEG2RAD; }
inline float ToDeg(float rad) { return rad * RAD2DEG; }

/// @brief 値を範囲に収める。下限の比較、上限の比較の順に行い、どちらも通らなければ入力値を返す。
inline float Clamp(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}
inline float Clamp01(float v)                { return Clamp(v, 0.0f, 1.0f); }
/// @note t は移動の割合。0 なら a、1 なら b、範囲外なら同じ直線上で区間の外を指す。
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
/// @brief 区間 [a,b] における v の正規化位置を返す。
/// @note 区間の幅 b − a が EPSILON 未満なら 0 を返す。逆順の区間もこの扱いになる。
inline float InverseLerp(float a, float b, float v) {
    return (b - a) < EPSILON ? 0.0f : (v - a) / (b - a);
}
/// @note 入力範囲での割合を出力範囲に移す。例えば 0～100 の 25 は、0～8 に写すと 2 になる。
inline float Remap(float v, float inMin, float inMax, float outMin, float outMax) {
    return Lerp(outMin, outMax, InverseLerp(inMin, inMax, v));
}
inline float Abs(float v)          { return v < 0.0f ? -v : v; }
/// @brief 正なら 1、負なら -1 を返す。0 と非数では 0 を返す。
inline float Sign(float value)
{
    if (value > 0.0f) {
        return 1.0f;
    }
    if (value < 0.0f) {
        return -1.0f;
    }
    return 0.0f;
}
inline float Pow(float base, float exp) { return std::pow(base, exp); }
inline float Sqrt(float v)         { return std::sqrt(v); }
inline float Floor(float v)        { return std::floor(v); }
inline float Ceil(float v)         { return std::ceil(v); }
inline float Round(float v)        { return std::round(v); }
inline float Min(float a, float b) { return a < b ? a : b; }
inline float Max(float a, float b) { return a > b ? a : b; }

inline bool NearlyEqual(float a, float b, float eps = EPSILON) { return Abs(a - b) < eps; }
inline bool NearlyZero(float v,           float eps = EPSILON) { return Abs(v) < eps; }

} // namespace fbzz::math
