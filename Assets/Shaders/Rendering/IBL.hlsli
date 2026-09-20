/// @file    IBL.hlsli
/// @brief   Image-Based Lighting の評価。拡散は irradiance キューブ (または Light Probe)、鏡面は split-sum 近似。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note IBL 用の G_Smith は k = roughness^2/2 でリマップする (直接光用の (r+1)^2/8 とは別)。
/// @note worldPos を取る多重定義は Light Probe Volume の中で拡散項だけを差し替える。取らない版は従来どおりキューブだけを引く。
/// @see https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf Karis, "Real Shading in Unreal Engine 4", Image-Based Lighting (split-sum)
/// @see https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf Lagarde, "Moving Frostbite to PBR 3.0", §4.9
#ifndef IBL_HLSLI
#define IBL_HLSLI

#include "Common/Math.hlsli"
#include "Rendering/BRDF.hlsli"
#include "Rendering/LightProbeGI.hlsli"

/// @brief IBL 用 Smith-Schlick の片側項。
/// @param NdotX 0 クランプ済みの dot(N, V) または dot(N, L)。
/// @param roughness α = roughness^2 にする前の粗さ [0,1]。
/// @note 直接光の k は光源の立体角を考慮した補正値で、重要度サンプリングの積分と合わせるには k = roughness^2/2 を使う。
float G_SchlickGGX_IBL(float NdotX, float roughness)
{
    float k = roughness * roughness * 0.5f;
    return NdotX / (NdotX * (1.0f - k) + k + EPSILON);
}

/// @brief Smith の双方向幾何減衰 (IBL 版)。
float G_Smith_IBL(float NdotV, float NdotL, float roughness)
{
    return G_SchlickGGX_IBL(NdotV, roughness) * G_SchlickGGX_IBL(NdotL, roughness);
}

/// @brief irradiance キューブの値を輝度方向へ寄せる。
/// @note 太陽ピークを irradiance から除外しているため (IrradianceConvolution の DIFFUSE_RADIANCE_LIMIT)、日陰の環境光は暖色成分の無い純粋な青空光になり青へ寄る。相互反射による色の中和を近似する。
/// @note Light Probe は相互反射そのものを焼いているので、この補正を掛けない。
float3 DesaturateSkyIrradiance(float3 irradiance)
{
    const float kIBLDiffuseDesaturation = 0.35f;
    const float irrLum = dot(irradiance, float3(0.2126f, 0.7152f, 0.0722f));
    return lerp(irradiance, irrLum.xxx, kIBLDiffuseDesaturation);
}

/// @brief 拡散環境光の引き結果。
struct DiffuseGI
{
    float3 diffuse;           ///< 拡散項に使う放射照度 / π (キューブ側は彩度補正済み)
    float3 sheen;             ///< Sheen / Cloth に使う値 (キューブ側は補正前。従来の絵を変えないため)
    float  specularOcclusion; ///< 鏡面 IBL に掛ける倍率 [0,1]
};

/// @brief キューブだけの従来の値。
DiffuseGI FBZZ_SkyDiffuse(float3 sky)
{
    DiffuseGI gi;
    gi.diffuse = DesaturateSkyIrradiance(sky);
    gi.sheen = sky;
    gi.specularOcclusion = 1.0f;
    return gi;
}

/// @brief 法線方向の拡散放射照度 / π。Light Probe Volume の中ではプローブ、外ではキューブを返す。
/// @param worldPos シェーディング点 [world]。
/// @param sampClamp Linear clamp サンプラー (プローブの Texture3D に使う)。
/// @note ボリュームの外では覆い率が 0 なので、キューブだけの従来版と同じ値になる。
/// @note 入れ子の lerp は fallback について線形 (係数 1 - coverage) なので、彩度補正の差分だけを後から足せば 1 回の引きで両方が出る。
DiffuseGI FBZZ_DiffuseIrradiance(float3 worldPos, float3 N, TextureCube irradianceMap,
                                 SamplerState samp, SamplerState sampClamp)
{
    const float3 sky = irradianceMap.Sample(samp, N).rgb;
    float coverage;
    const float3 lit = FBZZ_ApplyLightProbes(worldPos, N, sampClamp, sky, coverage);
    DiffuseGI gi;
    gi.sheen = lit;
    gi.diffuse = lit + (DesaturateSkyIrradiance(sky) - sky) * (1.0f - coverage);
    gi.specularOcclusion = FBZZ_ProbeSpecularOcclusion(lit, sky);
    return gi;
}

/// @brief 拡散放射照度を受け取って環境光寄与を返す (split-sum)。
/// @param irradiance 法線方向の拡散放射照度 / π。キューブかプローブかは呼び出し側が決める。
/// @param specularOcclusion 鏡面 IBL に掛ける倍率 [0,1] (プローブ由来。無ければ 1)。
/// @param ao 拡散にだけ掛ける遮蔽 [0,1]。
/// @param maxMipLevel prefilterMap の最大 mip。
/// @param sampClamp BRDF LUT 用の Linear clamp サンプラー。
/// @return ダイレクトライティングへ加算する環境光。AO 乗算済み。
/// @note 低サンプル SSAO を鏡面へ掛けると明るい反射との差が点状に強調されるため、AO は拡散だけに掛ける (鏡面遮蔽には bent normal 等が要る)。
float3 EvaluateIBLFromIrradiance(
    float3      N,
    float3      V,
    float3      albedo,
    float       metallic,
    float       roughness,
    float       ao,
    float3      irradiance,
    float       specularOcclusion,
    TextureCube prefilterMap,
    Texture2D<float4> brdfLUT,
    int         maxMipLevel,
    float       diffuseScale,
    float       specularScale,
    SamplerState      samp,
    SamplerState      sampClamp)
{
    float3 R     = reflect(-V, N);
    float  NdotV = saturate(dot(N, V));
    float3 F0    = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    /// @note 拡散の kD はラフネス補正フレネルから取り、メタルは拡散を持たない。
    float3 F       = F_SchlickRoughness(NdotV, F0, roughness);
    float3 kD      = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kD * irradiance * albedo * max(diffuseScale, 0.0f);

    float  mip              = roughness * (float)maxMipLevel;
    float3 prefilteredColor = prefilterMap.SampleLevel(samp, R, mip).rgb;

    /// @note 鏡面は AO が効かず、浅い視線で青空の反射が «青い縁» として残る。粗い非金属で特に不自然なので拡散より強めに輝度方向へ寄せる。
    const float kIBLSpecularDesaturation = 0.6f;
    float preLum = dot(prefilteredColor, float3(0.2126f, 0.7152f, 0.0722f));
    prefilteredColor = lerp(prefilteredColor, preLum.xxx, kIBLSpecularDesaturation);

    float2 brdf     = saturate(brdfLUT.Sample(sampClamp, float2(NdotV, roughness)).rg);
    float3 specular = prefilteredColor * (F0 * brdf.x + brdf.y) * max(specularScale, 0.0f) * specularOcclusion;

    /// @note 接地バウンス相当の中立な下限。日陰や AO 部が暗い青に潰れるのを防ぐ。AO 非依存で、コントラストを保つよう小さく保つ。
    const float3 kIBLAmbientFloor = float3(0.025f, 0.025f, 0.025f);
    return diffuse * saturate(ao) + specular + albedo * kIBLAmbientFloor;
}

/// @brief キューブだけで拡散を引く従来版。Light Probe を受けない。
float3 EvaluateIBL(
    float3      N,
    float3      V,
    float3      albedo,
    float       metallic,
    float       roughness,
    float       ao,
    TextureCube irradianceMap,
    TextureCube prefilterMap,
    Texture2D<float4> brdfLUT,
    int         maxMipLevel,
    float       diffuseScale,
    float       specularScale,
    SamplerState      samp,
    SamplerState      sampClamp)
{
    const float3 irradiance = DesaturateSkyIrradiance(irradianceMap.Sample(samp, N).rgb);
    return EvaluateIBLFromIrradiance(N, V, albedo, metallic, roughness, ao, irradiance, 1.0f,
                                     prefilterMap, brdfLUT, maxMipLevel,
                                     diffuseScale, specularScale, samp, sampClamp);
}

/// @brief Light Probe Volume を受ける版。
/// @param worldPos シェーディング点 [world]。
float3 EvaluateIBL(
    float3      worldPos,
    float3      N,
    float3      V,
    float3      albedo,
    float       metallic,
    float       roughness,
    float       ao,
    TextureCube irradianceMap,
    TextureCube prefilterMap,
    Texture2D<float4> brdfLUT,
    int         maxMipLevel,
    float       diffuseScale,
    float       specularScale,
    SamplerState      samp,
    SamplerState      sampClamp)
{
    const DiffuseGI gi = FBZZ_DiffuseIrradiance(worldPos, N, irradianceMap, samp, sampClamp);
    return EvaluateIBLFromIrradiance(N, V, albedo, metallic, roughness, ao, gi.diffuse, gi.specularOcclusion,
                                     prefilterMap, brdfLUT, maxMipLevel,
                                     diffuseScale, specularScale, samp, sampClamp);
}

/// @brief Clearcoat / Sheen / 異方性を持つ拡張版の核。拡散放射照度を受け取る。
/// @param gi 拡散・Sheen の放射照度と鏡面遮蔽。
/// @note Clearcoat は低粗さの専用 specular lobe として環境反射にも加え、その Fresnel 分を base から差し引いて二重加算を防ぐ。
float3 EvaluateIBLAdvancedFromIrradiance(
    float3 N, float3 V, float3 T, float3 B, float anisotropy,
    float3 albedo, float metallic, float roughness, float ao,
    float clearcoat, float clearcoatRoughness,
    float sheen, float3 sheenColor,
    DiffuseGI gi, TextureCube prefilterMap,
    Texture2D<float4> brdfLUT, int maxMipLevel,
    float diffuseScale, float specularScale,
    SamplerState samp, SamplerState sampClamp)
{
    const float coat = saturate(clearcoat);
    const float NdotV = saturate(dot(N, V));
    float3 R = SafeNormalize(reflect(-V, N), N);
    const float stretch = clamp(anisotropy, -0.95f, 0.95f) * 0.35f;
    R = SafeNormalize(R + T * dot(R, T) * stretch - B * dot(R, B) * stretch, N);
    const float3 coatF = F_SchlickRoughness(NdotV,
        float3(0.04f, 0.04f, 0.04f), max(saturate(clearcoatRoughness), 0.03f)) * coat;
    float3 base = EvaluateIBLFromIrradiance(N, V, albedo, saturate(metallic),
                                            max(saturate(roughness), 0.045f), ao, gi.diffuse, gi.specularOcclusion,
                                            prefilterMap, brdfLUT, maxMipLevel,
                                            diffuseScale, specularScale, samp, sampClamp);
    base *= saturate(1.0f - coatF);

    const float2 coatBrdf = saturate(brdfLUT.Sample(sampClamp,
        float2(NdotV, max(saturate(clearcoatRoughness), 0.03f))).rg);
    const float3 coatEnv = prefilterMap.SampleLevel(
        samp, R, max(saturate(clearcoatRoughness), 0.03f) * max((float)maxMipLevel, 0.0f)).rgb;
    const float3 clearcoatSpec = coatEnv *
        (float3(0.04f, 0.04f, 0.04f) * coatBrdf.x + coatBrdf.y) * coat * gi.specularOcclusion;

    const float grazing = Pow5(1.0f - NdotV);
    const float3 sheenEnv = gi.sheen * saturate(sheen) *
        saturate(sheenColor) * grazing * (1.0f - saturate(metallic)) * 0.5f * saturate(ao);
    return base + clearcoatSpec + sheenEnv;
}

/// @brief 拡張版の従来口。拡散はキューブだけ。
float3 EvaluateIBLAdvanced(
    float3 N, float3 V, float3 T, float3 B, float anisotropy,
    float3 albedo, float metallic, float roughness, float ao,
    float clearcoat, float clearcoatRoughness,
    float sheen, float3 sheenColor,
    TextureCube irradianceMap, TextureCube prefilterMap,
    Texture2D<float4> brdfLUT, int maxMipLevel,
    float diffuseScale, float specularScale,
    SamplerState samp, SamplerState sampClamp)
{
    const DiffuseGI gi = FBZZ_SkyDiffuse(irradianceMap.Sample(samp, N).rgb);
    return EvaluateIBLAdvancedFromIrradiance(N, V, T, B, anisotropy, albedo, metallic, roughness, ao,
        clearcoat, clearcoatRoughness, sheen, sheenColor, gi, prefilterMap,
        brdfLUT, maxMipLevel, diffuseScale, specularScale, samp, sampClamp);
}

/// @brief 拡張版の Light Probe Volume を受ける口。
/// @param worldPos シェーディング点 [world]。
float3 EvaluateIBLAdvanced(
    float3 worldPos,
    float3 N, float3 V, float3 T, float3 B, float anisotropy,
    float3 albedo, float metallic, float roughness, float ao,
    float clearcoat, float clearcoatRoughness,
    float sheen, float3 sheenColor,
    TextureCube irradianceMap, TextureCube prefilterMap,
    Texture2D<float4> brdfLUT, int maxMipLevel,
    float diffuseScale, float specularScale,
    SamplerState samp, SamplerState sampClamp)
{
    const DiffuseGI gi = FBZZ_DiffuseIrradiance(worldPos, N, irradianceMap, samp, sampClamp);
    return EvaluateIBLAdvancedFromIrradiance(N, V, T, B, anisotropy, albedo, metallic, roughness, ao,
        clearcoat, clearcoatRoughness, sheen, sheenColor, gi, prefilterMap,
        brdfLUT, maxMipLevel, diffuseScale, specularScale, samp, sampClamp);
}

#endif // IBL_HLSLI
