/// @file    Terrain.hlsl
/// @brief   ハイトマップ地形を Forward で HDR へ直接ライティングして描く。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note b0 = CameraConstants、b1 = TerrainCB (TerrainSurface.hlsli)、b3 = LightConstants、b4 = ShadowConstants、b8 = AdvancedGraphics。
/// @note t0 / t1 = 層番号 / 重みマップ、t13 = 影。層テクスチャは bindless 添字で引く。
/// @note 頂点レイアウトは TerrainRenderPass.cpp の TerrainVertex (POSITION / NORMAL / TANGENT / TEXCOORD0, 44 バイト) と一致させること。
/// @see Docs/design/terrain-layers.md
#include "Common/Binding.hlsli"

/// @note Common/Constants.hlsli は b1 に ObjectConstants を定義するが、地形は同じ枠を TerrainCB として使うため直接宣言する。
cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float    _camReserved;   ///< Water パスのみ waterSsrEnabled として使う枠
    float    isOrthographic; ///< 1 = 平行投影
    float    _camPad;
};

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS  4

struct PointLightData { float3 position; float range; float3 color; float intensity; };
struct SpotLightData  {
    float3 position; float range;
    float3 direction; float innerCos;
    float3 color; float outerCos;
    float  intensity; float3 _pad;
};

cbuffer LightConstants : register(CB_LIGHT)
{
    float3         lightDir;       float _lightPad;
    float3         lightColor;     float lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int            pointLightCount;
    int            spotLightCount;
    float2         _lightPad2;
    float3         ambientColor;
    float          _ambientPad;
};

/// @note 画面空間 AO / 接触影を Forward でも効かせるため、地形の画素で結果を引く。強度は b8 が運ぶ。
#include "Common/AdvancedGraphicsConstants.hlsli"
#include "Common/ShadowConstants.hlsli"
/// @note Lighting.hlsli は LightConstants の ambientColor を参照するので cbuffer 宣言の後に置く。
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/Wetness.hlsli"
#include "Terrain/TerrainSurface.hlsli"

FBZZ_TEX2D_T(float, g_shadowMap, 13);
SamplerComparisonState g_shadowSampler : register(SAMPLER_SHADOW);

float4 PSMain(TerrainPSInput p) : SV_Target0
{
    TerrainSurface surface = EvaluateTerrainSurface(p, 1.0f);
    float3 N = surface.normal;
    const float3 V = normalize(cameraPos - p.worldPos);
    const float3 L = normalize(-lightDir);

    /// @note 天候は Deferred の TerrainGBuffer.hlsl と同じ値・同じ順序で掛ける。
    const WetSurface wet = ApplyWetness(surface.albedo, surface.roughness, N,
                                        weather.x, weather.y, weather.z);
    const float3 albedo    = wet.albedo;
    const float  roughness = wet.roughness;

    float shadow = ComputeShadow(g_shadowMap, g_shadowSampler, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    /// @note 層の AO (材質の凹凸) と画面空間 AO (形状同士の遮蔽) は別物なので掛け合わせる。
    const float ao = surface.ao * FBZZ_ScreenAO(p.svPosition.xy);

    /// @note AO は環境光だけに掛ける。直射光まで遮ると谷の地形が不自然に暗くなる。
    /// @note 直接光は GBuffer 経路の DeferredLighting と同じ Cook-Torrance (metallic = 0) に揃える。
    float3 result = albedo * ambientColor * ao
                  + Lighting_PBR_Direct(N, V, L, albedo, 0.0f, roughness, lightColor, lightIntensity) * shadow;

    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_PBR_Direct(N, V, ps.L, albedo, 0.0f, saturate(roughness + ps.roughnessBias),
                                      ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    return float4(result, 1.0f);
}
