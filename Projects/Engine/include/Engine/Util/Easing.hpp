// FBZZ Engine
// Easing.hpp | fbzz::util
// イージング曲線関数集
// UI や演出の Tween で使う補間曲線をヘッダーオンリーで提供する。
// 入力 t は基本的に 0.0f から 1.0f を想定する。
#pragma once
#include <cmath>

namespace fbzz::util {

struct Easing {
    static constexpr float PI = 3.14159265358979323846f;

    // ── Linear ───────────────────────────────────────────────────────────
    static inline float Linear(float t) { return t; }

    // ── Quad (2次) ────────────────────────────────────────────────────────
    static inline float EaseInQuad   (float t) { return t * t; }
    static inline float EaseOutQuad  (float t) { return t * (2.0f - t); }
    static inline float EaseInOutQuad(float t) {
        return (t < 0.5f) ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }

    // ── Cubic (3次) ────────────────────────────────────────────────────────
    static inline float EaseInCubic   (float t) { return t * t * t; }
    static inline float EaseOutCubic  (float t) { float u = 1.0f - t; return 1.0f - u*u*u; }
    static inline float EaseInOutCubic(float t) {
        return (t < 0.5f) ? 4.0f * t * t * t
                           : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
    }

    // ── Quart (4次) ────────────────────────────────────────────────────────
    static inline float EaseInQuart   (float t) { return t * t * t * t; }
    static inline float EaseOutQuart  (float t) { float u = 1.0f - t; return 1.0f - u*u*u*u; }
    static inline float EaseInOutQuart(float t) {
        return (t < 0.5f) ? 8.0f * t * t * t * t
                           : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) * 0.5f;
    }

    // ── Quint (5次) ────────────────────────────────────────────────────────
    static inline float EaseInQuint   (float t) { return t * t * t * t * t; }
    static inline float EaseOutQuint  (float t) { float u = 1.0f - t; return 1.0f - u*u*u*u*u; }
    static inline float EaseInOutQuint(float t) {
        return (t < 0.5f) ? 16.0f * t * t * t * t * t
                           : 1.0f - std::pow(-2.0f * t + 2.0f, 5.0f) * 0.5f;
    }

    // ── Sine ─────────────────────────────────────────────────────────────
    static inline float EaseInSine   (float t) { return 1.0f - std::cos(t * HALF_PI); }
    static inline float EaseOutSine  (float t) { return std::sin(t * HALF_PI); }
    static inline float EaseInOutSine(float t) { return -(std::cos(PI * t) - 1.0f) * 0.5f; }

    // ── Expo ─────────────────────────────────────────────────────────────
    static inline float EaseInExpo   (float t) { return (t == 0.0f) ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f); }
    static inline float EaseOutExpo  (float t) { return (t == 1.0f) ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }
    static inline float EaseInOutExpo(float t) {
        if (t == 0.0f) return 0.0f;
        if (t == 1.0f) return 1.0f;
        return (t < 0.5f) ? std::pow(2.0f, 20.0f * t - 10.0f) * 0.5f
                           : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;
    }

    // ── Circ ─────────────────────────────────────────────────────────────
    static inline float EaseInCirc   (float t) { return 1.0f - std::sqrt(1.0f - t * t); }
    static inline float EaseOutCirc  (float t) { return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f)); }
    static inline float EaseInOutCirc(float t) {
        return (t < 0.5f) ? (1.0f - std::sqrt(1.0f - 4.0f * t * t)) * 0.5f
                           : (std::sqrt(1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f)) + 1.0f) * 0.5f;
    }

    // ── Back (少しオーバーシュート) ────────────────────────────────────────
    static inline float EaseInBack   (float t) {
        constexpr float c = 1.70158f;
        return (c + 1.0f) * t * t * t - c * t * t;
    }
    static inline float EaseOutBack  (float t) {
        constexpr float c = 1.70158f;
        float u = t - 1.0f;
        return 1.0f + (c + 1.0f) * u * u * u + c * u * u;
    }
    static inline float EaseInOutBack(float t) {
        constexpr float c = 1.70158f * 1.525f;
        return (t < 0.5f)
            ? (std::pow(2.0f * t, 2.0f) * ((c + 1.0f) * 2.0f * t - c)) * 0.5f
            : (std::pow(2.0f * t - 2.0f, 2.0f) * ((c + 1.0f) * (2.0f * t - 2.0f) + c) + 2.0f) * 0.5f;
    }

    // ── Elastic (バネ) ────────────────────────────────────────────────────
    static inline float EaseInElastic(float t) {
        if (t == 0.0f) return 0.0f;
        if (t == 1.0f) return 1.0f;
        return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * TWO_THIRDS_PI);
    }
    static inline float EaseOutElastic(float t) {
        if (t == 0.0f) return 0.0f;
        if (t == 1.0f) return 1.0f;
        return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * TWO_THIRDS_PI) + 1.0f;
    }
    static inline float EaseInOutElastic(float t) {
        if (t == 0.0f) return 0.0f;
        if (t == 1.0f) return 1.0f;
        return (t < 0.5f)
            ? -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * TWO_FIFTHS_PI)) * 0.5f
            :  (std::pow(2.0f,-20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * TWO_FIFTHS_PI)) * 0.5f + 1.0f;
    }

    // ── Bounce ────────────────────────────────────────────────────────────
    static inline float EaseOutBounce(float t) {
        constexpr float n = 7.5625f, d = 2.75f;
        if (t < 1.0f / d)       return n * t * t;
        if (t < 2.0f / d)       { t -= 1.5f   / d; return n * t * t + 0.75f; }
        if (t < 2.5f / d)       { t -= 2.25f  / d; return n * t * t + 0.9375f; }
                                  t -= 2.625f  / d; return n * t * t + 0.984375f;
    }
    static inline float EaseInBounce   (float t) { return 1.0f - EaseOutBounce(1.0f - t); }
    static inline float EaseInOutBounce(float t) {
        return (t < 0.5f) ? (1.0f - EaseOutBounce(1.0f - 2.0f * t)) * 0.5f
                           :        (1.0f + EaseOutBounce(2.0f * t - 1.0f)) * 0.5f;
    }

private:
    static constexpr float HALF_PI       = PI * 0.5f;
    static constexpr float TWO_THIRDS_PI = (2.0f * PI) / 3.0f;
    static constexpr float TWO_FIFTHS_PI = (2.0f * PI) / 4.5f;
};

} // namespace fbzz::util
