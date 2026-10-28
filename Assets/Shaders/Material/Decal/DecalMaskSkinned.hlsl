/// @file DecalMaskSkinned.hlsl
/// @brief DecalMask.hlsl のスキンドメッシュ版
/// @author Hasegawa Jin
/// @date 2026-08-23

/// @note SelectionMaskSkinnedMesh.hlsl を流用しないか:
/// @note あちらは白 1 色を返すだけで、レイヤー番号も可視サーフェス判定も持たない。
/// @note 流用していた頃は受信バッファ上でスキンドメッシュだけが別の意味の値を書いており、
/// @note キャラクターを受信対象から外した瞬間に判定が壊れていた。
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Skinning.hlsli"
#include "Common/Space.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texSceneDepth, TEX_DEPTH_SLOT);

/// @note LAYOUT: DecalMask.hlsl / RenderPassContext.hpp の DecalReceiverCB と一致させること。
cbuffer DecalReceiverConstants : register(CB_DECAL)
{
    float receiverLayerEncoded; /// @note レイヤー番号 + 1 (0 はクリア値 = 未描画)
    float3 _receiverPad;
};

struct DecalMaskPixelInput
{
    float4 pos : SV_POSITION;
};



DecalMaskPixelInput VSMain(SkinnedVSInput input)
{
    DecalMaskPixelInput output;
    float3 localPos = mul(float4(input.position, 1.0f), BlendSkinMatrix(input)).xyz;
    output.pos = mul(mul(float4(localPos, 1.0f), world), viewProjection);
    return output;
}

float4 PSMain(DecalMaskPixelInput input) : SV_Target
{
    float sceneDepth = texSceneDepth.Load(int3((int)input.pos.x, (int)input.pos.y, 0)).r;
    float sceneView  = LinearizeDepth(sceneDepth, nearZ, farZ, isOrthographic);
    float fragView   = LinearizeDepth(input.pos.z, nearZ, farZ, isOrthographic);
    /// @note 許容差は DecalMask.hlsl と同じ根拠。スキニングは行列合成を挟むぶん
    /// @note 通常描画との丸め差が大きいので、狭めないこと。
    if (fragView > sceneView + max(0.01f, sceneView * 0.002f))
        discard;

    return float4(receiverLayerEncoded, 0.0f, 0.0f, 1.0f);
}
