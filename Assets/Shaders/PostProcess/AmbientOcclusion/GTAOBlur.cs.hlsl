/// @file    GTAOBlur.cs.hlsl
/// @brief   半解像度 GTAO のノイズを、奥行きの境界を保ったままぼかす。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note    t23 = GTAO RAW (半解像度)、t7 = 深度、u7 = 出力。b0 Camera。Dispatch は ceil(出力幅/8) x ceil(出力高/8)。
/// @note    GTAO はスライス数を抑えてコストを稼ぐぶんノイズが出るので、ここで均してから合成する。

#include "PostProcess/AmbientOcclusion/AOBlurCommon.hlsli"
#include "Platform/Backend.hlsli"

FBZZ_TEX2D_T(float, texGTAORaw, TEX_GTAO_SLOT);
FBZZ_RWTEX2D_T(float, OutputGTAO, UAV_GTAO_BLUR_SLOT);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    float2 outSize;
    OutputGTAO.GetDimensions(outSize.x, outSize.y);
    if (pixel.x >= (uint)outSize.x || pixel.y >= (uint)outSize.y)
        return;

    OutputGTAO[pixel] = BlurAO(texGTAORaw, int2(pixel), outSize);
}
