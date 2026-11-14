/// @file    SkinnedPBR.hlsl
/// @brief   GPU スキニング + Cook-Torrance PBR のフォワードパス。
/// @author  Hasegawa Jin
/// @date    2026-05-19

/// @note PS の処理は Surface/PBR.hlsl と完全に一致させる (画面空間 AO / 接触影を引かない点だけが違う)。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Skinning.hlsli"
#include "Common/Space.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SpecularAA.hlsli"
#include "Rendering/Wetness.hlsli"
#include "Rendering/LodDither.hlsli"
#include "Rendering/ParallaxOcclusion.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    float  metallic;
    float  roughness;
    float  normalStrength;
    float  occlusionStrength;
    float3 emissiveColor;
    float  emissiveScale;
    float2 uvTiling;
    float2 uvOffset;
    float  alphaCutoff;
    float3 _pad0;
    uint   textureMask;
    float2 _pad1;
    float  parallaxDepth;      ///< @note Maximum recess in world metres. offset 92
    float  clearcoat;
    float  clearcoatRoughness;
    float  sheen;
    float  anisotropy;
    float3 sheenColor;
    float  _pad2;

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き、ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
    uint texNormalIndex;
    uint texMetallicIndex;
    uint texEmissiveIndex;
    uint texAOIndex;
    uint tex5Index;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);
FBZZ_MATERIAL_TEX(texMetallicRough, texMetallicIndex);
FBZZ_MATERIAL_TEX(texEmissive, texEmissiveIndex);
FBZZ_MATERIAL_TEX(texAO, texAOIndex);
SamplerState           sampDefault      : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow       : register(SAMPLER_SHADOW);
FBZZ_TEXCUBE(texIBLIrradiance, TEX_IBL_IRRADIANCE_SLOT);
FBZZ_TEXCUBE(texIBLPrefilter, TEX_IBL_PREFILTER_SLOT);
FBZZ_TEX2D_T(float4, texBRDFLut, TEX_IBL_BRDF_LUT_SLOT);
SamplerState           sampLinearClamp  : register(SAMPLER_LINEAR_CLAMP);



PSInput VSMain(SkinnedVSInput v)
{
    PSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float3 localN    = SafeNormalize(mul(v.normal,  (float3x3)skin), float3(0.0f, 1.0f, 0.0f));
    float3 localT    = SafeNormalize(mul(v.tangent, (float3x3)skin), float3(1.0f, 0.0f, 0.0f));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = SafeNormalize(mul(localN, (float3x3)worldInvTranspose), float3(0.0f, 1.0f, 0.0f));
    o.tangent    = SafeNormalize(mul(localT, (float3x3)world), float3(1.0f, 0.0f, 0.0f));
    o.uv         = v.uv;
    return o;
}

float4 PSMain(PSInput p, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    const float3 worldPositionDx = ddx(p.worldPos);
    const float3 worldPositionDy = ddy(p.worldPos);
    const float2 baseUv = p.uv * uvTiling + uvOffset;
    const float2 uvDx = ddx(baseUv);
    const float2 uvDy = ddy(baseUv);
    ApplyLodDither(p.svPosition.xy, objectParams.x);

    const float4 rawAlbedo = (textureMask & (1u << 0))
        ? texAlbedo.SampleGrad(sampDefault, baseUv, uvDx, uvDy)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float  alpha = rawAlbedo.a * albedo.a;
    clip(alpha - alphaCutoff);

    /// @note Cutout coverage is shared with shadow, depth and velocity passes at the undisplaced UV.
    const ParallaxSurface parallax = ResolveParallaxSurface(tex5Index, sampDefault,
        baseUv, uvDx, uvDy, p.worldPos, worldPositionDx, worldPositionDy, parallaxDepth, textureMask);
    const float2 uv = parallax.uv;
    float3 baseColor = rawAlbedo.rgb;
    [branch]
    if ((textureMask & 1u) && any(uv != baseUv))
        baseColor = texAlbedo.SampleGrad(sampDefault, uv, uvDx, uvDy).rgb;
    float3 col = SRGBToLinear(baseColor) * albedo.rgb;

    float3 N = SafeNormalize(p.normal, float3(0.0f, 1.0f, 0.0f));
    if (textureMask & (1u << 1))
    {
        float3 ns = texNormal.SampleGrad(sampDefault, uv, uvDx, uvDy).rgb;
        float3 nm = ApplyNormalMap(ns, N, SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f)));
        N = SafeNormalize(lerp(N, nm, saturate(normalStrength)), N);
    }
    N = ApplyParallaxSurfaceNormal(N, parallax, (textureMask & 2u) ? normalStrength : 0.0f);

    float met   = metallic;
    float rough = roughness;
    if (textureMask & (1u << 2))
    {
        float2 mr = texMetallicRough.SampleGrad(sampDefault, uv, uvDx, uvDy).gb;
        rough = mr.x;
        met   = mr.y;
    }

    met   = saturate(met);
    rough = max(saturate(rough), 0.045f);
    /// @note 濡れは素材の値なので法線分散のフィルタより先に掛ける (GBuffer.hlsl と同順)。
    const WetSurface wet = ApplyWetness(col, rough, N);
    col   = wet.albedo;
    rough = max(wet.roughness, 0.045f);
    N = OrientSurfaceNormal(N, frontFace);
    rough = FilterSpecularRoughness(N, rough);
    const float3 tangent = SafeNormalize(p.tangent, float3(1.0f, 0.0f, 0.0f));
    float3 T = SafeNormalize(tangent - N * dot(N, tangent),
                             abs(N.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f));
    T = SafeNormalize(T - N * dot(N, T), T);
    float3 B = SafeNormalize(cross(N, T), float3(0.0f, 0.0f, 1.0f));

    float ao = 1.0f;
    if (textureMask & (1u << 4))
        ao = lerp(1.0f, texAO.SampleGrad(sampDefault, uv, uvDx, uvDy).r, occlusionStrength);
    ao *= parallax.occlusion;

    FBZZWaterMediumSample receiverMedium;
    const bool receiverInWater = FBZZ_QueryWaterReceiverMedium(p.worldPos, receiverMedium);
    float3 V      = SafeNormalize(cameraPos - p.worldPos, N);
    float3 L      = SafeNormalize(-lightDir, N);
    float  shadow = ComputeShadowSurface(texShadow, sampShadow, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L, worldPositionDx, worldPositionDy);
    /// @note スキンドは GBuffer に描かれない (ExecuteGBufferPass が isSkinned を除外) ので、画面空間 AO / 接触影は背景の遮蔽を読んでしまい使わない。
    /// @note Deferred でもキャラは DeferredLighting を通らないため、パイプライン間の差は出ない。
    float3 result = iblIntensity > 0.0f
        ? Lighting_PBR_IBL_Advanced(p.worldPos, N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
              lightColor, lightIntensity, shadow, ao,
              texIBLIrradiance, texIBLPrefilter, texBRDFLut, iblMaxMipLevel,
              iblIntensity, iblDiffuseScale, iblSpecularScale,
              sampDefault, sampLinearClamp)
        : Lighting_PBR_Advanced(p.worldPos, N, V, L, T, B, col, met, rough,
              clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
              lightColor, lightIntensity, shadow, ao, receiverInWater, receiverMedium);

    /// @note 点光源 / スポットの走査元は clusterLightMode が決める (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN_PBR(p.worldPos, p.svPosition.xy, N, V, col, met, rough)
        const float3 sourceTransmittance = Lighting_WaterPunctualTransmittance(
            p.worldPos, _fbzzLight, ps, receiverInWater, receiverMedium);
        result += Lighting_PBR_Advanced(N, V, ps.L, T, B, col, met, saturate(rough + ps.roughnessBias),
            clearcoat, clearcoatRoughness, sheen, anisotropy, sheenColor,
            ps.color * sourceTransmittance, ps.intensity, 1.0f);
    FBZZ_PUNCTUAL_END

    float3 emissiveTex = (textureMask & (1u << 3))
        ? SRGBToLinear(texEmissive.SampleGrad(sampDefault, uv, uvDx, uvDy).rgb)
        : float3(1.0f, 1.0f, 1.0f);
    result += emissiveTex * emissiveColor * emissiveScale;

    return float4(result, alpha);
}
