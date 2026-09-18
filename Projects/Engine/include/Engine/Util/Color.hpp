/// @file    Color.hpp
/// @brief   RGBA カラー型と変換ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// Vector4 / RGBA32 / HSV / Hex との相互変換をまとめる。
/// Renderer へ渡す色は float 0.0f から 1.0f を基準にする。
#pragma once
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fbzz::util {

struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

    constexpr Color() = default;
    constexpr Color(float r, float g, float b, float a = 1.0f) : r(r), g(g), b(b), a(a) {}

    /// @name 変換
    /// @{
    constexpr math::Vector4 ToVector4() const { return { r, g, b, a }; }
    static constexpr Color  FromVector4(const math::Vector4& v) { return { v.x, v.y, v.z, v.w }; }

    /// 0xRRGGBBAA 形式の 32bit 値
    static constexpr Color FromRGBA32(uint32_t rgba) {
        return {
            ((rgba >> 24) & 0xFF) / 255.0f,
            ((rgba >> 16) & 0xFF) / 255.0f,
            ((rgba >>  8) & 0xFF) / 255.0f,
            ( rgba        & 0xFF) / 255.0f,
        };
    }

    /// "#RRGGBB" または "#RRGGBBAA" 形式の文字列
    static Color FromHex(const char* hex);

    constexpr uint32_t ToRGBA32() const {
        auto clamp = [](float v) -> uint8_t {
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return (uint32_t(clamp(r)) << 24) | (uint32_t(clamp(g)) << 16)
             | (uint32_t(clamp(b)) <<  8) |  uint32_t(clamp(a));
    }
    /// @}

    /// @name HSV 変換
    /// @{
    /// h: [0, 360)  s,v: [0, 1]
    static Color FromHSV(float h, float s, float v, float a = 1.0f);
    void         ToHSV(float& h, float& s, float& v) const;
    /// @}

    /// @name 補間・操作
    /// @{
    static Color Lerp(const Color& a, const Color& b, float t) {
        float ct = std::clamp(t, 0.0f, 1.0f);
        return {
            a.r + (b.r - a.r) * ct,
            a.g + (b.g - a.g) * ct,
            a.b + (b.b - a.b) * ct,
            a.a + (b.a - a.a) * ct,
        };
    }

    static Color LerpUnclamped(const Color& a, const Color& b, float t) {
        return {
            a.r + (b.r - a.r) * t,
            a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t,
        };
    }

    /// アルファだけ変えた複製
    constexpr Color WithAlpha(float newA) const { return { r, g, b, newA }; }

    /// 明るさを乗算 (HDR)
    constexpr Color operator*(float s) const { return { r * s, g * s, b * s, a }; }

    constexpr Color operator+(const Color& o) const { return { r+o.r, g+o.g, b+o.b, a+o.a }; }
    constexpr Color operator*(const Color& o) const { return { r*o.r, g*o.g, b*o.b, a*o.a }; }
    constexpr bool  operator==(const Color& o) const { return r==o.r && g==o.g && b==o.b && a==o.a; }
    constexpr bool  operator!=(const Color& o) const { return !(*this == o); }
    /// @}

    /// @name プリセット
    /// @{
    static const Color Red;
    static const Color Green;
    static const Color Blue;
    static const Color White;
    static const Color Black;
    static const Color Yellow;
    static const Color Cyan;
    static const Color Magenta;
    static const Color Gray;
    static const Color Clear;   ///< アルファ 0
    /// @}
};

inline const Color Color::Red     = { 1.0f, 0.0f, 0.0f, 1.0f };
inline const Color Color::Green   = { 0.0f, 1.0f, 0.0f, 1.0f };
inline const Color Color::Blue    = { 0.0f, 0.0f, 1.0f, 1.0f };
inline const Color Color::White   = { 1.0f, 1.0f, 1.0f, 1.0f };
inline const Color Color::Black   = { 0.0f, 0.0f, 0.0f, 1.0f };
inline const Color Color::Yellow  = { 1.0f, 1.0f, 0.0f, 1.0f };
inline const Color Color::Cyan    = { 0.0f, 1.0f, 1.0f, 1.0f };
inline const Color Color::Magenta = { 1.0f, 0.0f, 1.0f, 1.0f };
inline const Color Color::Gray    = { 0.5f, 0.5f, 0.5f, 1.0f };
inline const Color Color::Clear   = { 0.0f, 0.0f, 0.0f, 0.0f };

inline Color Color::FromHex(const char* hex)
{
    if (!hex) return {};
    if (hex[0] == '#') ++hex;

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };

    auto byte = [&](int i) -> float {
        return (hexVal(hex[i * 2]) * 16 + hexVal(hex[i * 2 + 1])) / 255.0f;
    };

    Color c;
    c.r = byte(0);
    c.g = byte(1);
    c.b = byte(2);
    c.a = (hex[6] && hex[7]) ? byte(3) : 1.0f;
    return c;
}

inline Color Color::FromHSV(float h, float s, float v, float a)
{
    if (s <= 0.0f) return { v, v, v, a };
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float hh = h / 60.0f;
    int   i  = static_cast<int>(hh);
    float ff = hh - i;
    float p  = v * (1.0f - s);
    float q  = v * (1.0f - s * ff);
    float t  = v * (1.0f - s * (1.0f - ff));
    switch (i) {
    case 0:  return { v, t, p, a };
    case 1:  return { q, v, p, a };
    case 2:  return { p, v, t, a };
    case 3:  return { p, q, v, a };
    case 4:  return { t, p, v, a };
    default: return { v, p, q, a };
    }
}

inline void Color::ToHSV(float& h, float& s, float& v) const
{
    float cmax = std::max({ r, g, b });
    float cmin = std::min({ r, g, b });
    float diff = cmax - cmin;
    v = cmax;
    s = (cmax > 0.0f) ? diff / cmax : 0.0f;
    if (diff <= 0.0f) { h = 0.0f; return; }
    if      (cmax == r) h = 60.0f * std::fmod((g - b) / diff, 6.0f);
    else if (cmax == g) h = 60.0f * ((b - r) / diff + 2.0f);
    else                h = 60.0f * ((r - g) / diff + 4.0f);
    if (h < 0.0f) h += 360.0f;
}

} // namespace fbzz::util
