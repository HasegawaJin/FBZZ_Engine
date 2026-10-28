/// @file    SkinnedShadowMap.hlsl
/// @brief   SkinnedShadowMap.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-06-23

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Skinning.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/LodDither.hlsli"

struct SMPSInput
{
    float4 svPosition : SV_POSITION;
};



SMPSInput VSMain(SkinnedVSInput v)
{
    SMPSInput o;
    float4 localPos = mul(float4(v.position, 1.0f), BlendSkinMatrix(v));
    float4 worldPos = mul(localPos, world);
    o.svPosition = mul(worldPos, viewProjection);
    return o;
}

void PSMain(SMPSInput p)
{
    ApplyLodDither(p.svPosition.xy, objectParams.x);
}
