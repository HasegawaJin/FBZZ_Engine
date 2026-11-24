/// @file    RenderTerrainMaterials.cpp
/// @brief   地形レイヤーの材質・テクスチャと GPU パラメーター。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderTerrainExtraction.hpp"
#include "../../../Asset/Streaming/CacheableLoadedReference.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Graphics/Renderer/Resources/Buffers/IStructuredBuffer.hpp>
#include <Graphics/Renderer/Resources/Textures/ITexture.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <algorithm>
#include <cstring>

namespace fbzz::scene {

static float ReadLayerFloat(const asset::MaterialAssetAuthoring* material, const std::string& name, float fallback)
{
    if (!material) return fallback;
    auto parameter = material->params.find(name);
    if (parameter != material->params.end() && !parameter->second.empty()) return parameter->second[0];
    return fallback;
}

/// @note 保持するのはハンドルだけ。抽出のたびに現在のアセットを参照し、編集や再インポートを反映する。
/// @see Docs/design/instanced-mesh.md
static asset::MaterialAsset::AuthoringSnapshotPtr ReadTerrainMaterial(TerrainRenderCache& cache, const std::string& path)
{
    if (path.empty()) return {};
    const auto revision = asset::AssetDatabase::Revision();
    const auto generation = asset::AssetManager::GetAssetGeneration();
    if (cache.materialIndexRevision != revision || cache.materialAssetGeneration != generation) {
        cache.materials.clear(); cache.materialIndexRevision = revision; cache.materialAssetGeneration = generation;
    }
    if (const auto found = cache.materials.find(path); found != cache.materials.end()) {
        if (const auto* material = asset::AssetManager::Get(found->second)) return material->ReadAuthoring();
        cache.materials.erase(found);
    }
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    const auto* material = asset::AssetManager::Get(handle);
    if (material && cache.materials.size() < 4096 && asset::detail::IsCacheableLoadedReference(path))
        cache.materials.emplace(path, handle);
    return material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
}
static std::string ReadLayerTexturePath(const asset::MaterialAssetAuthoring* material, const std::string& name)
{
    if (!material) return {};
    auto parameter = material->textures.find(name);
    if (parameter != material->textures.end()) return parameter->second;
    return {};
}

static TerrainLayerGpu ReadTerrainLayerParamsImpl(const asset::MaterialAssetAuthoring* material, std::string_view keyPrefix)
{
    const std::string prefix(keyPrefix);
    const auto get = [&](const char* key, float fallback) { return ReadLayerFloat(material, prefix + key, fallback); };
    TerrainLayerGpu layer;
    layer.tilingX            = get("tilingX", 8.0f);
    layer.tilingZ            = get("tilingZ", 8.0f);
    layer.normalStrength     = get("normalStrength", 1.0f);
    layer.roughness          = get("roughness", 0.8f);
    layer.ambientOcclusion   = get("ambientOcclusion", 1.0f);
    layer.hasAoRoughness     = ReadLayerTexturePath(material, prefix + "ao_roughness").empty() ? 0.0f : 1.0f;
    layer.hasHeight          = ReadLayerTexturePath(material, prefix + "height").empty() ? 0.0f : 1.0f;
    layer.heightBlend        = get("heightBlend", 0.0f);
    layer.autoMinHeight      = get("autoMinHeight", -10000.0f);
    layer.autoMaxHeight      = get("autoMaxHeight", 10000.0f);
    layer.autoHeightFade     = get("autoHeightFade", 1.0f);
    layer.autoBlendEnabled   = get("autoBlendEnabled", 0.0f);
    layer.autoMinSlope       = get("autoMinSlope", 0.0f);
    layer.autoMaxSlope       = get("autoMaxSlope", 1.0f);
    layer.autoSlopeFade      = get("autoSlopeFade", 0.1f);
    layer.autoBlendStrength  = get("autoBlendStrength", 1.0f);
    layer.triplanar          = get("triplanar", 0.0f);
    layer.triplanarSharpness = get("triplanarSharpness", 4.0f);
    layer.macroScale         = get("macroScale", 0.1f);
    layer.macroStrength      = get("macroStrength", 0.0f);
    return layer;
}

TerrainLayerGpu ReadTerrainLayerParams(const asset::MaterialAssetAuthoring& material, std::string_view keyPrefix)
{
    return ReadTerrainLayerParamsImpl(&material, keyPrefix);
}

TerrainLayerGpu ReadTerrainLayerParams(const asset::MaterialAsset* material, std::string_view keyPrefix)
{
    const auto snapshot = material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
    return ReadTerrainLayerParamsImpl(snapshot ? &snapshot->Data() : nullptr, keyPrefix);
}

bool TerrainFallbackTextures::IsValid() const
{
    return white.IsValid() && flatNormal.IsValid() && black.IsValid() && gray.IsValid();
}

TerrainFallbackTextures CreateTerrainFallbackTextures(renderer::ResourceManager& resources)
{
    TerrainFallbackTextures fallbackTextures;
    const uint8_t white[4]  = { 255, 255, 255, 255 };
    const uint8_t normal[4] = { 128, 128, 255, 255 };
    const uint8_t black[4]  = {   0,   0,   0, 255 };
    const uint8_t gray[4]   = { 128, 128, 128, 255 };
    fallbackTextures.white      = resources.CreateTexture(white, 1, 1);
    fallbackTextures.flatNormal = resources.CreateTexture(normal, 1, 1);
    fallbackTextures.black      = resources.CreateTexture(black, 1, 1);
    fallbackTextures.gray       = resources.CreateTexture(gray, 1, 1);
    return fallbackTextures;
}

void ReleaseTerrainFallbackTextures(renderer::ResourceManager& resources, TerrainFallbackTextures& textures)
{
    resources.Release(textures.white);
    resources.Release(textures.flatNormal);
    resources.Release(textures.black);
    resources.Release(textures.gray);
    textures = {};
}

/// @return テクスチャの bindless 添字。未ロード・発行失敗なら代替の添字。
static uint32_t BindlessIndexOr(renderer::ResourceManager& resources,
                                renderer::ResourceHandle<renderer::TextureTag> texture,
                                renderer::ResourceHandle<renderer::TextureTag> fallback)
{
    if (const renderer::ITexture* textureResource = resources.Get(texture)) {
        const uint32_t index = textureResource->GetBindlessIndex();
        if (index != renderer::INVALID_BINDLESS_INDEX) return index;
    }
    if (const renderer::ITexture* fallbackResource = resources.Get(fallback))
        return fallbackResource->GetBindlessIndex();
    return renderer::INVALID_BINDLESS_INDEX;
}

static void FillTerrainLayerTextureIndicesImpl(renderer::ResourceManager& resources,
                                    const asset::MaterialAssetAuthoring* material,
                                    std::string_view keyPrefix,
                                    const TerrainFallbackTextures& fallback,
                                    TerrainLayerGpu& layer)
{
    const std::string prefix(keyPrefix);
    const auto load = [&](const char* key) {
        const std::string path = ReadLayerTexturePath(material, prefix + key);
        return path.empty() ? renderer::ResourceHandle<renderer::TextureTag>{} : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, path);
    };
    layer.diffuseIndex     = BindlessIndexOr(resources, load("diffuse"), fallback.white);
    layer.normalIndex      = BindlessIndexOr(resources, load("normal"), fallback.flatNormal);
    layer.aoRoughnessIndex = BindlessIndexOr(resources, load("ao_roughness"), fallback.black);
    layer.heightIndex      = BindlessIndexOr(resources, load("height"), fallback.gray);
}

void FillTerrainLayerTextureIndices(renderer::ResourceManager& resources,
                                    const asset::MaterialAssetAuthoring& material,
                                    std::string_view keyPrefix,
                                    const TerrainFallbackTextures& fallback,
                                    TerrainLayerGpu& layer)
{
    FillTerrainLayerTextureIndicesImpl(resources, &material, keyPrefix, fallback, layer);
}

void FillTerrainLayerTextureIndices(renderer::ResourceManager& resources,
                                    const asset::MaterialAsset* material,
                                    std::string_view keyPrefix,
                                    const TerrainFallbackTextures& fallback,
                                    TerrainLayerGpu& layer)
{
    const auto snapshot = material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
    FillTerrainLayerTextureIndicesImpl(resources, snapshot ? &snapshot->Data() : nullptr, keyPrefix, fallback, layer);
}

namespace terrain_extraction_detail {

/// @brief 層配列を StructuredBuffer へ置く。層数が変わったら作り直し、内容が同じなら転送しない。
static void UploadLayerBuffer(TerrainGpuState& state, const std::vector<TerrainLayerGpu>& layers,
                              renderer::ResourceManager& resources)
{
    const uint32_t count = static_cast<uint32_t>(layers.size());
    const size_t bytes = layers.size() * sizeof(TerrainLayerGpu);
    if (!state.layerBuffer.IsValid() || state.layerCapacity != count) {
        resources.Release(state.layerBuffer);
        state.layerBuffer = resources.CreateStructuredBuffer(layers.data(), count,
                                                             static_cast<uint32_t>(sizeof(TerrainLayerGpu)));
        state.layerCapacity = count;
        state.uploadedLayers = layers;
        return;
    }
    if (state.uploadedLayers.size() == layers.size()
        && std::memcmp(state.uploadedLayers.data(), layers.data(), bytes) == 0)
        return;
    resources.Update(state.layerBuffer, layers.data(), bytes);
    state.uploadedLayers = layers;
}

TerrainLayerBinding PrepareTerrainLayers(TerrainRenderCache& cache, TerrainGpuState& gpu, TerrainComponent& terrain,
    renderer::ResourceManager& resources)
{
    /// @note 地形の dirty に依存せず現 authoring を毎回読む。索引付きの読込済み参照だけ handle を再利用する。
    /// @note bindless 添字も毎フレーム引く。差し替え・ホットリロードで変わるため (Material::Upload と同じ方針)。
    std::vector<TerrainLayerGpu> layers;
    layers.reserve(static_cast<size_t>((std::max)(terrain.LayerCount(), 1)));
    bool usesAutoBlend = false;
    for (const std::string& path : terrain.layerMaterials) {
        const auto materialAuthoringSnapshot = ReadTerrainMaterial(cache, path);
        const auto* material = materialAuthoringSnapshot ? &materialAuthoringSnapshot->Data() : nullptr;
        TerrainLayerGpu layer = ReadTerrainLayerParamsImpl(material, {});
        FillTerrainLayerTextureIndicesImpl(resources, material, {}, cache.fallback, layer);
        usesAutoBlend |= layer.autoBlendEnabled > 0.0f && layer.autoBlendStrength > 0.0f;
        layers.push_back(layer);
    }
    if (layers.empty()) {
        /// @note 層 0 枚の地形は白い既定層 1 枚で描く。シェーダーは layerCount >= 1 を前提にする。
        TerrainLayerGpu layer;
        FillTerrainLayerTextureIndices(resources, nullptr, {}, cache.fallback, layer);
        layers.push_back(layer);
    }
    UploadLayerBuffer(gpu, layers, resources);
    terrain.materialParamDirty = false;

    const renderer::IStructuredBuffer* layerBuffer = resources.Get(gpu.layerBuffer);
    const uint32_t layerBufferIndex = layerBuffer ? layerBuffer->GetBindlessIndex() : renderer::INVALID_BINDLESS_INDEX;
    return {static_cast<uint32_t>(layers.size()), layerBufferIndex, usesAutoBlend};
}

bool ApplyTerrainSurfaceMaterial(TerrainRenderCache& cache, const TerrainComponent& terrain, TerrainGpuState& gpu,
    renderer::RenderTerrainInput& surface, renderer::ResourceManager& resources)
{
    const auto materialAuthoringSnapshot = ReadTerrainMaterial(cache, terrain.surfaceMaterial);
    const auto* material = materialAuthoringSnapshot ? &materialAuthoringSnapshot->Data() : nullptr;
    auto layer = ReadTerrainLayerParamsImpl(material, {});
    layer.triplanar = 1.0f; layer.autoBlendEnabled = 0.0f;
    FillTerrainLayerTextureIndicesImpl(resources, material, {}, cache.fallback, layer);
    if (!gpu.surfaceLayerBuffer.IsValid())
        gpu.surfaceLayerBuffer = resources.CreateStructuredBuffer(&layer, 1, sizeof(layer));
    else if (!gpu.uploadedSurfaceLayer || std::memcmp(&*gpu.uploadedSurfaceLayer, &layer, sizeof(layer)) != 0)
        resources.Update(gpu.surfaceLayerBuffer, &layer, sizeof(layer));
    gpu.uploadedSurfaceLayer = layer;
    if (!gpu.surfaceSplatIndices.IsValid()) {
        const uint8_t indices[4]{}, weights[4]{255, 0, 0, 0};
        gpu.surfaceSplatIndices = resources.CreateTexture(indices, 1, 1);
        gpu.surfaceSplatWeights = resources.CreateTexture(weights, 1, 1);
    }
    const auto* buffer = resources.Get(gpu.surfaceLayerBuffer);
    if (!buffer || buffer->GetBindlessIndex() == renderer::INVALID_BINDLESS_INDEX) return false;
    surface.constants.layerBufferIndex = buffer->GetBindlessIndex();
    surface.constants.layerCount = 1;
    surface.constants.splatColumns = surface.constants.splatRows = 1;
    surface.constants.terrainParams.w = 0;
    surface.splatIndices = gpu.surfaceSplatIndices; surface.splatWeights = gpu.surfaceSplatWeights;
    return true;
}

}

}
