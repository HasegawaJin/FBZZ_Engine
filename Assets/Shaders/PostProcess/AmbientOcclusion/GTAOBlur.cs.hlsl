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

[numthreads(AO_BLUR_GROUP_SIZE, AO_BLUR_GROUP_SIZE, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
    uint3 localId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    const uint2 pixel = id.xy;
    uint2 outSize;
    OutputGTAO.GetDimensions(outSize.x, outSize.y);
    LoadAOBlurTile(texGTAORaw, groupId.xy, groupIndex, outSize);
    if (any(pixel >= outSize))
        return;

    OutputGTAO[pixel] = BlurAO(localId.xy);
}
