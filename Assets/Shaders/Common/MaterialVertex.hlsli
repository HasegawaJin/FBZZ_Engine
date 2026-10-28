/// @file    MaterialVertex.hlsli
/// @brief   標準表面・スキン材質の頂点変換を共用する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#ifndef FBZZ_MATERIAL_VERTEX_HLSLI
#define FBZZ_MATERIAL_VERTEX_HLSLI
#include "Common/Skinning.hlsli"
#include "Rendering/SurfaceCommon.hlsli"

/// @note normalize を使う既存経路の式を維持し、SafeNormalize の経路と混合しない。
PSInput TransformMaterialVertex(VSInput v)
{
    PSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.worldPos = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal = normalize(mul(v.normal, (float3x3)worldInvTranspose));
    o.tangent = normalize(mul(v.tangent, (float3x3)world));
    o.uv = v.uv;
    return o;
}

PSInput TransformSkinnedMaterialVertex(SkinnedVSInput v)
{
    PSInput o;
    float4x4 skin = BlendSkinMatrix(v);
    float4 localPos = mul(float4(v.position, 1.0f), skin);
    float3 localN = normalize(mul(v.normal, (float3x3)skin));
    float3 localT = normalize(mul(v.tangent, (float3x3)skin));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal = normalize(mul(localN, (float3x3)worldInvTranspose));
    o.tangent = normalize(mul(localT, (float3x3)world));
    o.uv = v.uv;
    return o;
}

/// @note 法線・接線の fallback と二段の SafeNormalize を保つ。
PSInput TransformSafeSkinnedMaterialVertex(SkinnedVSInput v)
{
    PSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float3 localN    = SurfaceSafeNormalize(mul(v.normal,  (float3x3)skin),
                                            float3(0.0f, 1.0f, 0.0f));
    float3 localT    = SurfaceSafeNormalize(mul(v.tangent, (float3x3)skin),
                                            float3(1.0f, 0.0f, 0.0f));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = SurfaceSafeNormalize(mul(localN, (float3x3)worldInvTranspose),
                                        float3(0.0f, 1.0f, 0.0f));
    o.tangent    = SurfaceSafeNormalize(mul(localT, (float3x3)world),
                                        float3(1.0f, 0.0f, 0.0f));
    o.uv         = v.uv;
    return o;
}

#endif ///< @note FBZZ_MATERIAL_VERTEX_HLSLI
