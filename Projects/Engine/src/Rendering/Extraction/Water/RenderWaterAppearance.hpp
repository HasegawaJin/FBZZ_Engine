/// @file    RenderWaterAppearance.hpp
/// @brief   水面・水中用の描画定数、シェーダーと材質の受け渡し。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Scene/References/Entity.hpp>
#include <Graphics/Effects/Surfaces/RenderWaterInput.hpp>
#include <span>

namespace fbzz::renderer { class ResourceManager; struct WaterDetailNoise; }

namespace fbzz::scene {
class Scene;
struct Transform;
struct WaterComponent;
struct WaterSplineSnapshot;

namespace water_render_detail {
class WaterRenderCache;
struct WaterCacheKey;
struct WaterMesh;

struct HorizonMaterialLink {
    std::size_t inputIndex;
    uint64_t receiverKey;
    math::Vector4 mouth;
};

[[nodiscard]] asset::MaterialAsset::AuthoringSnapshotPtr LoadWaterMaterial(const std::string& materialPath);
[[nodiscard]] float ReadWaterMaterialFloat(const asset::MaterialAssetAuthoring* material, const char* name, float fallback);
[[nodiscard]] renderer::WaterCB BuildWaterConstants(const WaterComponent& water,
    const asset::MaterialAssetAuthoring* material, const Transform& transform, const math::Matrix4& viewProjection,
    float time, float skyReflection, const renderer::WaterDetailNoise& detailNoise, renderer::ResourceManager& resources);
void ApplySplineWaterConstants(const WaterComponent& water, const WaterSplineSnapshot& spline,
    renderer::WaterCB& constants);
[[nodiscard]] renderer::WaterMediumInput BuildMediumInput(const WaterComponent& water, const Transform& transform,
    EntityID owner, const asset::MaterialAssetAuthoring* material, const WaterMesh& mesh,
    const renderer::WaterWaveMargin& margin, renderer::WaterCB& constants);
[[nodiscard]] renderer::WaterEffectParams BuildWaterEffectParams(const asset::MaterialAssetAuthoring* material);
void ResolveZoneWaterMaterials(Scene& scene, EntityID owner, const WaterComponent& water, const Transform& transform,
    renderer::RenderWaterInput& input, const renderer::WaterDetailNoise& noise, renderer::ResourceManager& resources);
void BindWaterAppearance(const asset::MaterialAssetAuthoring* material, WaterRenderCache& cache,
    const WaterCacheKey& key, renderer::RenderWaterInput& input, renderer::ResourceManager& resources);
void LinkHorizonWaterMaterials(std::vector<renderer::RenderWaterInput>& waterInputs,
    std::span<const HorizonMaterialLink> materialLinks);

} /// @note namespace water_render_detail
} /// @note namespace fbzz::scene
