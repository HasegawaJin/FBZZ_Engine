/// @file    GraphicsQualityFill.cs.hlsl
/// @brief   壁の両側で異なる SH と距離モーメントを用意する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include "Common/BindlessIndices.hlsli"
cbuffer QualityConstants : register(b2) { uint mode; float value; uint blocked; float reserved; };
FBZZ_RWTEX3D_T(float4, shOutput, 0);
FBZZ_RWTEX3D_T(float4, visibilityOutput, 1);
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 16u || id.y >= 8u || id.z >= 1u) return;
    const uint probe = id.x / 8u;
    const bool occluded = blocked == 2u || (blocked == 1u && probe == 1u);
    const float distance = occluded ? 0.01f : 1.0f;
    visibilityOutput[id] = float4(distance, distance * distance, 10.0f, 1.0f);
    if (id.y == 0u && id.x < 2u)
    {
        [unroll] for (uint k = 0u; k < 7u; ++k)
        {
            float4 c = 0.0f;
            if (k < 3u) c.w = id.x == 0u ? 0.1f : 1.0f;
            if (k == 6u) c.w = 1.0f;
            shOutput[uint3(id.x,0,k)] = c;
        }
    }
}
