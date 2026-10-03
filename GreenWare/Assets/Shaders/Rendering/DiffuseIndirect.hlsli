/// @file    DiffuseIndirect.hlsli
/// @brief   Raster と ray hit が共有する近似拡散間接光の材質応答。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#ifndef FBZZ_DIFFUSE_INDIRECT_HLSLI
#define FBZZ_DIFFUSE_INDIRECT_HLSLI
#include "Rendering/BRDF.hlsli"

/// @note Irradiance は放射照度 / PI。SH/キューブの近似値であり、経路追跡 GI ではない。
/// @see https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf Karis, Image-Based Lighting
float3 FBZZ_DiffuseIndirectResponse(float3 N, float3 V, float3 albedo, float metallic,
    float roughness, float3 irradiance, float diffuseScale)
{
    float3 F0 = lerp(0.04f.xxx, albedo, metallic);
    float3 F = F_SchlickRoughness(saturate(dot(N, V)), F0, roughness);
    return (1.0f - F) * (1.0f - metallic) * irradiance * albedo * max(diffuseScale, 0.0f);
}

/// @note 既存 Raster の小さな中立バウンス近似。物理 GI や AO ではなく、照明へ一度だけ加算する。
float3 FBZZ_DiffuseIndirectFloor(float3 albedo) { return albedo * 0.025f; }
#endif
