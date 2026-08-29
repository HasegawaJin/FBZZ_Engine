/// @file    ParticleColorSpace.hpp
/// @brief   パーティクル色の色空間変換 (sRGB / Linear / OkLab) と黒体放射色の生成。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fbzz::scene {

/// グラデーションのキー間をどの色空間で混ぜるか。
/// WHY: どの空間で混ぜるかは「正解が 1 つに決まらない」種類の選択で、用途で使い分かれる。
///      Gamma はカラーピッカー上の見た目どおりに繋がるが、白熱 → 橙 → 暗赤のような
///      彩度の高いランプでは中間が濁る。Linear は光として正しく足し合わさる代わりに
///      中間が明るく寄る。Oklab は明度と色相が知覚的に等間隔で動くため、
///      魔法エフェクトのような色相を大きく回すランプで破綻しない。
/// @note 値は Assets/Shaders/Rendering/ParticleCommon.hlsli の FBZZ_PGRAD_* と一致させること。
enum class ParticleColorSpace : uint8_t {
    Gamma  = 0, // オーサリング値をそのまま線形補間 (従来の挙動)
    Linear = 1, // リニア空間で補間
    Oklab  = 2, // 知覚的に等間隔な OkLab で補間
};

/// パーティクルの色をオーサリング空間 (sRGB) からリニアへ。
/// @note 変換式は Assets/Shaders/Common/Color.hlsli の SRGBToLinear と一致させること。
///       HDR 値 (>1) もそのまま通る (pow は単調なので順序が保たれる)。
inline float ParticleSrgbToLinear(float c)
{
    return std::pow((std::max)(c, 0.0f), 2.2f);
}

inline float ParticleLinearToSrgb(float c)
{
    return std::pow((std::max)(c, 0.0f), 1.0f / 2.2f);
}

/// RGB のみ変換し、アルファは触らない。
inline math::Vector4 ParticleSrgbToLinear(const math::Vector4& c)
{
    return { ParticleSrgbToLinear(c.x), ParticleSrgbToLinear(c.y), ParticleSrgbToLinear(c.z), c.w };
}

inline math::Vector4 ParticleLinearToSrgb(const math::Vector4& c)
{
    return { ParticleLinearToSrgb(c.x), ParticleLinearToSrgb(c.y), ParticleLinearToSrgb(c.z), c.w };
}

// HDR 値でも符号と単調性を保つ立方根。OkLab の LMS 圧縮に使う。
inline float ParticleSignedCbrt(float v)
{
    return v < 0.0f ? -std::cbrt(-v) : std::cbrt(v);
}

/// リニア sRGB → OkLab。
/// @note 係数は Björn Ottosson の OkLab 定義そのまま。GPU 側 (ParticleGpuSim.cs.hlsl の
///       LinearToOklab) と 1 桁も違えてはならない。
inline math::Vector3 ParticleLinearToOklab(const math::Vector3& c)
{
    const float l = 0.4122214708f * c.x + 0.5363325363f * c.y + 0.0514459929f * c.z;
    const float m = 0.2119034982f * c.x + 0.6806995451f * c.y + 0.1073969566f * c.z;
    const float s = 0.0883024619f * c.x + 0.2817188376f * c.y + 0.6299787005f * c.z;
    const float l_ = ParticleSignedCbrt(l);
    const float m_ = ParticleSignedCbrt(m);
    const float s_ = ParticleSignedCbrt(s);
    return { 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
             1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
             0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_ };
}

/// OkLab → リニア sRGB。
inline math::Vector3 ParticleOklabToLinear(const math::Vector3& lab)
{
    const float l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
    const float m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
    const float s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;
    return {  4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
             -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
             -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s };
}

// ---------------------------------------------------------------------------
// 黒体放射
// ---------------------------------------------------------------------------

// CIE 1931 等色関数の区分ガウス近似 (Wyman, Sloan & Shirley 2013)。
// WHY: 炎の色は「すす粒子の温度による黒体放射」で決まるため、正しい色を出すには
//      プランク分布を等色関数へ積分するしかない。テーブルを持たずに済む近似式を使う。
inline float ParticlePiecewiseGaussian(float x, float peak, float mu, float sigma1, float sigma2)
{
    const float t = (x - mu) / (x < mu ? sigma1 : sigma2);
    return peak * std::exp(-0.5f * t * t);
}

inline math::Vector3 ParticleCieXyzBar(float nanometres)
{
    const float x = ParticlePiecewiseGaussian(nanometres,  1.056f, 599.8f, 37.9f, 31.0f)
                  + ParticlePiecewiseGaussian(nanometres,  0.362f, 442.0f, 16.0f, 26.7f)
                  + ParticlePiecewiseGaussian(nanometres, -0.065f, 501.1f, 20.4f, 26.2f);
    const float y = ParticlePiecewiseGaussian(nanometres,  0.821f, 568.8f, 46.9f, 40.5f)
                  + ParticlePiecewiseGaussian(nanometres,  0.286f, 530.9f, 16.3f, 31.1f);
    const float z = ParticlePiecewiseGaussian(nanometres,  1.217f, 437.0f, 11.8f, 36.0f)
                  + ParticlePiecewiseGaussian(nanometres,  0.681f, 459.0f, 26.0f, 13.8f);
    return { x, y, z };
}

// プランクの法則 (分光放射輝度)。λ は nm、戻り値は相対値でよいので定数倍は省く。
inline float ParticlePlanckRadiance(float nanometres, float kelvin)
{
    constexpr double h = 6.62607015e-34;  // Planck
    constexpr double c = 2.99792458e8;    // 光速
    constexpr double kB = 1.380649e-23;   // Boltzmann
    const double lambda = static_cast<double>(nanometres) * 1.0e-9;
    const double l5 = lambda * lambda * lambda * lambda * lambda;
    const double exponent = (h * c) / (lambda * kB * static_cast<double>(kelvin));
    // exp のオーバーフローを避ける。指数が大きい領域は放射がほぼ 0 なので切ってよい。
    if (exponent > 700.0) return 0.0f;
    return static_cast<float>((2.0 * h * c * c) / (l5 * (std::exp(exponent) - 1.0)));
}

/// 色温度 [K] → 相対リニア sRGB。最大チャンネルが 1 になるよう正規化した「色味」だけを返す。
/// @note 輝度 (Stefan-Boltzmann の T^4) は呼び出し側が掛ける。色味と明るさを分けておくと
///       「温度で色は決まるが明るさは演出で決めたい」という実務上の要求に応えられる。
inline math::Vector3 ParticleBlackbodyChroma(float kelvin)
{
    const float clamped = std::clamp(kelvin, 500.0f, 40000.0f);
    math::Vector3 xyz = math::Vector3::ZERO;
    // 5nm 刻みの矩形積分。可視域の端は寄与が小さいので、これで十分な精度が出る。
    constexpr float kStep = 5.0f;
    for (float nm = 380.0f; nm <= 780.0f; nm += kStep) {
        const float radiance = ParticlePlanckRadiance(nm, clamped);
        const math::Vector3 bar = ParticleCieXyzBar(nm);
        xyz = xyz + bar * (radiance * kStep);
    }
    // CIE XYZ → リニア sRGB (Rec.709 原色 / D65 白色点)
    math::Vector3 rgb = {
         3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z,
        -0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z,
         0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z
    };
    // 色域外の負値は最も近い表現可能色へ寄せる (低色温度で B が負になる)。
    rgb = { (std::max)(rgb.x, 0.0f), (std::max)(rgb.y, 0.0f), (std::max)(rgb.z, 0.0f) };
    const float peak = (std::max)(rgb.x, (std::max)(rgb.y, rgb.z));
    if (peak <= 1.0e-8f) return { 1.0f, 1.0f, 1.0f };
    return rgb * (1.0f / peak);
}

/// 色温度 [K] → リニア sRGB。referenceKelvin で intensity 倍の明るさになるよう
/// Stefan-Boltzmann (T^4) で輝度を与える。
/// @note 芯が白熱して外縁が彩度の高い橙のまま残る、という炎の見えはこの T^4 が作る。
///       戻り値は HDR (1 を超える) になり得る。
inline math::Vector3 ParticleBlackbodyLinear(float kelvin, float referenceKelvin, float intensity)
{
    const math::Vector3 chroma = ParticleBlackbodyChroma(kelvin);
    const float reference = (std::max)(referenceKelvin, 1.0f);
    const float ratio = (std::max)(kelvin, 0.0f) / reference;
    const float radiance = ratio * ratio * ratio * ratio;
    return chroma * (radiance * (std::max)(intensity, 0.0f));
}

} // namespace fbzz::scene
