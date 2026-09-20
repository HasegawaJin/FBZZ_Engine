/// @file    Cloth.hlsl
/// @brief   両面の布を描画する Forward マテリアル。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/Color.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/SpecularAA.hlsli"
#include "Rendering/LodDither.hlsli"
#include "Rendering/ClothBRDF.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    float metallic;
    float roughness;
    float normalStrength;
    float occlusionStrength;
    float3 emissiveColor;
    float emissiveScale;
    float2 uvTiling;
    float2 uvOffset;
    float alphaCutoff;
    float3 _pad0;
    uint textureMask;
    float3 _pad1;
    float3 clothSheenColor;
    float sheen;
    uint texAlbedoIndex;
    uint texNormalIndex;
    uint texMetallicIndex;
    uint texEmissiveIndex;
    uint texAOIndex;
};

FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
FBZZ_MATERIAL_TEX(texNormal, texNormalIndex);
FBZZ_MATERIAL_TEX(texMetallicRough, texMetallicIndex);
FBZZ_MATERIAL_TEX(texEmissive, texEmissiveIndex);
FBZZ_MATERIAL_TEX(texAO, texAOIndex);
FBZZ_TEXCUBE(texIBLIrradiance, TEX_IBL_IRRADIANCE_SLOT);
FBZZ_TEXCUBE(texIBLRadiance, TEX_IBL_PREFILTER_SLOT);
SamplerState sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow : register(SAMPLER_SHADOW);
/// @note Light Probe Volume の Texture3D を引く Linear clamp。
SamplerState sampLinearClamp : register(SAMPLER_LINEAR_CLAMP);

PSInput VSMain(VSInput v)
{
    PSInput o;
    const float4 position = mul(float4(v.position, 1.0f), world);
    o.worldPos = position.xyz;
    o.svPosition = mul(position, viewProjection);
    o.normal = SafeNormalize(mul(v.normal, (float3x3)worldInvTranspose), float3(0, 1, 0));
    o.tangent = SafeNormalize(mul(v.tangent, (float3x3)world), float3(1, 0, 0));
    o.uv = v.uv;
    return o;
}

float4 PSMain(PSInput p) : SV_Target0
{
    ApplyLodDither(p.svPosition.xy, objectParams.x);
    const float2 uv = p.uv * uvTiling + uvOffset;
    const float4 sampleColor = (textureMask & 1u) ? texAlbedo.Sample(sampDefault, uv) : float4(1, 1, 1, 1);
    const float3 color = SRGBToLinear(sampleColor.rgb) * albedo.rgb;
    const float alpha = sampleColor.a * albedo.a;
    clip(alpha - alphaCutoff);
    float3 normal = SafeNormalize(p.normal, float3(0, 1, 0));
    const float3 view = SafeNormalize(cameraPos - p.worldPos, normal);
    /// @note 裏面判定は法線マップ適用前の幾何法線で固定する。負スケールで巻き順が反転しても視点側の半球で照明を評価する。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-faceforward 法線と視線による両面の向き付け。
    const float side = dot(normal, view) >= 0.0f ? 1.0f : -1.0f;
    if (textureMask & 2u) {
        const float3 mapped = ApplyNormalMap(texNormal.Sample(sampDefault, uv).rgb, normal,
            SafeNormalize(p.tangent, float3(1, 0, 0)));
        normal = SafeNormalize(lerp(normal, mapped, saturate(normalStrength)), normal);
    }
    /// @note 裏面でも同じ UV 接線基底で法線マップを評価し、評価後の法線全体を反転する。
    normal *= side;
    float rough = (textureMask & 4u) ? texMetallicRough.Sample(sampDefault, uv).g : roughness;
    rough = FilterSpecularRoughness(normal, clamp(rough, 0.05f, 1.0f));
    float ao = (textureMask & 16u) ? lerp(1.0f, texAO.Sample(sampDefault, uv).r, saturate(occlusionStrength)) : 1.0f;
    ao *= FBZZ_ScreenAO(p.svPosition.xy);
    const float3 light = SafeNormalize(-lightDir, normal);
    const float3 sheenTint = saturate(clothSheenColor) * saturate(sheen);
    float shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, normal, light);
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    float3 result = ClothDirect(normal, view, light, color, sheenTint, rough) * lightColor * (lightIntensity * LIGHT_UNIT_SCALE) * shadow;
    /// @note prefilter mip 0 は roughness=0 の環境コピー。Charlie の半球積分にはこの放射輝度だけを使用する。
    /// @see https://google.github.io/filament/main/filament.html#lighting/imagebasedlights/cloth Cloth IBL の分布と DFG。
    if (iblIntensity > 0.0f) {
        /// @note Cloth は従来から彩度補正前のキューブ値を使うので、同じ性質の Sheen 用の出力を取る。
        const DiffuseGI gi = FBZZ_DiffuseIrradiance(p.worldPos, normal, texIBLIrradiance, sampDefault, sampLinearClamp);
        result += gi.sheen * color * iblIntensity * iblDiffuseScale * ao;
        result += ClothEnvironmentSheen(normal, view, sheenTint, rough, texIBLRadiance, sampDefault)
            * iblIntensity * max(iblSpecularScale, 0.0f) * gi.specularOcclusion * ao;
    } else {
        /// @note 直接光の当たらない裏面も共通の環境光を受ける。固定 0.03 は Lit の環境光と Unlit の白を無視して黒く落としていた。
        result += color * ambientColor * ao;
    }
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, normal)
        result += ClothDirect(normal, view, ps.L, color, sheenTint, saturate(rough + ps.roughnessBias)) * ps.color * (ps.intensity * LIGHT_UNIT_SCALE);
    FBZZ_PUNCTUAL_END
    const float3 emission = (textureMask & 8u) ? SRGBToLinear(texEmissive.Sample(sampDefault, uv).rgb) : float3(1, 1, 1);
    result += emission * emissiveColor * emissiveScale;
    return float4(result, alpha);
}
