/// @file    Atmosphere.hlsli
/// @brief   Rayleigh + Mie の簡易大気散乱と、太陽・月ディスク。
/// @author  Hasegawa Jin
/// @date    2026-05-19
#ifndef ATMOSPHERE_HLSLI
#define ATMOSPHERE_HLSLI

#include "Common/Math.hlsli"

/// @brief Rayleigh 位相関数。空の青を決める。
/// @see https://doi.org/10.1145/166117.166140 Nishita et al., "Display of the Earth Taking into Account Atmospheric Scattering" (1993) §3
float RayleighPhase(float cosTheta)
{
    return (3.0f / (16.0f * PI)) * (1.0f + cosTheta * cosTheta);
}

/// @brief Mie 位相関数 (Henyey-Greenstein)。
/// @param g 非対称パラメータ [-1,1]。正で前方散乱 (太陽まわりのグレア)。大気の霞は 0.76〜0.85。
/// @see https://doi.org/10.1086/144246 Henyey & Greenstein, "Diffuse radiation in the Galaxy" (1941)
float MiePhase(float cosTheta, float g)
{
    float g2  = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * PI * pow(max(denom, EPSILON), 1.5f));
}

/// @brief 単一散乱・一定密度で近似した空の放射輝度。
/// @param rayDir   視線方向 (正規化)。
/// @param sunDir   太陽へ向かう方向 (正規化, -lightDir)。
/// @param rayleigh 波長ごとの Rayleigh 散乱係数 (例: 5.8e-6, 13.5e-6, 33.1e-6)。
/// @note 地球曲率・多重散乱・レイマーチを省いた Preetham 系の近似。
float3 ComputeAtmosphericScattering(float3 rayDir, float3 sunDir,
                                    float3 rayleigh, float mie, float mieG,
                                    float sunIntensity)
{
    float cosTheta = dot(rayDir, sunDir);

    float phaseR = RayleighPhase(cosTheta);
    float phaseM = MiePhase(cosTheta, mieG);

    /// @note 光路長は 1/(y + 0.15) で地平線へ滑らかに伸ばす。min(1/y, 40) のような硬いクランプは
    ///       帯になる。地平線下は y=0 の薄い層として扱い、地平線色へ収束させる。6.0 は調整係数。
    float altitude = max(rayDir.y, 0.0f);
    float path     = 6.0f / (altitude + 0.15f);

    /// @note 消散 (Beer-Lambert) を入れた解析解 (betaR*phaseR + betaM*phaseM)/betaExt*(1-exp(-betaExt*path))。
    ///       光路を線形に掛けるだけだと地平線で無制限に白飛びする。吸収は無視し散乱だけで近似。
    float3 betaR   = rayleigh;
    float3 betaM   = float3(mie, mie, mie);
    float3 betaExt = betaR + betaM;

    float3 transmittance = exp(-betaExt * path);
    float3 inScatter     = (betaR * phaseR + betaM * phaseM)
                         / max(betaExt, EPSILON) * (1.0f - transmittance);

    return sunIntensity * inScatter;
}

/// @brief 地平線より下の画素を隠す係数 [0,1]。
/// @param rayDir 視線方向 (正規化)。地平線は Skydome と同じく rayDir.y = 0。
/// @note 天体の «中心の高度» ではなく «画素の方向» で隠す。中心で判定すると、沈んだ後もディスクが
///       地平線の下 (地形が覆っていない空ドーム) に描かれ続け、半分沈んだ姿も作れない。
/// @note 縁の幅 0.003 (約 0.17°) はディスク半径より十分細く、地平線の切れ目がジャギーにならない量。
float HorizonMask(float3 rayDir)
{
    return smoothstep(0.0f, 0.003f, rayDir.y);
}

/// @brief 太陽ディスク。
/// @param sunDir 太陽へ向かう方向 (正規化)。
/// @note 0.9994 ≒ cos(2°)。実際の太陽 (約 0.53°) より大きくして視認しやすくしている。
float3 SunDisk(float3 rayDir, float3 sunDir, float sunIntensity)
{
    float cosAngle = dot(rayDir, sunDir);
    float disk     = smoothstep(0.9994f, 0.9999f, cosAngle);
    return float3(1.0f, 0.95f, 0.8f) * sunIntensity * disk * HorizonMask(rayDir);
}

/// @brief 月ディスク。
/// @param moonDir   月へ向かう方向 (正規化)。SunMoon では -sunDir。
/// @param sizeScale 角サイズの倍率 (1 で太陽程度)。
/// @note 地平線は画素ごとに HorizonMask で切る。昇り沈みでディスクが地平線に隠れていく。
float3 MoonDisk(float3 rayDir, float3 moonDir, float3 color, float brightness, float sizeScale)
{
    float cosAngle = dot(rayDir, moonDir);
    float s     = max(sizeScale, 0.05f);
    float edge0 = 1.0f - 0.0010f * s;
    float edge1 = 1.0f - 0.0002f * s;
    float disk  = smoothstep(edge0, edge1, cosAngle);
    return color * brightness * disk * HorizonMask(rayDir);
}

#endif // ATMOSPHERE_HLSLI
