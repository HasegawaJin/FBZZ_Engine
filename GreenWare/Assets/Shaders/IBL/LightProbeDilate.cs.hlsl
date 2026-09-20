/// @file    LightProbeDilate.cs.hlsl
/// @brief   壁に埋まった (無効な) プローブの SH を、周りの有効なプローブの重み付き平均で置き換える。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 入力は射影 CS が書いた生のボリューム (t0)、出力は描画が読むボリューム (u0)。並びは LightProbeGI.hlsli と同じ。
/// @note 無効なプローブを捨てずに埋めるのは、ハードウェアの三線形補間をそのまま使うため (引く側で 8 点を読み直す必要が無い)。
/// @see Docs/design/light-probe-gi.md «壁に埋まったプローブ»
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Platform/Backend.hlsli"

/// @note LAYOUT: LightProbeBakePass.cpp の LightProbeDilateCB と一致させること (16 bytes)。
cbuffer LightProbeDilateConstants : register(b0)
{
    uint3 grid;          ///< 各軸のプローブ数
    float validLimit;    ///< これ以上の有効度を «有効» とみなす
};

FBZZ_TEX3D_T(float4, gRawSH, 0);
FBZZ_RWTEX3D_T(float4, gDilatedSH, UAV_OUTPUT_SLOT);

static const uint kSlices = 7u;

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= grid)) return;

    const float selfValidity = gRawSH.Load(int4(id.xy, id.z + grid.z * 6u, 0)).w;
    if (selfValidity >= validLimit) {
        [unroll] for (uint k = 0u; k < kSlices; ++k) {
            const uint3 coord = uint3(id.xy, id.z + grid.z * k);
            gDilatedSH[coord] = gRawSH.Load(int4(coord, 0));
        }
        return;
    }

    /// @note 26 近傍のうち有効なものを 1/距離² で平均する。近傍に 1 つも無ければ自分の値をそのまま残す (真っ黒より漏れの方が目立たない)。
    float4 sum[kSlices];
    [unroll] for (uint s = 0u; s < kSlices; ++s) sum[s] = 0.0f;
    float weightSum = 0.0f;
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        const int3 n = int3(id) + int3(dx, dy, dz);
        if (all(int3(dx, dy, dz) == 0) || any(n < 0) || any(n >= int3(grid))) continue;
        const float validity = gRawSH.Load(int4(n.xy, n.z + int(grid.z) * 6, 0)).w;
        if (validity < validLimit) continue;
        const float w = validity / float(dx * dx + dy * dy + dz * dz);
        [unroll] for (uint k = 0u; k < kSlices; ++k)
            sum[k] += gRawSH.Load(int4(n.xy, n.z + int(grid.z * k), 0)) * w;
        weightSum += w;
    }

    [unroll] for (uint k = 0u; k < kSlices; ++k) {
        const uint3 coord = uint3(id.xy, id.z + grid.z * k);
        float4 value = weightSum > 0.0f ? sum[k] / weightSum : gRawSH.Load(int4(coord, 0));
        if (k == kSlices - 1u) value.w = weightSum > 0.0f ? 1.0f : selfValidity;
        gDilatedSH[coord] = value;
    }
}
