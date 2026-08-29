/// @file    ColorTemperature.hpp
/// @brief   色温度 [K] からリニア sRGB の光色を作る
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once
#include <Math/Vector3.hpp>
#include <algorithm>

namespace fbzz::renderer {

/// 黒体放射の色度をリニア sRGB へ変換する。
///
/// 経路は 色温度 → CIE 1960 UCS (Krystek の有理式近似) → CIE xy → XYZ → リニア sRGB。
///
/// WHY 「R/G/B を温度の多項式で直に出す」近似を使わないか:
///   よく出回っている多項式近似 (Tanner Helland 版など) は sRGB の *ガンマ後* の値へ
///   当ててあり、リニア空間で使うと中間色が転ぶ。このエンジンのライト色はリニアなので、
///   色度図を経由して物理的に変換する方が破綻しない。
///
/// WHY 最大成分で正規化するか:
///   黒体の絶対輝度は温度の 4 乗で効くため、生の XYZ をそのまま使うと色温度を動かした
///   だけで明るさが桁で変わる。intensity が明るさ、colorTemperature が色みという
///   役割分担を保つには、色みだけを取り出す必要がある。
///
/// @param kelvin 色温度 [K]。1000〜15000 の外はクランプする (近似式の有効域)。
[[nodiscard]] inline math::Vector3 ColorFromTemperature(float kelvin)
{
    const float t  = std::clamp(kelvin, 1000.0f, 15000.0f);
    const float t2 = t * t;

    // Krystek の有理式近似 (CIE 1960 UCS)
    const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t2)
                  / (1.0f        + 8.42420235e-4f * t + 7.08145163e-7f * t2);
    const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t2)
                  / (1.0f        - 2.89741816e-5f * t + 1.61456053e-7f * t2);

    // UCS (u, v) → CIE xy
    const float denom = 2.0f * u - 8.0f * v + 4.0f;
    if (std::abs(denom) < 1e-6f) return { 1.0f, 1.0f, 1.0f };
    const float x = 3.0f * u / denom;
    const float y = 2.0f * v / denom;
    if (y < 1e-6f) return { 1.0f, 1.0f, 1.0f };

    // xy → XYZ (輝度 Y = 1 に正規化した色度)
    const float bigX = x / y;
    const float bigY = 1.0f;
    const float bigZ = (1.0f - x - y) / y;

    // XYZ → リニア sRGB (sRGB/Rec.709 原色, D65 白色点)
    math::Vector3 rgb{
         3.2404542f * bigX - 1.5371385f * bigY - 0.4985314f * bigZ,
        -0.9692660f * bigX + 1.8760108f * bigY + 0.0415560f * bigZ,
         0.0556434f * bigX - 0.2040259f * bigY + 1.0572252f * bigZ,
    };

    // 色域外は負値になる。切り上げてから最大成分で正規化し、色みだけを残す。
    rgb.x = (std::max)(rgb.x, 0.0f);
    rgb.y = (std::max)(rgb.y, 0.0f);
    rgb.z = (std::max)(rgb.z, 0.0f);
    // WHY 括弧で囲むか: Windows.h が min/max を関数形式マクロで定義するため、
    //     素の std::max({...}) は「引数が多すぎるマクロ呼び出し」として展開され、
    //     このヘッダーを include した翻訳単位が丸ごと構文エラーになる。
    //     括弧を挟むとマクロ展開の対象にならない (エンジン全体で使っている書き方)。
    const float peak = (std::max)({ rgb.x, rgb.y, rgb.z });
    if (peak < 1e-6f) return { 1.0f, 1.0f, 1.0f };
    return { rgb.x / peak, rgb.y / peak, rgb.z / peak };
}

} // namespace fbzz::renderer
