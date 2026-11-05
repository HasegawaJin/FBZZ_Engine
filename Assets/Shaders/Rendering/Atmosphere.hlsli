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
    /// @note 帯になる。地平線下は y=0 の薄い層として扱い、地平線色へ収束させる。6.0 は調整係数。
    float altitude = max(rayDir.y, 0.0f);
    float path     = 6.0f / (altitude + 0.15f);

    /// @note 消散 (Beer-Lambert) を入れた解析解 (betaR*phaseR + betaM*phaseM)/betaExt*(1-exp(-betaExt*path))。
    /// @note 光路を線形に掛けるだけだと地平線で無制限に白飛びする。吸収は無視し散乱だけで近似。
    float3 betaR   = rayleigh;
    float3 betaM   = float3(mie, mie, mie);
    float3 betaExt = betaR + betaM;

    float3 transmittance = exp(-betaExt * path);
    float3 inScatter     = (betaR * phaseR + betaM * phaseM)
                         / max(betaExt, EPSILON) * (1.0f - transmittance);

    return sunIntensity * inScatter;
}

/// @brief 観測高度に応じた幾何学的地平線の可視率 [0,1]。
/// @param observerHeight 海面 Y=0 からの観測高度 [world m]。負値は海面として扱う。
/// @param radiusKm 惑星半径 [km]。屈折は含めず、地形と水面は実際の深度で遮蔽する。
/// @note h*(2R+h) は低い観測点でも 1-(R/(R+h))^2 の桁落ちを避ける。
/// @see https://aty.sdsu.edu/explain/atmos_refr/dip.html Dip without refraction, cos(d)=R/(R+h)
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-fwidth Pixel derivatives
float HorizonMask(float3 rayDir, float observerHeight = 0.0f, float radiusKm = 6371.0f)
{
    float radius = max(radiusKm, 1.0f) * 1000.0f;
    float height = max(observerHeight, 0.0f);
    float horizonY = -sqrt(height * (2.0f * radius + height)) / (radius + height);
    float edge = max(fwidth(rayDir.y) * 0.5f, 1.0e-6f);
    return smoothstep(horizonY - edge, horizonY + edge, rayDir.y);
}

/// @brief 1 AU で角直径約0.533度の太陽ディスク。
/// @param sunDir 光源と同じ、太陽へ向かう正規化方向。
/// @note 弦長で縁を測り、acos の中心での特異点と dot の 1 近傍の精度低下を避ける。
/// @see https://aa.usno.navy.mil/faq/sun_approx Angular semidiameter SD=0.2666/R degrees
float3 SunDisk(float3 rayDir, float3 sunDir, float sunIntensity,
               float observerHeight = 0.0f, float radiusKm = 6371.0f)
{
    const float radius = 2.0f * sin(0.2666f * PI / 360.0f);
    float chord = length(rayDir - sunDir);
    float edge = max(fwidth(chord) * 0.5f, 1.0e-6f);
    float disk = 1.0f - smoothstep(radius - edge, radius + edge, chord);
    return max(sunIntensity, 0.0f) * disk * HorizonMask(rayDir, observerHeight, radiusKm);
}

/// @brief 月ディスク。
/// @param moonDir   月へ向かう方向 (正規化)。SunMoon では -sunDir。
/// @param sizeScale 従来の月ディスクのサイズ調整係数。太陽の角直径とは独立。
/// @note 地平線の可視率は太陽と共通で、観測高度と画素の角度から求める。
float3 MoonDisk(float3 rayDir, float3 moonDir, float3 color, float brightness, float sizeScale,
                float observerHeight = 0.0f, float radiusKm = 6371.0f)
{
    float cosAngle = dot(rayDir, moonDir);
    float s     = max(sizeScale, 0.05f);
    float edge0 = 1.0f - 0.0010f * s;
    float edge1 = 1.0f - 0.0002f * s;
    float disk  = smoothstep(edge0, edge1, cosAngle);
    return color * brightness * disk * HorizonMask(rayDir, observerHeight, radiusKm);
}

/// @note ATMOSPHERE_HLSLI
#endif
