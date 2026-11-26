/// @file    CustomCompose.hlsl
/// @brief   縮小して走ったカスタムパスの結果を、実寸へ拡大して合成する
/// @author  Hasegawa Jin
/// @date    2026-08-29

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texSource, SRV_POST_COLOR_SLOT);
/// @note 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampLinear  : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

/// @note UV を縮めて引くか:
/// @note 縮小結果はフル解像度の RT の «左上だけ» に描かれている (ビューポートを絞って
/// @note 描いたため)。専用サイズの RT を持てば要らない計算だが、解像度ごとに RT を
/// @note 増やすと、縮小を使わないプロジェクトまで VRAM を払うことになる。
float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    return texSource.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv), 0);
}
