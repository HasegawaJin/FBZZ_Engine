/// @file    SurfaceMaterialData.hpp
/// @brief   Canonical PBR surfaces, typed textures and dense instance GPU records.
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <array>
#include <cstdint>

namespace fbzz::renderer {

enum class SurfaceMaterialIssue : uint8_t {
    NONE,
    UNSUPPORTED_SHADER,
    TEXTURE_UNSUPPORTED,
    ADVANCED_LOBES_UNSUPPORTED,
    INVALID_CONSTANTS,
    SOLID_DIELECTRIC_UNSUPPORTED,
    TEXTURE_UNAVAILABLE,
    TEXTURED_EMISSION_UNSUPPORTED,
};

/// @note Version 0 means the backend cannot prove stable CPU-authored texture content.
struct RayTextureBinding {
    ResourceHandle<TextureTag> texture;
    uint64_t contentVersion = 0;
    bool operator==(const RayTextureBinding&) const = default;
};

/// @note alpha は被覆だけを表し、transmission と乗算しない。古い材質は transmission=0。
/// @note attenuationColor は線形 RGB の距離 attenuationDistance [m] に対する透過率。
/// @note Solid interfaces switch media using geometric normals; thin sheets do not invent a solid interior.
/// @see https://pbr-book.org/4ed/Volume_Scattering/Transmittance Beer の吸収。
struct SolidDielectricSettings {
    float transmission = 0;
    float ior = 1.5f;
    math::Vector3 attenuationColor{1, 1, 1};
    float attenuationDistance = 1;
    bool thinWalled = false;
    [[nodiscard]] bool operator==(const SolidDielectricSettings& other) const;
};

/// @note 色・emission は線形 RGB。emission は emissiveColor と emissiveScale の積。
/// @note opaque roughness は標準 PBR 下限 0.045。dielectric は smooth 判定用の元値を保持する。
/// @note geometry / opacity とは独立した能力。未対応材質を標準 PBR として照明しない。
struct SurfaceMaterialData {
    math::Vector4 baseColor{1, 1, 1, 1};
    math::Vector3 emission{};
    float metallic = 0;
    float roughness = 0.5f;
    bool standardSurfaceSupported = false;
    SurfaceMaterialIssue issue = SurfaceMaterialIssue::UNSUPPORTED_SHADER;
    SolidDielectricSettings dielectric;
    bool solidDielectricSupported = false;
    std::array<RayTextureBinding, 5> textures{};
    std::array<float, 2> uvTiling{1, 1};
    std::array<float, 2> uvOffset{};
    float normalStrength = 1;
    float occlusionStrength = 1;
    float alphaCutoff = 0;
    float explicitTextureLod = 0;
    uint32_t textureMask = 0;

    /// @note snapshotSerial に依存せず float の bit pattern を比較し、微小な材質変更も検出する。
    [[nodiscard]] bool operator==(const SurfaceMaterialData& other) const;
};

/// @note InstanceID ごとに 128 bytes。前半 64 bytes は旧定数表と同じ並び。
/// @note supported=1 は standard PBR、2 は dielectric。dielectricFlags bit0 は thin-walled。
/// @note textureSrv は albedo/normal/metallicRough/emissive/AO。LOD 0 の明示サンプルで画面微分を代用しない。
struct RaySurfaceRecord {
    std::array<float, 4> baseColor{1, 1, 1, 1};
    std::array<float, 3> emission{};
    float metallic = 0;
    float roughness = 0.5f;
    uint32_t supported = 0;
    float transmission = 0;
    float ior = 1.5f;
    std::array<float, 3> attenuationColor{1, 1, 1};
    float attenuationDistance = 1;
    std::array<float, 2> uvTiling{1, 1};
    std::array<float, 2> uvOffset{};
    float normalStrength = 1;
    float alphaCutoff = 0;
    float explicitTextureLod = 0;
    uint32_t textureMask = 0;
    std::array<uint32_t, 5> textureSrv{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t dielectricFlags = 0;
    float occlusionStrength = 1;
    uint32_t reserved = 0;
    bool operator==(const RaySurfaceRecord&) const = default;
};
static_assert(sizeof(RaySurfaceRecord) == 128);

/// @note Engine が canonical standard PBR を検証する。GBuffer の raster route は証明に使わない。
/// @note 初期は全 texture と追加ローブを未対応にする。geometry / opacity の判定は変更しない。
/// @note dielectric は full transmission / nonmetal / nonemissive / alpha=1。rough thin sheets と dielectric textures は未対応。
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF Independent dielectric transmission model.
/// @see Docs/design/RayTracing.md §Raster と RT の表面モデル
[[nodiscard]] SurfaceMaterialData ResolveConstantSurfaceMaterial(
    const std::array<uint8_t, 96>& gbufferParams, bool standardPbr,
    bool usesTextures = false, bool advancedLobes = false,
    const SolidDielectricSettings& dielectric = {});

/// @note Canonical slots and encoded UNORM views only. Textured emission and dielectric textures remain unsupported.
/// @see Assets/Shaders/Material/Surface/PBR.hlsl PSMain texture and UV conventions.
[[nodiscard]] SurfaceMaterialData ResolveTexturedSurfaceMaterial(
    const std::array<uint8_t, 96>& gbufferParams, bool standardPbr,
    const std::array<RayTextureBinding, 5>& textures, bool unresolvedTexture = false,
    bool advancedLobes = false, const SolidDielectricSettings& dielectric = {});

/// @note 非有限値を GPU へ渡さず supported=0 とする。材質表だけの更新で AS を再構築しない。
[[nodiscard]] RaySurfaceRecord MakeRaySurfaceRecord(const SurfaceMaterialData& material);
/// @note Hybrid supports constant rough/smooth solids and smooth, unabsorbing thin sheets; partial transmission and dielectric textures remain unsupported.
/// @see Assets/Shaders/RayTracing/RayDielectric.hlsli RayDielectricSmooth and thin-sheet contracts.
[[nodiscard]] bool IsHybridRayDielectricSupported(const SurfaceMaterialData& material);
/// @return 0=opaque, 1=Hybrid-supported dielectric, 2=authored but unsupported dielectric; unsupported glass is not SSR source radiance.
[[nodiscard]] float ResolveHybridRaySurfaceMarker(const SurfaceMaterialData& material);
/// @note The canonical 96B GBuffer material reserves float@84 for the typed surface marker; all other bytes remain unchanged.
[[nodiscard]] std::array<uint8_t, 96> MakeHybridGBufferParams(
    const std::array<uint8_t, 96>& canonicalParams, const SurfaceMaterialData& material);
[[nodiscard]] const char* DescribeSurfaceMaterialIssue(SurfaceMaterialIssue issue);

} /// @note namespace fbzz::renderer
