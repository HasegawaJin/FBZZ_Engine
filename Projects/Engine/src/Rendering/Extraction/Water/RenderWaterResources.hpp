/// @file    RenderWaterResources.hpp
/// @brief   水面のメッシュ・地形テクスチャ・波紋を所有する抽出用キャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Scene/References/Entity.hpp>
#include <Engine/Scene/Water/WaterInteraction.hpp>
#include <Engine/Scene/Water/Surface/WaterTerrainInfo.hpp>
#include <Graphics/Effects/Surfaces/RenderWaterInput.hpp>
#include <Graphics/Renderer/Materials/Material.hpp>
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::asset { struct MaterialAssetAuthoring; }
namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::scene::water_render_detail {

/// @note dirty フラグを経由しない設定変更でも、現在の描画入力と照合する。
struct WaterMeshGeometry {
    WaterSurfaceShape surfaceShape = WaterSurfaceShape::RECTANGLE;
    uint64_t splineSignature = 0;
    float splineDepthCap = 0;
    float extentX = 0, extentZ = 0;
    uint32_t resolutionX = 0, resolutionZ = 0, chunkCount = 0, patchResolution = 0;
    bool sharedPatchGrid = false;
    bool operator==(const WaterMeshGeometry&) const = default;
};

/// @note 正規化パッチは同じ生成元・リセット世代の複数の水面で共有する。
struct WaterPatchGrid {
    explicit WaterPatchGrid(const renderer::ResourceManager& resources);
    WaterPatchGrid(const WaterPatchGrid&) = delete;
    ~WaterPatchGrid();

    const uint64_t resourceOwner;
    const uint64_t resetVersion;
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, 16> indexBuffers{};
    std::array<uint32_t, 16> indexCounts{};
    std::array<renderer::WaterStripIndices, 16> strips{};
    bool stripsReady = false;
};

/// @note 2・4・8・16・32・64 分割。最後の水面が手放すまでグリッドは有効。
using WaterPatchGridSlots = std::array<std::weak_ptr<WaterPatchGrid>, 6>;

struct WaterMesh {
    std::vector<renderer::RenderWaterPatch> chunks; ///< 波の変位を含める前のローカル AABB を持つ。
    std::shared_ptr<WaterPatchGrid> patchGrid;
    std::shared_ptr<const std::vector<renderer::WaterSurfaceCluster>> surfaceClusters;
    renderer::ResourceHandle<renderer::BufferTag> surfaceClusterIndices;
    renderer::ResourceHandle<renderer::BufferTag> surfaceClusterVertices;
    bool surfaceClustersAttempted = false;
    bool stripsReady = false;
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
    WaterMeshGeometry geometry;
};

struct WaterDepthTextureSource {
    uint32_t width = 0, height = 0;
    std::shared_ptr<const std::vector<uint8_t>> rgba;
};

struct WaterTextures {
    renderer::ResourceHandle<renderer::TextureTag> terrainInfoTexture;
    math::Vector4 terrainInfoParams{};
    math::Vector4 terrainDomain{};
    uint64_t signature = 0;
    bool depthIsUniform = false;
    bool dynamicDepth = false;
    bool dynamicDepthUnavailable = false;
    WaterDepthTextureSource uploadedDepth;
    WaterTerrainInfoAtlasCache terrainAtlas;
};

struct WaterTextureRequest {
    WaterTerrainBakeSettings bakeSettings;
    uint64_t signature = 0;
    bool canBakeTerrain = false;
};

/// @note 輪の寿命と位置は WaterComponent が持つ。描画ビュー間で共有するのは焼いたテクスチャだけ。
struct WaterRippleState {
    std::vector<uint8_t> bakedPixels;
    uint32_t width = kWaterRippleTextureSize;
    uint32_t height = kWaterRippleTextureSize;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    bool dirty = true;
    bool previousBakeHadRipples = false;
    uint64_t inputSignature = 0;
    bool signatureValid = false;
};

struct WaterCacheKey {
    uint64_t sceneGeneration = 0;
    uint32_t entityIndex = 0;
    uint32_t entityGeneration = 0;
    bool operator==(const WaterCacheKey&) const = default;
};

struct WaterCacheKeyHash {
    size_t operator()(const WaterCacheKey& key) const
    {
        return std::hash<uint64_t>{}(key.sceneGeneration)
            ^ std::hash<uint64_t>{}((static_cast<uint64_t>(key.entityGeneration) << 32u) | key.entityIndex);
    }
};

class WaterRenderCache {
public:
    explicit WaterRenderCache(const renderer::ResourceManager& resources);
    WaterRenderCache(const WaterRenderCache&) = delete;
    WaterRenderCache& operator=(const WaterRenderCache&) = delete;
    ~WaterRenderCache();

    static WaterRenderCache& Acquire(Scene& scene, renderer::ResourceManager& resources);
    static void Invalidate(const WaterCacheKey& key, bool meshDirty, bool textureDirty);
    static void ReleaseScene(Scene& scene);
    static void ReleaseEntity(Scene& scene, EntityID entity);

    void ReleaseUnavailableBodies(Scene& scene, renderer::ResourceManager& resources);
    [[nodiscard]] bool CollectTerrainInputs(Scene& scene, WaterTerrainInfoInputs& terrainInputs);
    void PrepareMesh(const WaterCacheKey& key, WaterComponent& water, renderer::ResourceManager& resources,
        bool buildSurfaceClusters);
    void PrepareTerrainTextures(const WaterCacheKey& key, WaterComponent& water, const Transform& transform,
        const WaterTerrainInfoInputs& terrainInputs, const WaterTextureRequest& request,
        renderer::IRenderer& renderer, renderer::ResourceManager& resources);
    const WaterRippleState& PrepareRipples(const WaterCacheKey& key, const WaterComponent& water,
        const Transform& transform, renderer::ResourceManager& resources);

    WaterMesh& MeshFor(const WaterCacheKey& key) { return m_meshes.at(key); }
    const WaterTextures& TerrainTexturesFor(const WaterCacheKey& key) const { return m_textures.at(key); }
    renderer::Material& AppearanceFor(const WaterCacheKey& key) { return m_appearances[key]; }

private:
    [[nodiscard]] renderer::ResourceManager* FindLiveOwner() const;
    template<class Erase>
    void ReleaseEntries(renderer::ResourceManager& resources, Erase erase);

    const uint64_t m_resourceOwner;
    const uint64_t m_resetVersion;
    std::unordered_map<WaterCacheKey, WaterMesh, WaterCacheKeyHash> m_meshes;
    WaterPatchGridSlots m_patchGrids;
    std::unordered_map<WaterCacheKey, WaterTextures, WaterCacheKeyHash> m_textures;
    std::unordered_map<WaterCacheKey, renderer::Material, WaterCacheKeyHash> m_appearances;
    std::unordered_map<WaterCacheKey, WaterRippleState, WaterCacheKeyHash> m_ripples;
    std::unordered_map<uint64_t, WaterTerrainInfoCache> m_terrainInfo;
    std::unordered_set<WaterCacheKey, WaterCacheKeyHash> m_dirtyMeshes;
    std::unordered_set<WaterCacheKey, WaterCacheKeyHash> m_dirtyTextures;
};

void NormalizeWaterMeshSettings(WaterComponent& water, bool splineSurface);
[[nodiscard]] WaterMeshGeometry ResolveWaterMeshGeometry(const WaterComponent& water);
[[nodiscard]] renderer::WaterWaveMargin ComputeWaveMargin(const WaterComponent& water, bool horizonSurface);
void ReleaseWaterMesh(WaterMesh& mesh, renderer::ResourceManager& resources);
void BuildWaterMesh(const WaterComponent& water, WaterMesh& mesh, WaterPatchGridSlots& patchGridSlots,
    renderer::ResourceManager& resources, bool buildSurfaceClusters);
void PopulateWaterGeometry(renderer::RenderWaterInput& input, const WaterComponent& water,
    const WaterMesh& mesh, bool splineSurface, bool horizonSurface, bool buildSurfaceClusters);
void PrepareWaterStripBuffers(const WaterComponent& water, WaterMesh& mesh, renderer::ResourceManager& resources);

[[nodiscard]] WaterTextureRequest ResolveWaterTextureRequest(const WaterComponent& water, const Transform& transform,
    const WaterTerrainInfoInputs& terrainInputs, bool validTerrainInputs, const asset::MaterialAssetAuthoring* material);
void UpdateWaterTextures(WaterTextures& textures, const WaterComponent& water, const Transform& transform,
    const WaterTerrainInfoInputs& terrainInputs, const WaterTextureRequest& request, bool replaceTexture,
    renderer::IRenderer& renderer, renderer::ResourceManager& resources);
void UpdateRippleState(WaterRippleState& state, const WaterComponent& water,
    const Transform& transform, renderer::ResourceManager& resources);

} // namespace fbzz::scene::water_render_detail
