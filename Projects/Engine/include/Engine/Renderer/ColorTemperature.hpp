/// @file    ColorTemperature.hpp
/// @brief   色温度 [K] からリニア sRGB の光色を作る
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once
#include <Math/Vector3.hpp>
#include <algorithm>

namespace fbzz::renderer {

/// @brief 黒体放射の色度をリニア sRGB へ変換する。
/// @note 経路: 色温度 → CIE 1960 UCS (Krystek 近似) → CIE xy → XYZ → リニア sRGB。
/// @note 多項式近似 (Tanner Helland 版等) は sRGB のガンマ後の値へ当ててあり、リニア空間では
///       中間色が転ぶため、色度図を経由して物理的に変換する。
/// @note 黒体の輝度は温度の 4 乗で効くため、生の XYZ では色温度を動かすと明るさも変わる。
///       intensity=明るさ / colorTemperature=色みを保つため最大成分で正規化し色みだけ残す。
/// @param kelvin 色温度 [K]。1000〜15000 の外はクランプする (近似式の有効域)。
[[nodiscard]] inline math::Vector3 ColorFromTemperature(float kelvin)
{
    const float t  = std::clamp(kelvin, 1000.0f, 15000.0f);
    const float t2 = t * t;

    /// @note Krystek の有理式近似 (CIE 1960 UCS)。
    const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t2)
                  / (1.0f        + 8.42420235e-4f * t + 7.08145163e-7f * t2);
    const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t2)
                  / (1.0f        - 2.89741816e-5f * t + 1.61456053e-7f * t2);

    /// @note UCS (u, v) → CIE xy への変換。
    const float denom = 2.0f * u - 8.0f * v + 4.0f;
    if (std::abs(denom) < 1e-6f) return { 1.0f, 1.0f, 1.0f };
    const float x = 3.0f * u / denom;
    const float y = 2.0f * v / denom;
    if (y < 1e-6f) return { 1.0f, 1.0f, 1.0f };

    /// @note xy → XYZ (輝度 Y = 1 に正規化した色度)。
    const float bigX = x / y;
    const float bigY = 1.0f;
    const float bigZ = (1.0f - x - y) / y;

    /// @note XYZ → リニア sRGB (sRGB/Rec.709 原色, D65 白色点)。
    math::Vector3 rgb{
         3.2404542f * bigX - 1.5371385f * bigY - 0.4985314f * bigZ,
        -0.9692660f * bigX + 1.8760108f * bigY + 0.0415560f * bigZ,
         0.0556434f * bigX - 0.2040259f * bigY + 1.0572252f * bigZ,
    };

    /// @note 色域外は負値になる。切り上げてから最大成分で正規化し、色みだけを残す。
    /// @note (std::max) と括弧で囲むのは、Windows.h の min/max 関数形式マクロを避けるため。
    ///       素の std::max({...}) はマクロ展開されて構文エラーになる (エンジン全体の書き方)。
    rgb.x = (std::max)(rgb.x, 0.0f);
    rgb.y = (std::max)(rgb.y, 0.0f);
    rgb.z = (std::max)(rgb.z, 0.0f);
    const float peak = (std::max)({ rgb.x, rgb.y, rgb.z });
    if (peak < 1e-6f) return { 1.0f, 1.0f, 1.0f };
    return { rgb.x / peak, rgb.y / peak, rgb.z / peak };
}

} // namespace fbzz::renderer
