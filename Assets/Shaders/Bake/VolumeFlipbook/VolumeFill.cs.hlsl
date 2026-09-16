/// @file    VolumeFill.cs.hlsl
/// @brief   Volume Flipbook Baker: 解析 puff の和を媒質ボリュームと速度ボリュームへ書き込む。
/// @author  Hasegawa Jin
/// @date    2026-09-11
//
// 式は Projects/Engine/src/Asset/VolumeFlipbookAnalytic.cpp の SampleVolumeFill と 1:1。
// 片方だけ直すと、テストが守っている CPU 側と実際に焼かれる絵が静かにずれる。
//
// WHY このシェーダーが境界か: 流体ソルバーに置き換えるときも、下流 (レイマーチ・MV・Atlas) は
//     «u0 に媒質、u1 に速度» しか見ていない。ソルバーは同じ 2 枚を書けばそのまま焼ける。
#include "Platform/Backend.hlsli"
#include "Rendering/ParticleNoise.hlsli"
#include "Common/BindlessIndices.hlsli"

struct VolumePuffGpu
{
    float4 body[3];      // y = dot(body[i].xyz, x) + body[i].w
    float4 velocity[3];  // v = dot(velocity[i].xyz, x) + velocity[i].w
    float4 params;       // x density·envelope / y 芯の温度 / z noise seed / w 物体座標の cull 半径
    float4 look;         // x colorKey / y 液体の割合 / z noise の倍率 / w 予備
};

cbuffer VolumeFillConstants : register(b0)
{
    uint  gResolution;
    uint  gPuffCount;
    float gNoiseFrequency;
    float gNoiseAmplitude;
};

// t14 は ComputeCall の kComputeStructuredBufferSlots の 1 つ。それ以外の番号だと、
// DX12 で束縛しなかったときの null がテクスチャ次元になり、読み値が未定義になる。
FBZZ_SBUFFER_T(VolumePuffGpu, gPuffs, 14);

FBZZ_RWTEX3D_T(float4, gMediumOut, 0); // R 密度 / G 温度 / B colorKey / A 液体の割合
FBZZ_RWTEX3D_T(float4, gVelocityOut, 1); // xyz 速度 [bake 単位/秒]

float PuffBodyDensity(float3 y, float seedOffset, float noiseScale)
{
    const float amplitude = saturate(gNoiseAmplitude * noiseScale);
    float n = FbmNoise3D(y * gNoiseFrequency + float3(seedOffset, seedOffset * 1.31f, seedOffset * 0.73f), 4);
    float r = length(y) - amplitude * 0.4f * n;
    return saturate(1.0f - smoothstep(0.3f, 1.0f, r)) * saturate(0.65f + 0.5f * n);
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= gResolution))
        return;

    const float3 x = (float3(id) + 0.5f) / float(gResolution) * 2.0f - 1.0f;
    float  densitySum = 0.0f;
    float  temperatureSum = 0.0f;
    float  colorSum = 0.0f;
    float  liquidSum = 0.0f;
    float3 velocitySum = 0.0f;

    [loop] for (uint i = 0; i < gPuffCount; ++i)
    {
        const VolumePuffGpu puff = gPuffs[i];
        const float3 y = float3(dot(puff.body[0].xyz, x) + puff.body[0].w,
                                dot(puff.body[1].xyz, x) + puff.body[1].w,
                                dot(puff.body[2].xyz, x) + puff.body[2].w);
        if (dot(y, y) > puff.params.w * puff.params.w)
            continue;
        const float density = puff.params.x * PuffBodyDensity(y, puff.params.z, puff.look.z);
        if (density <= 0.0f)
            continue;
        const float3 v = float3(dot(puff.velocity[0].xyz, x) + puff.velocity[0].w,
                                dot(puff.velocity[1].xyz, x) + puff.velocity[1].w,
                                dot(puff.velocity[2].xyz, x) + puff.velocity[2].w);
        densitySum += density;
        temperatureSum += density * puff.params.y * saturate(1.0f - length(y));
        colorSum += density * puff.look.x;
        liquidSum += density * puff.look.y;
        velocitySum += v * density;
    }

    const float inverse = densitySum > 1.0e-6f ? 1.0f / densitySum : 0.0f;
    gMediumOut[id] = float4(densitySum, temperatureSum * inverse, colorSum * inverse, liquidSum * inverse);
    gVelocityOut[id] = float4(velocitySum * inverse, 0.0f);
}
