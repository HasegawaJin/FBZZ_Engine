/// @file    AlphaShadow.hlsl
/// @brief   Standard masked geometry uses the same albedo alpha and UV contract as color.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/ObjectInstance.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Rendering/LodDither.hlsli"
#include "Rendering/LeafWind.hlsli"
FBZZ_TEX2D(g_shadowAlbedo, TEX_ALBEDO_SLOT);
SamplerState g_shadowAlbedoSampler : register(SAMPLER_DEFAULT);
struct ShadowOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float alpha : TEXCOORD1; };
ShadowOutput EvaluateAlphaShadow(float3 position, float2 uv, float4x4 objectWorld)
{
    ShadowOutput output;
    float3 casterPosition = position;
    /// @note The cascade-wide branch skips wind evaluation on the static depth path.
    [branch] if (stableLeafShadow <= 0.5f)
        casterPosition = FbzzLeafWind(position, uv, objectWorld, time);
    output.position = mul(mul(float4(casterPosition, 1), objectWorld), viewProjection);
    output.uv = uv * uvTiling + uvOffset;
    output.alpha = albedo.a;
    return output;
}

#ifdef FBZZ_SHADOW_SKINNED
#include "Common/Skinning.hlsli"
ShadowOutput VSMain(SkinnedVSInput v)
{
    const float4x4 skin = BlendSkinMatrix(v);
    const float4 position = mul(float4(v.position,1),skin);
    const float4x4 objectWorld = world;
#elif defined(FBZZ_INSTANCED)
ShadowOutput VSMain(VSInput v, uint instanceId : SV_InstanceID)
{
    const float4 position = float4(v.position,1);
    const float4x4 objectWorld = FbzzObjectInstance(instanceId).world;
#else
ShadowOutput VSMain(VSInput v)
{
    const float4 position = float4(v.position,1);
    const float4x4 objectWorld = world;
#endif
    return EvaluateAlphaShadow(position.xyz, v.uv, objectWorld);
}
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-clip - discard below the material cutoff.
void PSMain(ShadowOutput v)
{
    const float alpha = v.alpha*((textureMask&1u)!=0 ? g_shadowAlbedo.Sample(g_shadowAlbedoSampler,v.uv).a : 1.0f);
    clip(alpha-alphaCutoff);
    ApplyLodDither(v.position.xy,objectParams.x);
}
