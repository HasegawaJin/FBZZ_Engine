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

    // 大気光路長の近似 (仰角が浅いほど長くなる)。
    // 1/(y + 0.15) で地平線付近を滑らかに伸ばし、従来の min(1/y, 40) のような硬いクランプ帯を避ける。
    // 地平線下 (y<0) は y=0 の薄い大気層として扱い、滑らかに地平線色へ収束させる。
    float altitude = max(rayDir.y, 0.0f);
    float path     = 6.0f / (altitude + 0.15f);  // 光路長スケール（6.0 はチューニング係数）

    // 単一散乱の解析解 (一定密度近似):
    //   inScatter = (betaR*phaseR + betaM*phaseM) / betaExt * (1 - exp(-betaExt * path))
    // WHY: 従来は光路長を線形に掛けるだけで、地平線で散乱光が無制限に増えて白飛びしていた。
    //      消散 (Beer-Lambert) を入れることで光路が長いほど飽和し、地平線がリアルな
    //      明るい白〜オレンジへ収束する。波長ごとに betaExt が異なるため自然に色が分離する。
    float3 betaR   = rayleigh;                 // Rayleigh 散乱係数 (波長依存)
    float3 betaM   = float3(mie, mie, mie);    // Mie 散乱係数 (波長非依存)
    float3 betaExt = betaR + betaM;            // 消散係数 (吸収は無視し散乱のみで近似)

    float3 transmittance = exp(-betaExt * path);
    float3 inScatter     = (betaR * phaseR + betaM * phaseM)
                         / max(betaExt, EPSILON) * (1.0f - transmittance);

    return sunIntensity * inScatter;
}

// =========================================================================
// 太陽ディスク
//   太陽方向付近のピクセルを輝かせる。
// =========================================================================
float3 SunDisk(float3 rayDir, float3 sunDir, float sunIntensity)
{
    float cosAngle = dot(rayDir, sunDir);
    // 0.9994 ≒ cos(2°) : 実際の太陽 (~0.53°) より少し大きめにして視認しやすくする
    float disk     = smoothstep(0.9994f, 0.9999f, cosAngle);
    return float3(1.0f, 0.95f, 0.8f) * sunIntensity * disk;
}

// =========================================================================
// 月ディスク
//   moonDir   : 月の方向 (正規化)。SunMoon では太陽の反対側 (-sunDir) を渡す。
//   color     : 月色 / brightness : 明るさ / sizeScale : 角サイズ倍率 (1=太陽程度)
//   月が地平線下 (moonDir.y <= 0) のときは出さず、昇るにつれ滑らかに現れる。
// =========================================================================
float3 MoonDisk(float3 rayDir, float3 moonDir, float3 color, float brightness, float sizeScale)
{
    float cosAngle = dot(rayDir, moonDir);
    // sizeScale で角半径を調整。smoothstep は昇順 (edge0 < edge1) で中心ほど 1。
    float s     = max(sizeScale, 0.05f);
    float edge0 = 1.0f - 0.0010f * s;
    float edge1 = 1.0f - 0.0002f * s;
    float disk  = smoothstep(edge0, edge1, cosAngle);
    // 地平線付近で滑らかにフェードイン (太陽が出ている昼間は moonDir.y<0 で消える)。
    float visibility = saturate(moonDir.y * 6.0f);
    return color * brightness * disk * visibility;
}

#endif // ATMOSPHERE_HLSLI