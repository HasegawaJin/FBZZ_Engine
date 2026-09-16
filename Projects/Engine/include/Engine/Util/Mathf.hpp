/// @file    Mathf.hpp
/// @brief   スカラー数学ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// Unity ライクな Clamp / Lerp / SmoothDamp などをまとめる。
/// ベクトルや行列は Math モジュール側を使い、ここでは float 中心に扱う。
#pragma once
#include <Math/MathUtils.hpp>
#include <cmath>
#include <algorithm>
#include <limits>

namespace fbzz::util {

struct Mathf {
    static constexpr float PI      = math::PI;
    static constexpr float TWO_PI  = math::TWO_PI;
    static constexpr float HALF_PI = math::HALF_PI;
    static constexpr float DEG2RAD = math::DEG2RAD;
    static constexpr float RAD2DEG = math::RAD2DEG;
    static constexpr float EPSILON = math::EPSILON;

    // ── 線形補間 ─────────────────────────────────────────────────────────
    static inline float Lerp(float a, float b, float t)
        { return a + (b - a) * t; }

    static inline float LerpUnclamped(float a, float b, float t)
        { return a + (b - a) * t; }

    static inline float InverseLerp(float a, float b, float v)
        { return (b != a) ? (v - a) / (b - a) : 0.0f; }

    // ── クランプ ──────────────────────────────────────────────────────────
    static inline float Clamp(float v, float lo, float hi)
        { return std::clamp(v, lo, hi); }

    static inline int   Clamp(int v, int lo, int hi)
        { return std::clamp(v, lo, hi); }

    static inline float Clamp01(float v)
        { return std::clamp(v, 0.0f, 1.0f); }

    // ── スムース補間 ──────────────────────────────────────────────────────
    static inline float SmoothStep(float edge0, float edge1, float x) {
        float t = Clamp01((x - edge0) / (edge1 - edge0));
        return t * t * (3.0f - 2.0f * t);
    }

    // 慣性付き追従 (Unity の Mathf.SmoothDamp 相当)
    // velocity はフレームをまたいで保持するため呼び出し側が管理する
    static inline float SmoothDamp(float current, float target,
                                   float& velocity, float smoothTime, float dt) {
        float omega = 2.0f / smoothTime;
        float x     = omega * dt;
        float e     = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        float delta = current - target;
        float temp  = (velocity + omega * delta) * dt;
        velocity    = (velocity - omega * temp) * e;
        return target + (delta + temp) * e;
    }

    // ── 線形追従 ──────────────────────────────────────────────────────────
    static inline float MoveTowards(float current, float target, float maxDelta) {
        float diff = target - current;
        if (std::abs(diff) <= maxDelta) return target;
        return current + (diff > 0.0f ? maxDelta : -maxDelta);
    }

    // 角度版 (degree 単位)
    static inline float MoveTowardsAngle(float current, float target, float maxDelta) {
        float delta = DeltaAngle(current, target);
        if (-maxDelta < delta && delta < maxDelta) return target;
        return MoveTowards(current, current + delta, maxDelta);
    }

    // ── 周期関数 ──────────────────────────────────────────────────────────
    // [0, length] を往復する鋸波
    static inline float PingPong(float t, float length) {
        t = std::fmod(t, length * 2.0f);
        return length - std::abs(t - length);
    }

    // [0, length) に折りたたむ
    static inline float Repeat(float t, float length) {
        return t - std::floor(t / length) * length;
    }

    // current → target への最短角度差 (degree, [-180, 180])
    static inline float DeltaAngle(float current, float target) {
        float delta = Repeat(target - current, 360.0f);
        if (delta > 180.0f) delta -= 360.0f;
        return delta;
    }

    // ── 比較 ─────────────────────────────────────────────────────────────
    static inline bool Approximately(float a, float b, float eps = EPSILON)
        { return std::abs(a - b) <= eps; }

    // ── 三角関数 (radian) ────────────────────────────────────────────────
    static inline float Sin(float v)           { return std::sin(v); }
    static inline float Cos(float v)           { return std::cos(v); }
    static inline float Tan(float v)           { return std::tan(v); }
    static inline float Asin(float v)          { return std::asin(v); }
    static inline float Acos(float v)          { return std::acos(v); }
    static inline float Atan2(float y, float x){ return std::atan2(y, x); }

    // ── 汎用 ─────────────────────────────────────────────────────────────
    static inline float Abs(float v)           { return std::abs(v); }
    static inline float Sign(float v)          { return (v >= 0.0f) ? 1.0f : -1.0f; }
    static inline float Sqrt(float v)          { return std::sqrt(v); }
    static inline float Pow(float b, float e)  { return std::pow(b, e); }
    static inline float Log(float v)           { return std::log(v); }
    static inline float Exp(float v)           { return std::exp(v); }
    static inline float Floor(float v)         { return std::floor(v); }
    static inline float Ceil(float v)          { return std::ceil(v); }
    static inline float Round(float v)         { return std::round(v); }
    static inline float Max(float a, float b)  { return std::max(a, b); }
    static inline float Min(float a, float b)  { return std::min(a, b); }
};

} // namespace fbzz::util
