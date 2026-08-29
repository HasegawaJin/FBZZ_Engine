// FBZZ Engine
// PostProcess/Custom/CustomCompose.hlsl | PostProcess
// 縮小して走ったカスタムパスの結果を、実寸へ拡大して合成する

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texSource   : register(TEX_GBUFFER0);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampLinear  : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

// WHY UV を縮めて引くか:
//   縮小結果はフル解像度の RT の «左上だけ» に描かれている (ビューポートを絞って
//   描いたため)。専用サイズの RT を持てば要らない計算だが、解像度ごとに RT を
//   増やすと、縮小を使わないプロジェクトまで VRAM を払うことになる。
float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    return texSource.SampleLevel(sampLinear, p.uv * customPassInfo.x, 0);
}
