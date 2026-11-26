/// @file    RenderWaterTextures.cpp
/// @brief   水深と岸際の泡を保持する地形テクスチャの解決と転送。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderWaterAppearance.hpp"
#include "RenderWaterResources.hpp"
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Water/Surface/WaterDepthField.hpp>
#include <Engine/Scene/Water/Surface/WaterSurfaceQuery.hpp>
#include <Graphics/Renderer/Resources/Textures/ITexture.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace fbzz::scene::water_render_detail {

namespace {

/// @note 一定値のチャンネルはバイリニア補間でも値を保つ。水底データのないアトラスでは設定した一定の深さを保つ。
bool HasUniformPhysicalDepth(const std::vector<uint8_t>& rgba, float depthRange, float depthLimit)
{
    if (rgba.empty() || rgba.size() % 4u != 0u) {
        return false;
    }
    const uint8_t flags = rgba[3u];
    if ((flags & 2u) != 0u) {
        return false;
    }
    const bool hasBottom = (flags & 1u) != 0u;
    for (size_t pixel = 0; pixel < rgba.size(); pixel += 4u) {
        if (rgba[pixel + 3u] != flags) {
            return false;
        }
        if (hasBottom && (rgba[pixel + 1u] != rgba[1u] || rgba[pixel + 2u] != rgba[2u])) {
            return false;
        }
    }
    if (!hasBottom) {
        return true;
    }
    float depth = 0.0f;
    return TryDecodeWaterTerrainDepth(rgba[1u], rgba[2u], depthRange, depth) && depth >= depthLimit;
}

TerrainHeightRegion FindWaterDepthChanges(const WaterDepthTextureSource& previous, const WaterDepthTextureSource& current)
{
    TerrainHeightRegion changes;
    const size_t rowBytes = static_cast<size_t>(current.width) * 4u;
    for (uint32_t z = 0; z < current.height; ++z) {
        const size_t byteOffset = z * rowBytes;
        const auto* oldRow = previous.rgba->data() + byteOffset;
        const auto* newRow = current.rgba->data() + byteOffset;
        if (std::memcmp(oldRow, newRow, rowBytes) == 0) {
            continue;
        }
        int first = 0, last = static_cast<int>(current.width) - 1;
        while (std::memcmp(oldRow + first * 4u, newRow + first * 4u, 4u) == 0) {
            ++first;
        }
        while (std::memcmp(oldRow + last * 4u, newRow + last * 4u, 4u) == 0) {
            --last;
        }
        changes.Include({first, static_cast<int>(z), last, static_cast<int>(z)});
    }
    return changes;
}

bool CanUploadWaterDepth(const WaterDepthField* field, float attenuationDepth)
{
    return field && field->attenuationDepth == attenuationDepth && field->width && field->height
        && field->width <= MAX_HORIZON_WATER_TERRAIN_RESOLUTION && field->height <= MAX_HORIZON_WATER_TERRAIN_RESOLUTION
        && field->rgba.size() == static_cast<size_t>(field->width) * field->height * 4u;
}

void UpdateWaterDepthTexture(WaterTextures& textures, WaterDepthTextureSource depthSource,
    bool replaceTexture, renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    const auto& previous = textures.uploadedDepth;
    const bool sameDimensions = previous.rgba && previous.width == depthSource.width && previous.height == depthSource.height;
    const auto* pixels = depthSource.rgba->data();
    const uint32_t rowPitch = depthSource.width * 4u;
    if (!replaceTexture && sameDimensions && textures.terrainInfoTexture.IsValid()) {
        if (previous.rgba == depthSource.rgba) {
            return;
        }
        const auto changedRegion = FindWaterDepthChanges(previous, depthSource);
        if (changedRegion.Empty()) {
            textures.uploadedDepth = std::move(depthSource);
            return;
        }
        if (textures.dynamicDepth || !textures.dynamicDepthUnavailable) {
            bool depthUploadSucceeded = false;
            // 同一フレーム内の更新は、このテクスチャを参照する先行描画の後に投入する。
            renderer.ExecuteResourceOperation([&] {
                if (textures.dynamicDepth) {
                    if (auto* texture = resources.Get(textures.terrainInfoTexture)) {
                        const size_t byteOffset = static_cast<size_t>(changedRegion.minZ) * rowPitch + changedRegion.minX * 4u;
                        depthUploadSucceeded = texture->UpdateRegion(changedRegion.minX, changedRegion.minZ,
                            changedRegion.maxX - changedRegion.minX + 1, changedRegion.maxZ - changedRegion.minZ + 1,
                            pixels + byteOffset, rowPitch);
                    }
                    return;
                }
                const auto replacement = resources.CreateDynamicTexture(depthSource.width, depthSource.height,
                    renderer::DynamicTextureFormat::RGBA8);
                if (auto* texture = resources.Get(replacement)) {
                    depthUploadSucceeded = texture->UpdateRegion(0, 0, depthSource.width, depthSource.height, pixels, rowPitch);
                }
                if (depthUploadSucceeded) {
                    resources.Release(textures.terrainInfoTexture);
                    textures.terrainInfoTexture = replacement;
                    textures.dynamicDepth = true;
                } else if (replacement.IsValid()) {
                    resources.Release(replacement);
                } else {
                    textures.dynamicDepthUnavailable = true;
                }
            });
            if (depthUploadSucceeded) {
                textures.uploadedDepth = std::move(depthSource);
                return;
            }
        }
    }

    if (textures.terrainInfoTexture.IsValid()) {
        resources.Release(textures.terrainInfoTexture);
    }
    textures.terrainInfoTexture = resources.CreateTexture(pixels, depthSource.width, depthSource.height);
    textures.dynamicDepth = false;
    textures.uploadedDepth = textures.terrainInfoTexture.IsValid() ? std::move(depthSource) : WaterDepthTextureSource{};
}

}

WaterTextureRequest ResolveWaterTextureRequest(const WaterComponent& water, const Transform& transform,
    const WaterTerrainInfoInputs& terrainInputs, bool validTerrainInputs, const asset::MaterialAssetAuthoring* material)
{
    const float foamThreshold = ReadWaterMaterialFloat(material, "foamThreshold", 0.3f);
    const float foamFade      = ReadWaterMaterialFloat(material, "foamFade",      0.5f);
    WaterTextureRequest request;
    auto& bakeSettings = request.bakeSettings;
    const auto terrainResolution = ResolveWaterTerrainBakeResolution(
        terrainInputs, water, transform, WaterSurfaceReadView::SIMULATION);
    bakeSettings.width = static_cast<uint32_t>(terrainResolution.x);
    bakeSettings.height = static_cast<uint32_t>(terrainResolution.y);
    bakeSettings.foamThreshold = (std::max)(foamThreshold, 0.0f);
    bakeSettings.foamFade = (std::max)(foamFade, 0.0f);
    const bool validTerrainSettings = TryComputeWaterTerrainInfoSignature(
        terrainInputs, water, transform, bakeSettings, request.signature,
        WaterSurfaceReadView::SIMULATION);
    if (const WaterDepthField* depthField = water.EffectiveDepthField(WaterSurfaceReadView::SIMULATION);
        CanUploadWaterDepth(depthField, water.EffectiveWaveAttenuationDepth(WaterSurfaceReadView::SIMULATION))) {
        request.signature = depthField->signature;
    }
    request.canBakeTerrain = validTerrainInputs && validTerrainSettings;
    return request;
}

void UpdateWaterTextures(
    WaterTextures& textures,
    const WaterComponent& water,
    const Transform& transform,
    const WaterTerrainInfoInputs& terrainInputs,
    const WaterTextureRequest& request,
    bool replaceTexture,
    renderer::IRenderer& renderer,
    renderer::ResourceManager& resources)
{
    textures.signature = request.signature;
    textures.terrainInfoParams = {};
    textures.terrainDomain = {};
    textures.depthIsUniform = false;
    const auto& depthField = water.simulationReadEnabled && water.simulationSurfacePublished
        ? water.simulationDepthField : water.depthField;
    const float attenuationDepth = water.EffectiveWaveAttenuationDepth(WaterSurfaceReadView::SIMULATION);
    if (CanUploadWaterDepth(depthField.get(), attenuationDepth)) {
        const auto& field = *depthField;
        textures.depthIsUniform = HasUniformPhysicalDepth(field.rgba, field.depthRange, water.buoyancyDepth);
        UpdateWaterDepthTexture(textures, {field.width, field.height, {depthField, &field.rgba}},
            replaceTexture, renderer, resources);
        textures.terrainDomain = field.domain;
        if (textures.terrainInfoTexture.IsValid()) {
            textures.terrainInfoParams = {1.0f, field.depthRange,
                static_cast<float>(field.width), static_cast<float>(field.height)};
        }
        return;
    }
    std::shared_ptr<const WaterTerrainInfoAtlas> atlas;
    if (request.canBakeTerrain && textures.terrainAtlas.TryBakeSnapshot(terrainInputs, water, transform,
            request.bakeSettings, atlas,
            WaterSurfaceReadView::SIMULATION)) {
        textures.depthIsUniform = HasUniformPhysicalDepth(atlas->rgba, atlas->depthRange, water.buoyancyDepth);
        UpdateWaterDepthTexture(textures, {atlas->width, atlas->height, {atlas, &atlas->rgba}},
            replaceTexture, renderer, resources);
        textures.terrainDomain = atlas->domain;
        if (textures.terrainInfoTexture.IsValid()) {
            textures.terrainInfoParams = {1.0f, atlas->depthRange,
                static_cast<float>(atlas->width), static_cast<float>(atlas->height)};
        }
    } else {
        if (textures.terrainInfoTexture.IsValid()) {
            resources.Release(textures.terrainInfoTexture);
        }
        textures.uploadedDepth = {};
        textures.dynamicDepth = false;
        const std::array<uint8_t, 4> empty{};
        textures.terrainInfoTexture = resources.CreateTexture(empty.data(), 1, 1);
    }
}

}
