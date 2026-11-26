/// @file    SurfaceMaterialData.hpp
/// @brief   Canonical PBR surfaces, typed textures and dense instance GPU records.
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Api.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
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
    ResourceHandle<TextureTag> texture; ///< レイ材質評価で読むテクスチャ資源の非所有ハンドル。
    uint64_t contentVersion = 0; ///< 同じハンドル上の更新を区別する内容版。0 は証明不能。
    bool operator==(const RayTextureBinding&) const = default;
};

/// @note alpha は被覆だけを表し、transmission と乗算しない。古い材質は transmission=0。
/// @note attenuationColor は線形 RGB の距離 attenuationDistance [m] に対する透過率。
/// @note Solid interfaces switch media using geometric normals; thin sheets do not invent a solid interior.
/// @see https://pbr-book.org/4ed/Volume_Scattering/Transmittance Beer の吸収。
struct SolidDielectricSettings {
    float transmission = 0; ///< アルファ被覆とは独立した誘電体の透過量。
    float ior = 1.5f; ///< 入射境界の Fresnel と屈折方向の計算に使う屈折率。
    math::Vector3 attenuationColor{1, 1, 1}; ///< 基準距離を通過した光のシーン線形 RGB 透過率。
    float attenuationDistance = 1; ///< `attenuationColor` の透過率になる媒質内の距離 [m]。
    bool thinWalled = false; ///< 内側の体積を持たない薄板として境界を評価するか。
    [[nodiscard]] FBZZ_GRAPHICS_API bool operator==(const SolidDielectricSettings& other) const;
};

/// @note 色・emission は線形 RGB。emission は emissiveColor と emissiveScale の積。
/// @note opaque roughness は標準 PBR 下限 0.045。dielectric は smooth 判定用の元値を保持する。
/// @note geometry / opacity とは独立した能力。未対応材質を標準 PBR として照明しない。
struct SurfaceMaterialData {
    math::Vector4 baseColor{1, 1, 1, 1}; ///< シーン線形 RGB の基準色と、独立した被覆率の A。
    math::Vector3 emission{}; ///< 発光色と強度の積で得るシーン線形 RGB 放射輝度。
    float metallic = 0; ///< 標準 PBR で誘電体と金属の反射を混ぜる係数。
    float roughness = 0.5f; ///< 表面の粗さ。誘電体では対応可否の判定に使う元値を保持する。
    bool standardSurfaceSupported = false; ///< 標準 PBR 表面としてレイ照明に使用できるか。
    SurfaceMaterialIssue issue = SurfaceMaterialIssue::UNSUPPORTED_SHADER; ///< レイ照明で表面を扱えない理由。
    SolidDielectricSettings dielectric; ///< 中実または薄板の誘電体境界の輸送入力。
    bool solidDielectricSupported = false; ///< 対応済みの誘電体の輸送契約を満たすか。
    std::array<RayTextureBinding, 5> textures{}; ///< 基準色・法線・金属度と粗さ・発光・AO の各レイ材質テクスチャ。
    std::array<float, 2> uvTiling{1, 1}; ///< UV の各軸へ掛ける倍率。
    std::array<float, 2> uvOffset{}; ///< タイル倍率を適用した UV の各軸へ足すオフセット。
    float normalStrength = 1; ///< 法線マップによる法線変化の強さ。
    float occlusionStrength = 1; ///< AO テクスチャによる間接照明の減衰を混ぜる強さ。
    float alphaCutoff = 0; ///< アルファ被覆候補を通すためのしきい値。
    float explicitTextureLod = 0; ///< 画面微分を使わずレイ材質サンプルに指定する mip レベル。
    uint32_t textureMask = 0; ///< 使用可能な材質テクスチャのスロットを示すビット集合。

    /// @note snapshotSerial に依存せず float の bit pattern を比較し、微小な材質変更も検出する。
    [[nodiscard]] FBZZ_GRAPHICS_API bool operator==(const SurfaceMaterialData& other) const;
};

/// @note InstanceID ごとに 128 bytes。前半 64 bytes は旧定数表と同じ並び。
/// @note supported=1 は standard PBR、2 は dielectric。dielectricFlags bit0 は thin-walled。
/// @note textureSrv は albedo/normal/metallicRough/emissive/AO。LOD 0 の明示サンプルで画面微分を代用しない。
struct RaySurfaceRecord {
    std::array<float, 4> baseColor{1, 1, 1, 1}; ///< シェーダーに渡すシーン線形 RGB の基準色と A の被覆率。
    std::array<float, 3> emission{}; ///< シェーダーに渡すシーン線形 RGB の発光放射輝度。
    float metallic = 0; ///< 標準 PBR で使う金属度。
    float roughness = 0.5f; ///< シェーダーで評価する表面の粗さ。
    uint32_t supported = 0; ///< 表面モデルの分類。0 は未対応、1 は標準 PBR、2 は誘電体。
    float transmission = 0; ///< 誘電体表面で使う被覆率とは独立した透過量。
    float ior = 1.5f; ///< 誘電体境界を評価する屈折率。
    std::array<float, 3> attenuationColor{1, 1, 1}; ///< 基準距離に対するシーン線形 RGB の透過率。
    float attenuationDistance = 1; ///< 媒質内でこの透過率を適用する基準距離 [m]。
    std::array<float, 2> uvTiling{1, 1}; ///< 材質テクスチャをサンプリングする UV の軸別倍率。
    std::array<float, 2> uvOffset{}; ///< 倍率を適用した UV の軸別オフセット。
    float normalStrength = 1; ///< 法線マップから受ける法線変化の強さ。
    float alphaCutoff = 0; ///< アルファ被覆判定に用いるしきい値。
    float explicitTextureLod = 0; ///< 画面微分を使わない材質サンプルの mip レベル。
    uint32_t textureMask = 0; ///< 有効な `textureSrv` のスロットを示すビット集合。
    std::array<uint32_t, 5> textureSrv{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}; ///< 基準色・法線・金属度と粗さ・発光・AO の bindless SRV 添字。
    uint32_t dielectricFlags = 0; ///< 誘電体のフラグ。ビット 0 は薄板。
    float occlusionStrength = 1; ///< AO テクスチャの効果を混ぜる強さ。
    uint32_t reserved = 0; ///< GPU レコードの配置を保つ予約領域。
    bool operator==(const RaySurfaceRecord&) const = default;
};
static_assert(sizeof(RaySurfaceRecord) == 128);

/// @note Engine が canonical standard PBR を検証する。GBuffer の raster route は証明に使わない。
/// @note 初期は全 texture と追加ローブを未対応にする。geometry / opacity の判定は変更しない。
/// @note dielectric は full transmission / nonmetal / nonemissive / alpha=1。rough thin sheets と dielectric textures は未対応。
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF Independent dielectric transmission model.
/// @see Docs/design/RayTracing.md §Raster と RT の表面モデル
[[nodiscard]] FBZZ_GRAPHICS_API SurfaceMaterialData ResolveConstantSurfaceMaterial(
    const std::array<uint8_t, 96>& gbufferParams, bool standardPbr,
    bool usesTextures = false, bool advancedLobes = false,
    const SolidDielectricSettings& dielectric = {});

/// @note Canonical slots and encoded UNORM views only. Textured emission and dielectric textures remain unsupported.
/// @see Assets/Shaders/Material/Surface/PBR.hlsl PSMain texture and UV conventions.
[[nodiscard]] FBZZ_GRAPHICS_API SurfaceMaterialData ResolveTexturedSurfaceMaterial(
    const std::array<uint8_t, 96>& gbufferParams, bool standardPbr,
    const std::array<RayTextureBinding, 5>& textures, bool unresolvedTexture = false,
    bool advancedLobes = false, const SolidDielectricSettings& dielectric = {});

/// @note 非有限値を GPU へ渡さず supported=0 とする。材質表だけの更新で AS を再構築しない。
[[nodiscard]] FBZZ_GRAPHICS_API RaySurfaceRecord MakeRaySurfaceRecord(const SurfaceMaterialData& material);
/// @note Hybrid supports constant rough/smooth solids and smooth, unabsorbing thin sheets; partial transmission and dielectric textures remain unsupported.
/// @see Assets/Shaders/RayTracing/RayDielectric.hlsli RayDielectricSmooth and thin-sheet contracts.
[[nodiscard]] FBZZ_GRAPHICS_API bool IsHybridRayDielectricSupported(const SurfaceMaterialData& material);
/// @return 0=opaque, 1=Hybrid-supported dielectric, 2=authored but unsupported dielectric; unsupported glass is not SSR source radiance.
[[nodiscard]] FBZZ_GRAPHICS_API float ResolveHybridRaySurfaceMarker(const SurfaceMaterialData& material);
/// @note The canonical 96B GBuffer material reserves float@84 for the typed surface marker; all other bytes remain unchanged.
[[nodiscard]] FBZZ_GRAPHICS_API std::array<uint8_t, 96> MakeHybridGBufferParams(
    const std::array<uint8_t, 96>& canonicalParams, const SurfaceMaterialData& material);
[[nodiscard]] FBZZ_GRAPHICS_API const char* DescribeSurfaceMaterialIssue(SurfaceMaterialIssue issue);

} // namespace fbzz::renderer
