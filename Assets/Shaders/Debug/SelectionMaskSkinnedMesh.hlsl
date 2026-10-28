/// @file    SelectionMaskSkinnedMesh.hlsl
/// @brief   SelectionMaskSkinnedMesh.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-06-23

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Skinning.hlsli"

struct MaskPSInput
{
    float4 position : SV_POSITION;
};

MaskPSInput VSMain(SkinnedVSInput input)
{
    MaskPSInput output;
    output.position = SkinningMaskClipPosition(input, world, viewProjection);
    return output;
}

float4 PSMain(MaskPSInput input) : SV_TARGET
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
