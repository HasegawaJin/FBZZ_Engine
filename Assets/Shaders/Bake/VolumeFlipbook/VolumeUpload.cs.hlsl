/// @file    VolumeUpload.cs.hlsl
/// @brief   Volume Flipbook Baker: CPU で解いた流体の格子を媒質ボリュームと速度ボリュームへ写す。
/// @author  Hasegawa Jin
/// @date    2026-09-11
//
// VolumeFill.cs.hlsl (解析 puff) の代わりに走り、同じ 2 枚 (u0 媒質 / u1 速度) を書く。
// 並びは VolumeFlipbookFluid.cpp の PackFluidVolume と同じ (x が最も速い)。

#include "Common/BindlessIndices.hlsli"

cbuffer VolumeUploadConstants : register(b0)
{
    uint  gResolution;
    uint3 gUploadPad;
};

// t14 / t15 は ComputeCall の kComputeStructuredBufferSlots。
FBZZ_SBUFFER_T(float4, gMediumIn, 14);
FBZZ_SBUFFER_T(float4, gVelocityIn, 15);

FBZZ_RWTEX3D_T(float4, gMediumOut, 0); // R 密度 / G 温度 / B colorKey / A 液体の割合
FBZZ_RWTEX3D_T(float4, gVelocityOut, 1); // xyz 速度 [bake 単位/秒]

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= gResolution))
        return;
    const uint index = id.x + gResolution * (id.y + gResolution * id.z);
    gMediumOut[id]   = gMediumIn[index];
    gVelocityOut[id] = gVelocityIn[index];
}
