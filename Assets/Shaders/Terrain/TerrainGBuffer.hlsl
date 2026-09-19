/// @file    TerrainGBuffer.hlsl
/// @brief   地形を Deferred GBuffer (MRT) へ書き出す。ライティングは DeferredLighting に委ねる。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note GBuffer へ書くことで GTAO / SSAO / ContactShadows / SSR / IBL が他の不透明物と同じように地形へ効く。
/// @note LAYOUT: SV_Target0 = albedo(linear) / roughness、SV_Target1 = worldNormal*0.5+0.5 / metallic (Pipeline/Deferred/GBuffer.hlsl と一致)。
/// @see Docs/design/terrain-layers.md
#include "Common/Binding.hlsli"
#include "Rendering/SpecularAA.hlsli"
#include "Rendering/Wetness.hlsli"
#include "Terrain/TerrainSurface.hlsli"

struct GBufferOut
{
    float4 albedoRoughness : SV_Target0;
    float4 normalMetallic  : SV_Target1;
};

/// @note Deferred の地形はマット面で鏡面反射が弱く、法線マップの陰影が Forward より地味になる。
///       XY を増幅して拡散陰影 (N·L) だけでも凹凸を出す。層ごとの強弱は .mat の Normal Strength が持つ。
static const float kTerrainNormalBoost = 3.0f;
/// @note 層の roughness を尊重しつつ、青空 IBL が鏡のように映るのを防ぐ控えめな下限。
static const float kTerrainMinRoughness = 0.6f;

GBufferOut PSMain(TerrainPSInput p)
{
    TerrainSurface surface = EvaluateTerrainSurface(p, kTerrainNormalBoost);
    const float3 N = surface.normal;

    /// @note AO は GBuffer にチャンネルが無く、画面空間 GTAO/SSAO が担う (メッシュと同じ扱い)。
    float roughness = max(saturate(surface.roughness), kTerrainMinRoughness);

    /// @note 天候。地形は b1 を TerrainCB に使うので b8 を宣言できず、値は TerrainCB から手渡す。
    const WetSurface wet = ApplyWetness(surface.albedo, roughness, N,
                                        weather.x, weather.y, weather.z);
    roughness = wet.roughness;

    /// @note 増幅した法線は遠景で 1 画素あたりの振れが大きく太陽の反射がタイル状に明滅するので、その分を粗さへ返す。
    roughness = FilterSpecularRoughness(N, roughness);

    GBufferOut o;
    o.albedoRoughness = float4(wet.albedo, roughness);
    o.normalMetallic  = float4(N * 0.5f + 0.5f, 0.0f);
    return o;
}
