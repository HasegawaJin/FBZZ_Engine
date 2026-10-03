/// @file    RayPathResolve.hlsl
/// @brief   Reference Path の表示用 HDR 平均と主可視面深度を転写する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(pathRadiance, TEX_GBUFFER0_SLOT);
FBZZ_TEX2D(pathSurface, TEX_GBUFFER1_SLOT);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

struct PathResolveOutput
{
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

/// @note 深度は中心レイの Reversed-Z。AA サンプルの放射輝度履歴とは独立した現在の可視面を使う。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics (SV_Depth)
PathResolveOutput PSMain(FBZZFullscreenVertex input)
{
    const int3 pixel = int3(int2(input.svPosition.xy), 0);
    PathResolveOutput output;
    output.color = pathRadiance.Load(pixel);
    output.depth = saturate(pathSurface.Load(pixel).w);
    return output;
}
