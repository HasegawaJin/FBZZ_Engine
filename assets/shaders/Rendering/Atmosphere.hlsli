// FBZZ Engine
// Atmosphere.hlsli | Rendering
// Rayleigh + Mie 大気散乱 (Skydome.hlsl が使用)
#ifndef ATMOSPHERE_HLSLI
#define ATMOSPHERE_HLSLI

#include "Common/Math.hlsli"

// =========================================================================
// Rayleigh 位相関数
//   空の青色成分を決定する。波長依存の散乱係数と組み合わせて使う。
// =========================================================================
float RayleighPhase(float cosTheta)
{
    return (3.0f / (16.0f * PI)) * (1.0f + cosTheta * cosTheta);
}

// =========================================================================
// Mie 位相関数 (Henyey-Greenstein)
//   g  : 非対称パラメータ (-1〜1)
//       g > 0 = 前方散乱 (太陽付近に光が集中 → グレア)
//       g = 0 = 等方散乱
//   典型値: g = 0.76〜0.85 (大気の霞)
// =========================================================================
float MiePhase(float cosTheta, float g)
{
    float g2  = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * PI * pow(max(denom, EPSILON), 1.5f));
}

// =========================================================================
// 簡易大気散乱 (単一散乱近似)
//
//   rayDir   : 視線方向 (正規化)
//   sunDir   : 太陽方向 (正規化, ライトの進行方向を反転したもの = toward sun)
//   rayleigh : 波長ごとの Rayleigh 散乱係数 (float3, 例: float3(5.8e-6, 13.5e-6, 33.1e-6))
//   mie      : Mie 散乱係数 (float, 例: 21e-6)
//   mieG     : Mie 非対称パラメータ (0.76 程度)
//   sunIntensity : 太陽光強度
//
//   実装: 地球曲率・多重散乱を省いた Preetham モデルの近似。
//         レイマーチなしで動作するためリアルタイム向け。
// =========================================================================
float3 ComputeAtmosphericScattering(float3 rayDir, float3 sunDir,
                                    float3 rayleigh, float mie, float mieG,
                                    float sunIntensity)
{
    float cosTheta = dot(rayDir, sunDir);

    // Rayleigh と Mie の位相関数
    float phaseR = RayleighPhase(cosTheta);
    float phaseM = MiePhase(cosTheta, mieG);

    // 大気光路長の近似 (仰角が浅いほど長くなる)
    float altitude = max(rayDir.y, 0.01f);
    float optical  = 1.0f / altitude;
    optical = min(optical, 40.0f);  // 地平線付近のクランプ

    float3 rayleighScatter = rayleigh * phaseR * optical;
    float3 mieScatter      = float3(mie, mie, mie) * phaseM * optical;

    return sunIntensity * (rayleighScatter + mieScatter);
}

// =========================================================================
// 太陽ディスク
//   太陽方向付近のピクセルを輝かせる。
// =========================================================================
float3 SunDisk(float3 rayDir, float3 sunDir, float sunIntensity)
{
    float cosAngle = dot(rayDir, sunDir);
    float disk     = smoothstep(0.9998f, 0.9999f, cosAngle);
    return float3(1.0f, 0.95f, 0.8f) * sunIntensity * disk;
}

#endif // ATMOSPHERE_HLSLI