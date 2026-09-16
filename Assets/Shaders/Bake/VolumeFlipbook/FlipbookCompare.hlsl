/// @file    FlipbookCompare.hlsl
/// @brief   焼いた Flipbook を «MV なし | MV あり» の横並びで再生するエディター用プレビュー。
/// @author  Hasegawa Jin
/// @date    2026-09-11
//
// 右半分の warp は Material/Effects/Particle.hlsl の currentUv / nextUv の計算と 1:1。
// WHY 写すか: プレビューがゲームと違う式で補間すると、«プレビューでは滑らかなのに
//     ゲームでは二重に見える» が起きる。規約 (FlipbookMotionVectorEncoding.hpp) を
//     確かめる場所として、ゲームと同じ計算をここにも置く。
#include "Common/Fullscreen.hlsli"

cbuffer FlipbookCompareConstants : register(b0)
{
    float4 gCurrentRect;  // xy = 左上の Atlas UV / zw = タイルの大きさ
    float4 gNextRect;
    float  gBlend;
    float  gStrength;     // マテリアルの motionVectorStrength (規約の S)
    uint   gTileSize;
    uint   gBackground;   // 0 = 暗 / 1 = 明 / 2 = チェッカー
};

Texture2D<float4> gColorAtlas  : register(t0); // 事前乗算、RGB は sRGB で符号化
Texture2D<float4> gMotionAtlas : register(t1);
SamplerState      gLinearClamp : register(s2);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float3 CompareBackground(float2 local)
{
    if (gBackground == 1u) return pow(float3(0.82f, 0.82f, 0.84f), 2.2f);
    if (gBackground == 2u)
    {
        const uint2 cell = uint2(local / 16.0f);
        return pow(((cell.x + cell.y) & 1u) != 0u ? float3(0.42f, 0.42f, 0.42f) : float3(0.26f, 0.26f, 0.26f), 2.2f);
    }
    return pow(float3(0.18f, 0.18f, 0.2f), 2.2f);
}

float4 LinearizeTexel(float4 texel)
{
    return float4(pow(max(texel.rgb, 0.0f), 2.2f), texel.a);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const uint2 pixel = uint2(p.svPosition.xy);
    const bool withMotion = pixel.x >= gTileSize;
    const float2 local = float2(pixel.x - (withMotion ? gTileSize : 0u), pixel.y) + 0.5f;
    const float2 tileUv = local / float(gTileSize);

    float2 currentUv = gCurrentRect.xy + tileUv * gCurrentRect.zw;
    float2 nextUv = gNextRect.xy + tileUv * gNextRect.zw;
    if (withMotion)
    {
        const float2 motion = gMotionAtlas.SampleLevel(gLinearClamp, currentUv, 0.0f).rg * 2.0f - 1.0f;
        currentUv += motion * (gBlend * gStrength);
        nextUv -= motion * ((1.0f - gBlend) * gStrength);
    }

    const float4 current = LinearizeTexel(gColorAtlas.SampleLevel(gLinearClamp, currentUv, 0.0f));
    const float4 next = LinearizeTexel(gColorAtlas.SampleLevel(gLinearClamp, nextUv, 0.0f));
    const float4 blended = lerp(current, next, saturate(gBlend));
    const float3 composite = blended.rgb + CompareBackground(local) * (1.0f - blended.a);
    return float4(pow(saturate(composite), 1.0f / 2.2f), 1.0f);
}
