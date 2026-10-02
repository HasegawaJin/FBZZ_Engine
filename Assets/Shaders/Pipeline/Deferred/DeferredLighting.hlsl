/// @file    DeferredLighting.hlsl
/// @brief   GBuffer を読んで PBR ライティングを当てるディファードライティングパス。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note 頂点バッファなしのフルスクリーン三角形で描き、PS が GBuffer + 深度から worldPos を復元する。

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
/// @note 画面空間 AO / 接触影はこのパスが t9 / t24 を自前で宣言するので、共有ヘッダー側の宣言を外して二重定義を避ける。
#define FBZZ_NO_SCREEN_SHADING 1

#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/ReflectionPolicy.hlsli"
#include "Rendering/HybridGlassPolicy.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texGBuffer0, TEX_GBUFFER0_SLOT);   ///< @note albedo(RGB) + roughness(A)
FBZZ_TEX2D(texGBuffer1, TEX_GBUFFER1_SLOT);   ///< @note normal(RGB, [0,1] 符号化) + metallic(A)
/// @note t3 はこのパスでは emission の MRT。材質テクスチャと異なり、既に線形 HDR。
FBZZ_TEX2D(texGBufferEmission, TEX_EMISSIVE_SLOT);
FBZZ_TEX2D(texDepth, TEX_DEPTH_SLOT);
FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_TEX2D(texSSAO, TEX_SSAO_SLOT);
FBZZ_TEX2D_T(float, texContactShadow, TEX_CONTACT_SHADOW_SLOT); ///< @note t24: 接触影マスク (1=非遮蔽, 0=遮蔽)
/// @note t20 alpha is typed: 1=weighted opaque specular, 2=full dielectric radiance, 0=invalid.
FBZZ_TEX2D_T(float4, texRayReflection, 20);
/// @note t23 は Hybrid resolve 専用。RGB は受け側 Fresnel 適用済み、alpha は反射率と独立した信頼度。
FBZZ_TEX2D_T(float4, texScreenReflection, 23);
SamplerState           sampDefault  : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow   : register(SAMPLER_SHADOW);
SamplerState           sampGBuffer  : register(SAMPLER_POINT_CLAMP);

/// @note IBL テクスチャは AdvancedGraphicsConstants.iblIntensity > 0 のときだけバインドされる。
FBZZ_TEXCUBE(texIBLIrradiance, TEX_IBL_IRRADIANCE_SLOT);  ///< @note t16: 拡散 IBL (半球積分済み)
FBZZ_TEXCUBE(texIBLPrefilter, TEX_IBL_PREFILTER_SLOT);   ///< @note t17: 鏡面 IBL (roughness → mip)
FBZZ_TEX2D_T(float4, texBRDFLut, TEX_IBL_BRDF_LUT_SLOT);   ///< @note t18: RG=scale/bias。起動時に BRDFIntegration.cs.hlsl がベイク
SamplerState      sampLinearClamp  : register(SAMPLER_LINEAR_CLAMP); ///< @note s2。BRDF LUT の UV を [0,1] に収めるため Clamp

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

/// @brief 頂点 ID だけでフルスクリーン三角形の UV / クリップ座標を作る。
FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv          = float2((id & 1u) ? 2.0f : 0.0f,
                           (id & 2u) ? 2.0f : 0.0f);
    o.svPosition  = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float2 uv = p.uv;

    /// @note 最遠 (Reversed-Z で 0) はジオメトリの無い空・背景なので捨てる。
    float  ndcDepth = texDepth.Sample(sampGBuffer, uv).r;
    if (IsFarDepth(ndcDepth)) discard;

    float4 gb0    = texGBuffer0.Sample(sampGBuffer, uv);
    float4 gb1    = texGBuffer1.Sample(sampGBuffer, uv);
    float4 receiverEmission = texGBufferEmission.Sample(sampGBuffer, uv);
    const bool resolveActive = HybridReflectionResolveActive(reflectionResolveEnabled);
    const bool glassReceiver = resolveActive && HybridGlassReceiver(receiverEmission.a);
    /// @note The immutable SSR source excludes authored glass rather than lighting its opaque GBuffer proxy.
    if (reflectionResolveEnabled == HYBRID_REFLECTION_RESOLVE_SOURCE && glassReceiver) return float4(0, 0, 0, 1);
    float4 rayReflection = 0.0f;
    [branch]
    if (rayReflectionEnabled > 0.0f && reflectionResolveEnabled != HYBRID_REFLECTION_RESOLVE_SOURCE)
        rayReflection = texRayReflection.Sample(sampGBuffer, uv);
    /// @note A proven camera-inside/glass result owns complete radiance, including an opaque terminal; Raster lighting must not be added again.
    if (reflectionResolveEnabled == HYBRID_REFLECTION_RESOLVE_FINAL
        && (receiverEmission.a == 0 || receiverEmission.a == HYBRID_GLASS_SUPPORTED)
        && rayReflection.a == HYBRID_REFLECTION_DIELECTRIC_FULL && all(isfinite(rayReflection)) && all(rayReflection.rgb >= 0))
        return float4(rayReflection.rgb, 1);
    float3 col    = gb0.rgb;
    float  rough  = gb0.a;
    /// @note 補間や GBuffer 境界でも単位法線を BRDF へ渡し、鏡面値の発散を防ぐ。
    float3 N      = SafeNormalize(gb1.rgb * 2.0f - 1.0f,
                                  float3(0.0f, 1.0f, 0.0f));
    float  met    = gb1.a;

    /// @note GBuffer は面内でフィルタ済みの最終 roughness。別オブジェクトの法線差を再び混ぜず、RT と同じ下限を使う。
    rough = clamp(rough, 0.045f, 1.0f);
    met   = saturate(met);

    float3 worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);

    float ao = 1.0f;
    if (ssaoIntensity > 0.0f)
    {
        float ssao = texSSAO.Sample(sampLinearClamp, uv).r;
        ao = lerp(1.0f, saturate(ssao), saturate(ssaoIntensity));
    }

    float3 V      = SafeNormalize(cameraPos - worldPos, N);
    float3 L      = SafeNormalize(-lightDir, N);
    float  shadow = ComputeShadow(texShadow, sampShadow, worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);

    /// @note 無効時は RenderSystem が contactShadowStrength=0 を入れるので、未バインドの t24 を読まずに済む。
    /// @note マスクは ContactShadows.cs が strength を織り込み済み。半解像度でも正規化 UV + Linear Clamp でバイリニアに拡大される。
    if (contactShadowStrength > 0.0f)
        shadow *= texContactShadow.Sample(sampLinearClamp, uv);

    const bool rayReflectionValid = rayReflection.a == HYBRID_REFLECTION_OPAQUE_SPECULAR
        && all(isfinite(rayReflection)) && !glassReceiver;
    float4 screenReflection = 0.0f;
    float screenWeight = 0.0f;
    [branch]
    if (reflectionResolveEnabled == HYBRID_REFLECTION_RESOLVE_FINAL && !glassReceiver && reflectionSsrEnabled > 0.0f
        && isfinite(ssrIntensity) && ssrIntensity > 0.0f)
    {
        screenReflection = texScreenReflection.Sample(sampGBuffer, uv);
        if (all(isfinite(screenReflection))) screenWeight = saturate(screenReflection.a * ssrIntensity);
    }
    /// @note Available RT owns glossy metals even when primary validation fails; those holes fall back to IBL, not unproven SSR.
    if (reflectionResolveEnabled == HYBRID_REFLECTION_RESOLVE_FINAL && rayReflectionEnabled > 0.0f
        && HybridReflectionPrefersRay(gb1.a, gb0.a))
        screenWeight = 0.0f;
    const float rayWeight = rayReflectionValid ? 1.0f - screenWeight : 0.0f;
    const float environmentSpecularScale = resolveActive
        ? iblSpecularScale * (1.0f - screenWeight - rayWeight)
        : (rayReflectionValid ? 0.0f : iblSpecularScale);

    /// @note IBL 有効時は環境光を IBL に差し替えて ambientColor を使わない。無効時は定数 ambient (albedo * ambientColor * ao)。
    float3 result;
    if (iblIntensity > 0.0f)
    {
        result = Lighting_PBR_IBL(worldPos, N, V, L, col, met, rough,
                                   lightColor, lightIntensity, shadow, ao,
                                   texIBLIrradiance, texIBLPrefilter, texBRDFLut,
                                   iblMaxMipLevel, iblIntensity,
                                   iblDiffuseScale, environmentSpecularScale,
                                   sampDefault, sampLinearClamp);
    }
    else
    {
        result = Lighting_PBR(N, V, L, col, met, rough,
                              lightColor, lightIntensity, shadow, ao);
    }
    /// @note SCREEN_FIRST は鏡面間接光だけを分配し、直接光・拡散 GI・自発光を変更しない。
    if (reflectionResolveEnabled == HYBRID_REFLECTION_RESOLVE_FINAL)
    {
        /// @note 無効 provider の NaN * 0 を避け、正の weight の値だけを加える。
        if (screenWeight > 0.0f) result += screenReflection.rgb * screenWeight;
        if (rayWeight > 0.0f) result += rayReflection.rgb * rayWeight;
    }
    else if (rayReflectionValid) result += rayReflection.rgb;

    /// @note 点光源 / スポットの走査元 (b3 の固定長配列 / StructuredBuffer / クラスタリスト) は ClusteredLights.hlsli が clusterLightMode で切り替える。
    FBZZ_PUNCTUAL_BEGIN(worldPos, p.svPosition.xy, N)
        result += Lighting_PBR_Direct(N, V, ps.L, col, met, saturate(rough + ps.roughnessBias), ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    /// @note カスケード可視化。無効時は白。Split Lambda と境界ブレンド幅を地面や壁で目視調整するため不透明の主経路に置く。
    result *= ShadowCascadeDebugTint(worldPos);

    /// @note 自発光は影・AO・IBL・デバッグの照明色から独立し、HDR 合成へ一度だけ加える。
    result += receiverEmission.rgb;
    return float4(result, 1.0f);
}
