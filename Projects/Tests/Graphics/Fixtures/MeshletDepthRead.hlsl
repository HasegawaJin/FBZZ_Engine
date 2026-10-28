/// @file    MeshletDepthRead.hlsl
/// @brief   本番の深度出力を画素位置を変えずにRGBA読戻し境界へ写す。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, meshletDepth, 7);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex input) : SV_Target0
{
    uint width;
    uint height;
    meshletDepth.GetDimensions(width, height);
    const int2 pixel = clamp((int2)input.svPosition.xy, int2(0, 0), int2(width, height) - 1);
    const float depth = meshletDepth.Load(int3(pixel, 0));
    return float4(depth, depth, depth, 1.0f);
}
