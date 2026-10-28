/// @file    DepthCopy.hlsl
/// @brief   GBuffer の深度を全画面三角形で転写する。
/// @author  Hasegawa Jin
/// @date    2026-05-28

/// @note Pipeline/Deferred/DepthCopy.hlsl | Pipeline
/// @note GBuffer の深度テクスチャを hdrRT の深度バッファへ転写する
/// @note フルスクリーン三角形で実行し、SV_Depth 出力で深度値を上書きする。
/// @note Deferred パスで Sky / スキンドフォワードが正しい深度でテストできるようにするために必要。

#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texGBufDepth, TEX_DEPTH_SLOT);  ///< @note t7: gbufferRT 深度 SRV

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float PSMain(FBZZFullscreenVertex p) : SV_Depth
{
    /// @note このパスの出力先は colorCount=0 の深度専用 RT。
    /// @note SV_Target を宣言すると D3D11 は RTV slot 0 を要求するため、深度だけを返す。
    return texGBufDepth.Load(int3((int)p.svPosition.x, (int)p.svPosition.y, 0)).r;
}
