/// @file    SSAOBlur.cs.hlsl
/// @brief   半解像度 SSAO のノイズを、奥行きの境界を保ったままぼかす。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note    t9 = SSAO (半解像度)、t7 = 深度、u0 = 出力。b0 Camera。Dispatch は ceil(出力幅/8) x ceil(出力高/8)。

#include "PostProcess/AmbientOcclusion/AOBlurCommon.hlsli"
#include "Platform/Backend.hlsli"

FBZZ_TEX2D(texSSAO, TEX_SSAO_SLOT);
FBZZ_RWTEX2D_T(float4, outputBlur, UAV_OUTPUT_SLOT);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint2 pixel = dtid.xy;
    float2 outSize;
    outputBlur.GetDimensions(outSize.x, outSize.y);
    if (pixel.x >= (uint)outSize.x || pixel.y >= (uint)outSize.y)
        return;

    const float ao = BlurAO(texSSAO, int2(pixel), outSize);
    outputBlur[pixel] = float4(ao, ao, ao, 1.0f);
}
