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

inline float Clamp(float v, float minVal, float maxVal) {
    return v < minVal ? minVal : (v > maxVal ? maxVal : v);
}
inline float Clamp01(float v)                { return Clamp(v, 0.0f, 1.0f); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
/// @brief 区間 [a,b] における v の正規化位置を返す。
/// @note a == b (ゼロ幅区間) のときは v を区間先頭とみなし 0 を返す。
inline float InverseLerp(float a, float b, float v) {
    return (b - a) < EPSILON ? 0.0f : (v - a) / (b - a);
}
inline float Remap(float v, float inMin, float inMax, float outMin, float outMax) {
    return Lerp(outMin, outMax, InverseLerp(inMin, inMax, v));
}
inline float Abs(float v)          { return v < 0.0f ? -v : v; }
inline float Sign(float v)         { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
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
