/// @file    RenderWaterExtractor.cpp
/// @brief   WaterComponent → GPU 水面メッシュ・泡マスク・波紋テクスチャ生成と描画 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
/// @note 水面は透明描画・Terrain 高さ参照・動的 CPU テクスチャ更新を扱う。GPU リソースは
/// @note Component に持たせず System 側の static cache に閉じ、Scene データは保存しやすい
/// @note 純粋なパラメータのまま保つ。
#include "Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterTerrainInfo.hpp>
#include <Engine/Scene/WaterDepthField.hpp>
#include <Engine/Scene/WaterExclusion.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Core/Profiler/ProfileScope.hpp>

#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include <Engine/Asset/WaterOpticalSettings.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include "Engine/Asset/VelocityFieldAtlas.hpp"
#include "Fluid/VectorFieldAsset.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/WaterComponent.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {

/// @note Captured once when the native module loads; only exact "tiled" opts in. Missing, "full" and unknown values retain the full scan.
const bool USE_TILED_RIPPLE_BAKE = [] {
    const char* mode = std::getenv("FBZZ_WATER_RIPPLE_BAKE_MODE");
    return mode && std::string_view(mode) == "tiled";
}();

struct WaterCandidatePool {
    std::vector<std::unique_ptr<std::vector<EntityID>>> entries;
    size_t depth = 0;
};

WaterCandidatePool& WaterCandidates()
{
    thread_local WaterCandidatePool pool;
    return pool;
}

/// @note Snapshot the current Component order; nested extraction cannot replace the outer candidate storage.
class WaterCandidateLease {
public:
    explicit WaterCandidateLease(Scene& scene)
    {
        auto& pool = WaterCandidates();
        if (pool.depth == pool.entries.size())
            pool.entries.push_back(std::make_unique<std::vector<EntityID>>());
        m_entities = pool.entries[pool.depth++].get();
        const auto source = scene.GetEntities<WaterComponent>();
        m_entities->assign(source.begin(), source.end());
    }
    ~WaterCandidateLease() { m_entities->clear(); --WaterCandidates().depth; }
    WaterCandidateLease(const WaterCandidateLease&) = delete;
    WaterCandidateLease& operator=(const WaterCandidateLease&) = delete;
    std::span<const EntityID> Entities() const { return *m_entities; }
private:
    std::vector<EntityID>* m_entities = nullptr;
};

/// @note WaterVertex / WaterCB / WaterEffectParams は `RenderPassContext.hpp` にある。
/// @note エディタのマテリアルプレビューが本編と同じレイアウトを焼く必要があり、ここに複製すると黙ってずれる。

/// @brief チャンク 1 個分の GPU リソースと、波を乗せる前のローカル AABB。
/// @note 波の振幅は meshDirty を立てずに変わるため AABB へ焼き込まない。焼き込むと振幅を
/// @note 上げた瞬間から AABB だけ古くなり、見えている端のチャンクが消える。
using WaterChunk = renderer::RenderWaterPatch;

/// @note dirty フラグを経由しない設定変更でも、描画入力と異なるメッシュを再利用しない。
struct WaterMeshGeometry {
    WaterSurfaceShape surfaceShape = WaterSurfaceShape::RECTANGLE;
    uint64_t splineSignature = 0;
    float splineDepthCap = 0;
    float extentX = 0, extentZ = 0;
    uint32_t resolutionX = 0, resolutionZ = 0, chunkCount = 0, patchResolution = 0;
    bool sharedPatchGrid = false;
    bool operator==(const WaterMeshGeometry&) const = default;
};

WaterMeshGeometry ResolveWaterMeshGeometry(const WaterComponent& water)
{
    WaterMeshGeometry result;
    result.surfaceShape = WaterSurfaceEffectiveShape(water, WaterSurfaceReadView::SIMULATION);
    if (UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION)) {
        const auto* geometry = WaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
        result.splineSignature = geometry ? geometry->signature : 0;
        result.splineDepthCap = (std::max)(water.buoyancyDepth, 0.0f);
        return result;
    }
    result.extentX = water.extentX;
    result.extentZ = water.extentZ;
    result.sharedPatchGrid = water.adaptivePatchLod || water.farOceanEnabled;
    if (result.sharedPatchGrid) result.patchResolution = water.lodPatchResolution;
    else {
        result.resolutionX = water.resolutionX;
        result.resolutionZ = water.resolutionZ;
        result.chunkCount = water.chunkCount;
    }
    return result;
}

struct WaterMesh {
    std::vector<WaterChunk> chunks;
    renderer::ResourceHandle<renderer::BufferTag> patchVertexBuffer;
    std::array<renderer::ResourceHandle<renderer::BufferTag>, 16> patchIndexBuffers{};
    std::array<uint32_t, 16> patchIndexCounts{};
    math::Vector3 aabbMin;
    math::Vector3 aabbMax;
    WaterMeshGeometry geometry;
};

/// @note 頂点が Gerstner 変位で平面から出る量。水平は Q*A、垂直は A の総和が上限。
using WaveMargin = renderer::WaterWaveMargin;

WaveMargin ComputeWaveMargin(const WaterComponent& water)
{
    WaveMargin margin;
    if (water.enableGerstnerWaves) {
        /// @note 群の包絡は最大 (1 + waveGrouping) 倍、方向広がりは主 + 伴走の和まで振幅を持ち上げる。
        /// @note ここに入れ忘れると、山に当たったチャンクだけが «AABB からはみ出した» 扱いで消える。
        const float peak = (1.0f + math::Clamp01(water.waveGrouping))
                         * WaterComponent::WaveSpreadAmplitudeSum(water.waveSpread);
        for (size_t i = 0; i < (std::min)(static_cast<size_t>(water.waveCount), water.waves.size()); ++i) {
            const GerstnerWave& wave = water.waves[i];
            const float amplitude = (std::max)(wave.amplitude, 0.0f) * peak;
            margin.vertical   += amplitude;
            margin.horizontal += amplitude * math::Clamp01(wave.steepness);
        }
    }
    /// @note 流れの場が作る形と着水の輪も «平面から出る量»。入れないと穴や山や輪の上のチャンク
    /// @note だけが «AABB からはみ出した» 扱いで消える。盛り上がりも穴と同じ枠で測る。
    float flowDisplacement = 0.0f;
    for (int i = 0; i < (std::min)(water.surfaceFlowCount, kWaterSurfaceFlowCount); ++i) {
        flowDisplacement = (std::max)(
            flowDisplacement, std::abs(water.surfaceFlows[static_cast<size_t>(i)].height));
    }
    /// @note 輪の合成断面の上限は本体 + 後続 2 本 = 1.58 倍。
    margin.vertical += flowDisplacement
                     + (water.ripples.empty() ? 0.0f : kWaterRippleHeightAmplitude * 1.6f);

    /// @note さざ波・法線ゆらぎのぶんだけ余裕を持たせる。
    margin.horizontal += 0.5f;
    margin.vertical   += 0.5f;
    return margin;
}

/// @brief 水面が使うテクスチャはエンジンが生成する泡マスクだけ。
/// @note 法線・泡・環境反射は手続き / 空連動 IBL から取るため、オーサリング済みテクスチャ資産は不要。
struct WaterTextures {
    renderer::ResourceHandle<renderer::TextureTag> foamMask;
    math::Vector4 terrainInfoParams{};
    uint64_t signature = 0;
    bool depthIsUniform = false;
};

/// @note 焼いた 1 枚と «前のフレームに輪があったか»。
/// @note 輪そのものは WaterComponent::ripples が正本。ここに置くと SceneView と GameView で
/// @note 寿命が 2 回進み、CPU 側の GetSurfaceHeightAt からも読めない。
struct WaterRippleState {
    std::vector<uint8_t> pixels;
    uint32_t width = kWaterRippleTextureSize;
    uint32_t height = kWaterRippleTextureSize;
    renderer::ResourceHandle<renderer::TextureTag> gpuTex;
    bool dirty = true;
    /// @note 最後に焼いたとき輪があったか。落としきった次のフレームに 1 回だけ焼き直すため。
    bool hadRipples = false;
    uint64_t inputSignature = 0;
    bool signatureValid = false;
};

struct WaterCacheKey {
    uint64_t sceneGeneration = 0;
    uint32_t index = 0;
    uint32_t generation = 0;
    bool operator==(const WaterCacheKey&) const = default;
};
struct WaterCacheKeyHash {
    size_t operator()(const WaterCacheKey& key) const {
        return std::hash<uint64_t>{}(key.sceneGeneration)
            ^ std::hash<uint64_t>{}((static_cast<uint64_t>(key.generation) << 32u) | key.index);
    }
};
static std::unordered_map<WaterCacheKey, WaterMesh, WaterCacheKeyHash> s_meshCache;
static std::unordered_map<WaterCacheKey, WaterTextures, WaterCacheKeyHash> s_texCache;
/// @note Appearance owns b2 independently of generated foam/depth textures and follows the same scene/resource lifetime.
static std::unordered_map<WaterCacheKey, renderer::Material, WaterCacheKeyHash> s_appearanceCache;
static std::unordered_map<WaterCacheKey, WaterRippleState, WaterCacheKeyHash> s_rippleStates;
static uint64_t s_waterResourceOwner = 0;
static uint64_t s_waterResourceVersion = UINT64_MAX;

struct TerrainInfoState {
    WaterTerrainInfoCache cache;
};
static std::unordered_map<uint64_t, TerrainInfoState> s_terrainInfoStates;

void ReleaseWaterMesh(WaterMesh& mesh, renderer::ResourceManager& resources)
{
    for (auto& chunk : mesh.chunks) {
        if (chunk.vertexBuffer.IsValid()) resources.Release(chunk.vertexBuffer);
        if (chunk.indexBuffer.IsValid()) resources.Release(chunk.indexBuffer);
    }
    if (mesh.patchVertexBuffer.IsValid()) resources.Release(mesh.patchVertexBuffer);
    for (auto& indexBuffer : mesh.patchIndexBuffers)
        if (indexBuffer.IsValid()) resources.Release(indexBuffer);
    mesh = {};
}

void ReleaseWaterCacheEntries(uint64_t sceneGeneration, const EntityID* entity, renderer::ResourceManager& resources)
{
    if (s_waterResourceOwner != resources.GetInstanceId() || s_waterResourceVersion != resources.GetResetVersion()) return;
    const auto matches = [sceneGeneration, entity](const WaterCacheKey& key) {
        return key.sceneGeneration == sceneGeneration
            && (!entity || (key.index == entity->index && key.generation == entity->generation));
    };
    std::erase_if(s_meshCache, [&](auto& entry) {
        if (!matches(entry.first)) return false;
        ReleaseWaterMesh(entry.second, resources);
        return true;
    });
    std::erase_if(s_appearanceCache, [&](auto& entry) {
        if (!matches(entry.first)) return false;
        if (entry.second.paramsBuffer.IsValid()) resources.Release(entry.second.paramsBuffer);
        entry.second.paramsBuffer = {};
        return true;
    });
    std::erase_if(s_texCache, [&](auto& entry) {
        if (!matches(entry.first)) return false;
        if (entry.second.foamMask.IsValid()) resources.Release(entry.second.foamMask);
        return true;
    });
    std::erase_if(s_rippleStates, [&](auto& entry) {
        if (!matches(entry.first)) return false;
        if (entry.second.gpuTex.IsValid()) resources.Release(entry.second.gpuTex);
        return true;
    });
}


void BuildWaterMesh(const WaterComponent& water, WaterMesh& mesh, renderer::ResourceManager& resources)
{
    assert(water.resolutionX > 0);
    assert(water.resolutionZ > 0);

    ReleaseWaterMesh(mesh, resources);
    mesh.geometry = ResolveWaterMeshGeometry(water);

    if (UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION)) {
        const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
        if (!geometry) return;
        const auto& snapshot = *geometry;
        mesh.aabbMin = snapshot.localBoundsMin;
        mesh.aabbMax = snapshot.localBoundsMax;
        const math::Vector2 extent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
        if (snapshot.vertices.empty() || snapshot.triangles.empty()
            || !std::isfinite(extent.x) || !std::isfinite(extent.y)
            || extent.x <= 0.0f || extent.y <= 0.0f) return;

        std::vector<WaterVertex> vertices;
        vertices.reserve(snapshot.vertices.size());
        const float depthCap = (std::max)(water.buoyancyDepth, 0.0f);
        for (const WaterSplineVertex& source : snapshot.vertices) {
            WaterVertex vertex{};
            vertex.position = source.localPosition;
            vertex.uv = {source.localPosition.x / extent.x + 0.5f,
                source.localPosition.z / extent.y + 0.5f};
            vertex.splineData = {(std::min)(source.depth, depthCap),
                source.worldVelocity.x, source.worldVelocity.z, 1.0f};
            vertices.push_back(vertex);
        }
        std::vector<uint32_t> indices;
        indices.reserve(snapshot.triangles.size() * 3u);
        for (const WaterSplineTriangle& triangle : snapshot.triangles)
            indices.insert(indices.end(), triangle.indices.begin(), triangle.indices.end());

        WaterChunk chunk;
        chunk.vertexBuffer = resources.CreateVertexBuffer(vertices.data(),
            vertices.size() * sizeof(WaterVertex), static_cast<uint32_t>(sizeof(WaterVertex)));
        chunk.indexBuffer = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        chunk.indexCount = static_cast<uint32_t>(indices.size());
        chunk.aabbMin = snapshot.localBoundsMin;
        chunk.aabbMax = snapshot.localBoundsMax;
        mesh.chunks.push_back(std::move(chunk));
        return;
    }

    const float halfX = water.extentX * 0.5f;
    const float halfZ = water.extentZ * 0.5f;
    mesh.aabbMin = { -halfX, 0.0f, -halfZ };
    mesh.aabbMax = { halfX, 0.0f, halfZ };
    if (water.adaptivePatchLod || water.farOceanEnabled) {
        const uint32_t cells = water.lodPatchResolution;
        std::vector<WaterVertex> vertices;
        vertices.reserve(static_cast<size_t>(cells + 1u) * (cells + 1u));
        for (uint32_t z = 0; z <= cells; ++z) {
            for (uint32_t x = 0; x <= cells; ++x) {
                const math::Vector2 uv{static_cast<float>(x) / cells, static_cast<float>(z) / cells};
                vertices.push_back({{uv.x, 0.0f, uv.y}, uv});
            }
        }
        mesh.patchVertexBuffer = resources.CreateVertexBuffer(vertices.data(),
            vertices.size() * sizeof(WaterVertex), static_cast<uint32_t>(sizeof(WaterVertex)));
        for (uint32_t mask = 0; mask < mesh.patchIndexBuffers.size(); ++mask) {
            std::vector<uint32_t> indices;
            if (!renderer::BuildSurfacePatchIndices(cells, mask, indices)) continue;
            mesh.patchIndexBuffers[mask] = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
            mesh.patchIndexCounts[mask] = static_cast<uint32_t>(indices.size());
        }
        return;
    }

    const float dx = water.extentX / static_cast<float>(water.resolutionX);
    const float dz = water.extentZ / static_cast<float>(water.resolutionZ);
    const float ox = -water.extentX * 0.5f;
    const float oz = -water.extentZ * 0.5f;

    /// @note 切り上げでなく境界を按分して分ける。ceil(res/chunks) を全チャンクに掛けると
    /// @note 最後のほうのチャンクは開始セルが res を追い越し、差が符号なしで折り返して
    /// @note 4G 要素の reserve になり、解像度を上げた瞬間に確保失敗で落ちていた。
    const uint32_t numChunks =
        (std::min)((std::max)(water.chunkCount, 1u), (std::min)(water.resolutionX, water.resolutionZ));
    auto splitAt = [](uint32_t cells, uint32_t index, uint32_t count) {
        return static_cast<uint32_t>((static_cast<uint64_t>(cells) * index) / count);
    };

    for (uint32_t cz = 0; cz < numChunks; ++cz) {
        for (uint32_t cx = 0; cx < numChunks; ++cx) {
            const uint32_t ixStart = splitAt(water.resolutionX, cx,     numChunks);
            const uint32_t ixEnd   = splitAt(water.resolutionX, cx + 1, numChunks);
            const uint32_t izStart = splitAt(water.resolutionZ, cz,     numChunks);
            const uint32_t izEnd   = splitAt(water.resolutionZ, cz + 1, numChunks);
            if (ixEnd <= ixStart || izEnd <= izStart) continue;

            const uint32_t vertCols = ixEnd - ixStart + 1;
            const uint32_t vertRows = izEnd - izStart + 1;

            std::vector<WaterVertex> verts;
            verts.reserve(static_cast<size_t>(vertCols) * static_cast<size_t>(vertRows));
            for (uint32_t iz = izStart; iz <= izEnd; ++iz) {
                for (uint32_t ix = ixStart; ix <= ixEnd; ++ix) {
                    WaterVertex v;
                    v.position = { ox + static_cast<float>(ix) * dx, 0.0f, oz + static_cast<float>(iz) * dz };
                    v.uv = {
                        static_cast<float>(ix) / static_cast<float>(water.resolutionX),
                        static_cast<float>(iz) / static_cast<float>(water.resolutionZ)
                    };
                    verts.push_back(v);
                }
            }

            std::vector<uint32_t> indices;
            indices.reserve(static_cast<size_t>(vertCols - 1) * static_cast<size_t>(vertRows - 1) * 6u);
            for (uint32_t iz = 0; iz < vertRows - 1; ++iz) {
                for (uint32_t ix = 0; ix < vertCols - 1; ++ix) {
                    const uint32_t i00 = iz * vertCols + ix;
                    const uint32_t i10 = i00 + 1;
                    const uint32_t i01 = i00 + vertCols;
                    const uint32_t i11 = i01 + 1;
                    indices.insert(indices.end(), { i00, i01, i10, i10, i01, i11 });
                }
            }

            WaterChunk chunk;
            chunk.vertexBuffer = resources.CreateVertexBuffer(
                verts.data(), verts.size() * sizeof(WaterVertex), static_cast<uint32_t>(sizeof(WaterVertex)));
            chunk.indexBuffer = resources.CreateIndexBuffer(
                indices.data(), static_cast<uint32_t>(indices.size()));
            chunk.indexCount = static_cast<uint32_t>(indices.size());
            chunk.aabbMin = { ox + static_cast<float>(ixStart) * dx, 0.0f, oz + static_cast<float>(izStart) * dz };
            chunk.aabbMax = { ox + static_cast<float>(ixEnd)   * dx, 0.0f, oz + static_cast<float>(izEnd)   * dz };
            mesh.chunks.push_back(std::move(chunk));
        }
    }

    mesh.aabbMin = { ox, 0.0f, oz };
    mesh.aabbMax = { -ox, 0.0f, -oz };
}


inline float WGetF(const asset::MaterialAssetAuthoring* m, const char* name, float def)
{
    if (!m) return def;
    auto it = m->params.find(name);
    if (it != m->params.end() && !it->second.empty() && std::isfinite(it->second[0])) return it->second[0];
    return def;
}
inline math::Vector2 WGetF2(const asset::MaterialAssetAuthoring* m, const char* name, math::Vector2 def)
{
    if (!m) return def;
    auto it = m->params.find(name);
    if (it != m->params.end() && it->second.size() >= 2
        && std::isfinite(it->second[0]) && std::isfinite(it->second[1]))
        return { it->second[0], it->second[1] };
    return def;
}
inline math::Vector3 WGetF3(const asset::MaterialAssetAuthoring* m, const char* name, math::Vector3 def)
{
    if (!m) return def;
    auto it = m->params.find(name);
    if (it != m->params.end() && it->second.size() >= 3
        && std::isfinite(it->second[0]) && std::isfinite(it->second[1]) && std::isfinite(it->second[2]))
        return { it->second[0], it->second[1], it->second[2] };
    return def;
}

/// @note Uniform encoded channels remain uniform under bilinear filtering; a no-bottom atlas preserves the authored constant depth.
bool HasUniformPhysicalDepth(const std::vector<uint8_t>& rgba, float depthRange, float depthLimit)
{
    if (rgba.empty() || rgba.size() % 4u != 0u) return false;
    const uint8_t flags = rgba[3u];
    if ((flags & 2u) != 0u) return false;
    const bool hasBottom = (flags & 1u) != 0u;
    for (size_t pixel = 0; pixel < rgba.size(); pixel += 4u) {
        if (rgba[pixel + 3u] != flags) return false;
        if (hasBottom && (rgba[pixel + 1u] != rgba[1u] || rgba[pixel + 2u] != rgba[2u])) return false;
    }
    if (!hasBottom) return true;
    float depth = 0.0f;
    return TryDecodeWaterTerrainDepth(rgba[1u], rgba[2u], depthRange, depth) && depth >= depthLimit;
}

WaterTextures BuildTextureSet(
    const WaterComponent& water,
    const Transform& waterTransform,
    const WaterTerrainInfoInputs& inputs,
    bool validInputs,
    renderer::ResourceManager& resources,
    const WaterTerrainBakeSettings& settings,
    uint64_t signature)
{
    WaterTextures textures;
    textures.signature = signature;
    const WaterDepthField* depthField = water.EffectiveDepthField(WaterSurfaceReadView::SIMULATION);
    const float attenuationDepth = water.EffectiveWaveAttenuationDepth(WaterSurfaceReadView::SIMULATION);
    if (depthField && attenuationDepth > 0.0f
        && depthField->attenuationDepth == attenuationDepth) {
        const auto& field = *depthField;
        textures.depthIsUniform = HasUniformPhysicalDepth(field.rgba, field.depthRange, water.buoyancyDepth);
        textures.foamMask = resources.CreateTexture(field.rgba.data(), field.width, field.height);
        if (textures.foamMask.IsValid())
            textures.terrainInfoParams = {1.0f, field.depthRange,
                static_cast<float>(field.width), static_cast<float>(field.height)};
        return textures;
    }
    WaterTerrainInfoAtlas atlas;
    if (validInputs && TryBakeWaterTerrainInfo(inputs, water, waterTransform, settings, atlas,
            WaterSurfaceReadView::SIMULATION)) {
        textures.depthIsUniform = HasUniformPhysicalDepth(atlas.rgba, atlas.depthRange, water.buoyancyDepth);
        textures.foamMask = resources.CreateTexture(atlas.rgba.data(), atlas.width, atlas.height);
        if (textures.foamMask.IsValid())
            textures.terrainInfoParams = {1.0f, atlas.depthRange,
                static_cast<float>(atlas.width), static_cast<float>(atlas.height)};
    } else {
        const std::array<uint8_t, 4> empty{};
        textures.foamMask = resources.CreateTexture(empty.data(), 1, 1);
    }
    return textures;
}

/// @note 波紋テクスチャを焼く。RG = 法線 xy、**B = 頂点へ乗せる高さ** [m] (kWaterRippleHeightScale 正規化)。
/// @note 高さにだけ輪ごとの meshFade を掛ける。細い輪は頂点で刻めないので «傾きだけ» の帯へ落ち、
/// @note 法線は今までどおり全部書く (water-waves.md の帯の原則)。
/// @note テクセルをワールド XZ へ直して距離を測る。UV 距離で測ると、非正方形の水面で輪が
/// @note 楕円になり CPU の GetSurfaceHeightAt と食い違う。
/// @warning 断面は WaterComponent::WaterRippleWave が正本。ここに写しを作らないこと。
void UpdateRippleState(WaterRippleState& state, const WaterComponent& water,
                       const Transform& transform, renderer::ResourceManager& resources)
{
    const auto waterWorld = transform.GetWorldMatrix();
    const auto extent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
    const float invHeightScale = 1.0f / kWaterRippleHeightScale;
    uint64_t signature = 1469598103934665603ull;
    const auto mix = [&signature](float value) {
        signature ^= std::bit_cast<uint32_t>(value);
        signature *= 1099511628211ull;
    };
    for (const auto& row : waterWorld.m) for (float value : row) mix(value);
    mix(extent.x); mix(extent.y);
    signature ^= water.ripples.size(); signature *= 1099511628211ull;
    for (const auto& ripple : water.ripples) {
        mix(ripple.center.x); mix(ripple.center.y); mix(ripple.radius);
        mix(ripple.width); mix(ripple.amplitude); mix(ripple.meshFade);
    }
    /// @note All views share world-space ripple data; identical inputs reuse the same baked texture.
    if (state.signatureValid && state.inputSignature == signature && resources.Get(state.gpuTex)) {
        state.dirty = false;
        return;
    }

    {
        FBZZ_PROFILE_SCOPE("WaterRipple::Bake");
        struct RippleBounds {
            const WaterRipple* ripple = nullptr;
            float innerSquared = 0.0f;
            float outerSquared = 0.0f;
            bool bounded = false;
        };
        std::vector<RippleBounds> activeRipples;
        activeRipples.reserve(water.ripples.size());
        for (const auto& ripple : water.ripples) {
            if (ripple.amplitude <= 0.0f) continue;
            const float width = (std::max)(ripple.width, 1.0e-4f);
            const float inner = (std::max)(0.0f, ripple.radius - width * 6.0f - width * 16.0f);
            const float outer = (std::max)(0.0f, ripple.radius + width * 16.0f);
            const float innerSquared = inner * inner;
            const float outerSquared = outer * outer;
            const bool bounded = std::numeric_limits<float>::is_iec559
                && std::numeric_limits<float>::digits == 24 && std::isfinite(innerSquared)
                && std::isfinite(outerSquared) && std::isfinite(ripple.radius) && std::isfinite(ripple.width);
            activeRipples.push_back({&ripple, innerSquared, outerSquared, bounded});
        }
        const auto worldAt = [&](uint32_t x, uint32_t y) {
            const math::Vector2 uv = {
                (static_cast<float>(x) + 0.5f) / static_cast<float>(state.width),
                (static_cast<float>(y) + 0.5f) / static_cast<float>(state.height)
            };
            /// @note Keep the canonical texel-center mapping and operation order used by EmitWaterRipple.
            const auto point = waterWorld * math::Vector4{(uv.x - 0.5f) * extent.x,
                0.0f, (uv.y - 0.5f) * extent.y, 1.0f};
            return math::Vector2{point.x, point.z};
        };
        const auto bakeRegion = [&](uint32_t firstX, uint32_t firstY, uint32_t endX, uint32_t endY,
            std::span<const RippleBounds> candidates) {
            for (uint32_t y = firstY; y < endY; ++y) {
                for (uint32_t x = firstX; x < endX; ++x) {
                    const auto world = worldAt(x, y);
                    math::Vector2 offset = math::Vector2::ZERO;
                    float height = 0.0f;
                    /// @note Both traversal modes preserve the ripple order and the existing binary32 zero-tail test.
                    for (const auto& bounds : candidates) {
                        const auto& ripple = *bounds.ripple;
                        const math::Vector2 d = world - ripple.center;
                        const float distanceSquared = d.LengthSq();
                        /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/data-type-constants IEEE float range; WaterComponent::WaterRippleWave remains authoritative.
                        if (bounds.bounded && std::isfinite(distanceSquared)
                            && (distanceSquared < bounds.innerSquared || distanceSquared > bounds.outerSquared)) continue;
                        const float dist = std::sqrt(distanceSquared);
                        if (dist <= math::EPSILON) continue;

                        const float wave = WaterComponent::WaterRippleWave(ripple, dist) * ripple.amplitude;
                        offset += (d / dist) * wave;
                        height += wave * ripple.meshFade * kWaterRippleHeightAmplitude;
                    }

                    const size_t p = (static_cast<size_t>(y) * state.width + x) * 4u;
                    state.pixels[p + 0] = static_cast<uint8_t>(math::Clamp01(0.5f + offset.x * 0.5f) * 255.0f);
                    state.pixels[p + 1] = static_cast<uint8_t>(math::Clamp01(0.5f + offset.y * 0.5f) * 255.0f);
                    state.pixels[p + 2] =
                        static_cast<uint8_t>(math::Clamp01(0.5f + height * invHeightScale * 0.5f) * 255.0f);
                    state.pixels[p + 3] = 255u;
                }
            }
        };
        if (!USE_TILED_RIPPLE_BAKE) {
            state.pixels.assign(static_cast<size_t>(state.width) * static_cast<size_t>(state.height) * 4u, 128u);
            bakeRegion(0u, 0u, state.width, state.height, activeRipples);
        } else {
            const auto overlapsTile = [](const RippleBounds& bounds,
                const math::Vector2& low, const math::Vector2& high) {
                if (!bounds.bounded || !std::isfinite(low.x) || !std::isfinite(low.y)
                    || !std::isfinite(high.x) || !std::isfinite(high.y)) return true;
                const math::Vector2 a = low - bounds.ripple->center;
                const math::Vector2 b = high - bounds.ripple->center;
                const math::Vector2 nearest{
                    a.x > 0.0f ? a.x : (b.x < 0.0f ? b.x : 0.0f),
                    a.y > 0.0f ? a.y : (b.y < 0.0f ? b.y : 0.0f)};
                const math::Vector2 farthest{
                    (std::max)(std::abs(a.x), std::abs(b.x)),
                    (std::max)(std::abs(a.y), std::abs(b.y))};
                const float nearSquared = std::nextafter(nearest.LengthSq(), 0.0f);
                const float farSquared = std::nextafter(farthest.LengthSq(), std::numeric_limits<float>::infinity());
                /// @note Unknown or overflowing distances keep the original per-texel path.
                return !std::isfinite(nearSquared) || !std::isfinite(farSquared)
                    || (farSquared >= bounds.innerSquared && nearSquared <= bounds.outerSquared);
            };
            /// @note Zero contributions encode as 127 after truncation, including the empty frame after the last ripple.
            state.pixels.assign(static_cast<size_t>(state.width) * static_cast<size_t>(state.height) * 4u, 127u);
            for (size_t alpha = 3; alpha < state.pixels.size(); alpha += 4u) state.pixels[alpha] = 255u;
            constexpr uint32_t RIPPLE_TILE_SIZE = 16u;
            const auto roundingMargin = [&](size_t axis) {
                const double magnitude = std::abs(static_cast<double>(waterWorld.m[axis][0])) * std::abs(extent.x) * 0.5
                    + std::abs(static_cast<double>(waterWorld.m[axis][2])) * std::abs(extent.y) * 0.5
                    + std::abs(static_cast<double>(waterWorld.m[axis][3]));
                const double error = (magnitude + 1.0) * 32.0 * std::numeric_limits<float>::epsilon();
                if (!std::isfinite(error) || error > std::numeric_limits<float>::max())
                    return std::numeric_limits<float>::infinity();
                return std::nextafter(static_cast<float>(error), std::numeric_limits<float>::infinity());
            };
            /// @note Account for texel mapping and affine-operation rounding using absolute terms, including large translation/cancellation; invalid bounds retain every ripple.
            const math::Vector2 roundingError{roundingMargin(0u), roundingMargin(2u)};
            std::vector<RippleBounds> tileRipples;
            tileRipples.reserve(activeRipples.size());
            for (uint32_t tileY = 0; tileY < state.height; tileY += RIPPLE_TILE_SIZE) {
                const uint32_t endY = (std::min)(tileY + RIPPLE_TILE_SIZE, state.height);
                for (uint32_t tileX = 0; tileX < state.width; tileX += RIPPLE_TILE_SIZE) {
                    const uint32_t endX = (std::min)(tileX + RIPPLE_TILE_SIZE, state.width);
                    const std::array corners{worldAt(tileX, tileY), worldAt(endX - 1u, tileY),
                        worldAt(tileX, endY - 1u), worldAt(endX - 1u, endY - 1u)};
                    math::Vector2 low = corners.front(), high = corners.front();
                    bool finiteCorners = true;
                    for (const auto& corner : corners) {
                        finiteCorners = finiteCorners && std::isfinite(corner.x) && std::isfinite(corner.y);
                        low.x = (std::min)(low.x, corner.x); low.y = (std::min)(low.y, corner.y);
                        high.x = (std::max)(high.x, corner.x); high.y = (std::max)(high.y, corner.y);
                    }
                    low.x = std::nextafter(low.x - roundingError.x, -std::numeric_limits<float>::infinity());
                    low.y = std::nextafter(low.y - roundingError.y, -std::numeric_limits<float>::infinity());
                    high.x = std::nextafter(high.x + roundingError.x, std::numeric_limits<float>::infinity());
                    high.y = std::nextafter(high.y + roundingError.y, std::numeric_limits<float>::infinity());
                    tileRipples.clear();
                    for (const auto& bounds : activeRipples)
                        if (!finiteCorners || overlapsTile(bounds, low, high)) tileRipples.push_back(bounds);
                    if (tileRipples.empty()) continue;
                    bakeRegion(tileX, tileY, endX, endY, tileRipples);
                }
            }
        }
    }
    {
        FBZZ_PROFILE_SCOPE("WaterRipple::Upload");
        if (!resources.Get(state.gpuTex))
            state.gpuTex = resources.CreateDynamicTexture(state.width, state.height, renderer::DynamicTextureFormat::RGBA8);
        auto* texture = resources.Get(state.gpuTex);
        const bool updated = texture && texture->UpdateRegion(0, 0, state.width, state.height,
            state.pixels.data(), state.width * 4u);
        if (!updated) {
            if (state.gpuTex.IsValid()) resources.Release(state.gpuTex);
            state.gpuTex = resources.CreateTexture(state.pixels.data(), state.width, state.height);
        }
        state.signatureValid = resources.Get(state.gpuTex) != nullptr;
    }
    state.inputSignature = signature;
    state.dirty = !state.signatureValid;
    state.hadRipples = !water.ripples.empty();
}

/// @note viewProjection は TAA ジッター込みで渡す。カメラから組み直すとジッターが落ちる。
/// @param resources 焼いた速度場を常駐させる (VelocityFieldAtlas) のに要る。
WaterCB BuildWaterCB(const WaterComponent& water, const asset::MaterialAssetAuthoring* mat,
                     const Transform& transform, const math::Matrix4& viewProjection, float time,
                     float skyReflection, const WaterDetailNoise& detailNoise,
                     renderer::ResourceManager& resources)
{
    WaterCB cb{};
    const math::Matrix4 world = transform.GetWorldMatrix();
    cb.worldMatrix = world;
    cb.wvpMatrix = viewProjection * world;

    const auto shallowColor = WGetF3(mat, "shallowColor", { 0.20f, 0.60f, 0.70f });
    const auto deepColor    = WGetF3(mat, "deepColor",    { 0.00f, 0.10f, 0.30f });
    cb.shallowColorDepth = { shallowColor.x, shallowColor.y, shallowColor.z,
                              WGetF(mat, "shallowDepth", 0.5f) };
    cb.deepColorDepth    = { deepColor.x, deepColor.y, deepColor.z,
                              WGetF(mat, "deepDepth", 5.0f) };
    cb.surfaceParams = {
        WGetF(mat, "opacity",       0.85f),
        WGetF(mat, "reflectivity",  0.5f),
        WGetF(mat, "fresnelBias",   0.02f),
        WGetF(mat, "fresnelPower",  5.0f)
    };
    const auto optics = (mat ? asset::ResolveWaterOpticalSettings(*mat) : asset::WaterOpticalSettings{});
    cb.absorptionCoefficients = {optics.absorption.x, optics.absorption.y, optics.absorption.z, 0.0f};
    cb.scatteringCoefficients = {optics.scattering.x, optics.scattering.y, optics.scattering.z, 0.0f};
    cb.opticalParams = {optics.enabled ? 1.0f : 0.0f, optics.phaseG, 0.0f, 0.0f};
    /// @note 水面の «実寸» はローカル extent ではなくワールド実寸。拡大された水面でも、縁のフェードと
    /// @note 波のエイリアシング判定が、画面に出ているとおりの大きさで効くようにする。
    const auto extent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
    const float worldExtentX = extent.x * std::abs(transform.worldScale.x);
    const float worldExtentZ = extent.y * std::abs(transform.worldScale.z);
    /// @note z はさざ波タイルの勾配復号係数。ベイクした値をそのまま渡し、シェーダー側に定数を
    /// @note 写さない (写すとタイルを焼き直してもシェーダーが古い係数で復号し続ける)。
    cb.normalParams = { worldExtentX, worldExtentZ, detailNoise.derivativeScale,
                        WGetF(mat, "normalStrength", 1.0f) };
    /// @note xy は描画の基準セル幅。物理の波高はセル幅に依存しない。
    /// @note z はタイルのセル数の逆数。
    cb.timeParams   = { water.cellSize.x, water.cellSize.y, detailNoise.invTileCells, time };
    cb.gridParams = { water.cameraFocusedGrid ? 1.0f : 0.0f,
        (std::clamp)(water.nearCellSize, 0.25f, 10.0f),
        static_cast<float>(water.resolutionX), static_cast<float>(water.resolutionZ) };
    cb.foamParams = {
        WGetF(mat, "foamThreshold",     0.3f),
        WGetF(mat, "foamFade",          0.5f),
        WGetF(mat, "foamStrength",      1.0f),
        /// @note 泡のムラはワールド座標で評価するため、旧 foamTiling(UV 倍率) とは単位が違う。
        WGetF(mat, "foamNoiseScale",    0.5f)
    };
    cb.crestFoamParams = {
        math::Clamp01(WGetF(mat, "crestFoamThreshold", 0.25f)),
        (std::clamp)(WGetF(mat, "crestFoamFade", 0.15f), 0.001f, 1.0f),
        (std::clamp)(WGetF(mat, "crestFoamStrength", 0.9f), 0.0f, 4.0f),
        (std::clamp)(WGetF(mat, "crestFoamLifetime", 1.2f), 0.0f, 10.0f)
    };
    const auto foamColor = WGetF3(mat, "foamColor", { 0.95f, 0.98f, 1.0f });
    cb.foamColorRoughness = { math::Clamp01(foamColor.x), math::Clamp01(foamColor.y),
        math::Clamp01(foamColor.z), (std::clamp)(WGetF(mat, "foamRoughness", 0.7f), 0.05f, 1.0f) };
    /// @note 外周フェードの幅はメートルで持ち、軸ごとに extent で割って UV へ直す。UV 比で
    /// @note 持つと大きい水面ほど帯が広くなり、縁から何 m で消えるかが大きさに依存してしまう。
    const float edgeFadeMeters = (std::max)(WGetF(mat, "edgeFade", 1.5f), 0.0f);
    cb.refractionFlowParams = {
        WGetF(mat, "refractionStrength", 0.03f),
        WGetF(mat, "flowSpeed",          0.3f),
        edgeFadeMeters / worldExtentX,
        edgeFadeMeters / worldExtentZ
    };
    cb.detailParams = {
        WGetF(mat, "detailScale",    0.35f),
        WGetF(mat, "detailSpeed",    0.6f),
        WGetF(mat, "detailStrength", 1.0f),
        math::Clamp01(WGetF(mat, "smoothness", 0.92f))
    };
    const auto sssColor = WGetF3(mat, "sssColor", { 0.12f, 0.50f, 0.46f });
    cb.sssParams = { sssColor.x, sssColor.y, sssColor.z,
                     math::Clamp01(WGetF(mat, "sssStrength", 0.6f)) };
    /// @note 波の山ほど透過光を強くするため、CPU 側と同じ「振幅の合計」を波高の基準として渡す。
    /// @note 群の包絡と方向広がりのぶんも含める。含めないと、山に当たった波だけ waveState.x が 1 で
    /// @note 飽和し、透過光と白波が «そこだけ最大» に張り付く。
    const float peak = (1.0f + math::Clamp01(water.waveGrouping))
                     * WaterComponent::WaveSpreadAmplitudeSum(water.waveSpread);
    float waveHeightSum = 0.0f;
    if (water.enableGerstnerWaves)
        for (size_t i = 0; i < (std::min)(static_cast<size_t>(water.waveCount), water.waves.size()); ++i)
            waveHeightSum += (std::max)(water.waves[i].amplitude, 0.0f);
    cb.reflectParams = {
        skyReflection * math::Clamp01(WGetF(mat, "skyReflection", 1.0f)),
        math::Clamp01(water.waveGrouping),
        transform.worldPosition.y,
        (std::max)(waveHeightSum * peak, 0.01f)
    };
    const auto flowDir = WGetF2(mat, "flowDirection", { 1.0f, 0.0f });
    const float flowLen = std::sqrt(flowDir.x * flowDir.x + flowDir.y * flowDir.y);
    /// @note zw はさざ波の «形»。異方比は風向と直交する «うね» の伸び、ワープ幅はうねりの斜面が
    /// @note さざ波を運ぶ距離 [m]。どちらも 1.0 / 0.0 にすれば従来の等方・非追従へ戻る。
    const math::Vector2 detailShape = {
        (std::max)(WGetF(mat, "detailAnisotropy", 2.0f), 1.0f),
        (std::max)(WGetF(mat, "detailWarp",       0.5f), 0.0f)
    };
    cb.flowParams = flowLen > 1.0e-4f
        ? math::Vector4{ flowDir.x / flowLen, flowDir.y / flowLen, detailShape.x, detailShape.y }
        : math::Vector4{ 1.0f, 0.0f, detailShape.x, detailShape.y };
    /// @note 流れの場が出す形・流れ・質感。本数は y に入れる (枠を 1 つ増やさずに済む)。
    /// @note 高さも流速も CPU で決める。形の式は WaterComponent が正本で、浮力・水中判定と
    /// @note 同じ値でなければならない。シェーダーで導き直すと二重化が 1 本増える。
    const int flowCount = (std::min)(water.surfaceFlowCount, kWaterSurfaceFlowCount);
    cb.waveShapeParams = { math::Clamp01(water.waveSpread),
                           static_cast<float>(flowCount), 0.0f, 0.0f };
    for (int i = 0; i < kWaterSurfaceFlowCount; ++i) {
        if (i >= flowCount) continue;
        const WaterSurfaceFlow& f = water.surfaceFlows[static_cast<size_t>(i)];
        cb.surfaceFlowA[i] = { f.center.x, f.center.y, f.radius, f.height };
        cb.surfaceFlowB[i] = { f.speed, f.falloffPower,
                               static_cast<float>(static_cast<int>(f.kind)), f.chop };
        /// @note 常駐できなかった場はタイル -1 で渡す。本数を詰めると別の流れに化けるので、
        /// @note 枠は残したまま «無効» を送る (ParticleGpuSim と同じ扱い)。
        float tile = -1.0f;
        float maxMagnitude = 1.0f;
        if (f.kind == FlowFieldType::Baked && f.vectorField != nullptr) {
            tile = static_cast<float>(asset::VelocityFieldAtlas::Acquire(*f.vectorField, resources));
            maxMagnitude = f.vectorField->maxMagnitude;
        }
        cb.surfaceFlowC[i] = { f.direction.x, f.direction.y, tile, maxMagnitude };
        cb.surfaceFlowD[i] = { f.inverseRotation.x, f.inverseRotation.y,
                               f.inverseRotation.z, f.inverseRotation.w };
        cb.surfaceFlowE[i] = { f.extents.x, f.extents.y, f.extents.z, f.planeOffsetY };
    }

    const size_t waveCount = (std::min)(static_cast<size_t>(water.waveCount), water.waves.size());
    cb.waveControls = {static_cast<float>(waveCount), 0.0f, 0.0f, 1.0f};
    const WaterDepthField* depthField = water.EffectiveDepthField(WaterSurfaceReadView::SIMULATION);
    const float attenuationDepth = water.EffectiveWaveAttenuationDepth(WaterSurfaceReadView::SIMULATION);
    cb.waveDepthParams = {attenuationDepth,
        depthField && attenuationDepth > 0.0f ? 1.0f : 0.0f,
        depthField ? depthField->outsideFadeWidth : 1.0f, 0.0f};
    const float horizontalScale = water.GetHorizontalWaveScale(WaterSurfaceReadView::SIMULATION);
    std::array<float, 16> phases{};
    for (size_t i = 0; i < waveCount; ++i) {
        const GerstnerWave& wave = water.waves[static_cast<size_t>(i)];
        assert(wave.steepness <= 1.0f);

        const float directionLengthSq = wave.direction.LengthSq();
        const bool validWave = water.enableGerstnerWaves && wave.amplitude > 0.0001f
            && wave.wavelength > 0.0001f && std::isfinite(directionLengthSq) && directionLengthSq > 1.0e-8f;
        const math::Vector2 dir = validWave ? wave.direction.Normalized() : math::Vector2{1.0f, 0.0f};
        const float k = validWave ? math::TWO_PI / wave.wavelength : 0.0f;
        const float omega = validWave ? std::sqrt(9.8f * k) : 0.0f;
        const float steepness = math::Clamp01(wave.steepness) * horizontalScale;
        cb.waveDir[i] = { dir.x, dir.y, steepness, validWave ? 1.0f : 0.0f };
        cb.waveParams[i] = { validWave ? wave.amplitude : 0.0f, wave.wavelength, omega, k };
        phases[i] = validWave ? wave.phaseOffsetRadians : 0.0f;
    }
    for (size_t i = 0; i < 4; ++i)
        cb.wavePhasePacked[i] = {phases[i * 4], phases[i * 4 + 1], phases[i * 4 + 2], phases[i * 4 + 3]};

    return cb;
}

WaterEffectParams BuildWaterEffectParams(const asset::MaterialAssetAuthoring* mat)
{
    WaterEffectParams params{};
    params.rimGlowStrength    = WGetF(mat, "rimGlowStrength",    params.rimGlowStrength);
    params.minShallowAlpha    = WGetF(mat, "minShallowAlpha",    params.minShallowAlpha);
    params.specularStrength   = WGetF(mat, "specularStrength",   params.specularStrength);
    const math::Vector3 skyTint = WGetF3(mat, "skyReflectTint", {
        params.skyReflectTint[0], params.skyReflectTint[1], params.skyReflectTint[2]
    });
    params.skyReflectTint[0] = skyTint.x;
    params.skyReflectTint[1] = skyTint.y;
    params.skyReflectTint[2] = skyTint.z;
    const math::Vector3 rippleColor = WGetF3(mat, "rippleRingColor", {
        params.rippleRingColor[0], params.rippleRingColor[1], params.rippleRingColor[2]
    });
    params.rippleRingColor[0] = rippleColor.x;
    params.rippleRingColor[1] = rippleColor.y;
    params.rippleRingColor[2] = rippleColor.z;
    params.rippleRingStrength = WGetF(mat, "rippleRingStrength", params.rippleRingStrength);
    return params;
}

/// @brief ローカル AABB をワールド行列で包み直して可視判定する。
/// @note 描画側は `Transform::GetWorldMatrix()` を使い親の回転・スケールが乗るため、
/// @note カリングをローカル position 基準にすると水面を子にしたり拡大した瞬間に消える。
void ExpandByWaveMargin(const WaveMargin& margin, math::Vector3& outMin, math::Vector3& outMax)
{
    outMin.x -= margin.horizontal;
    outMin.z -= margin.horizontal;
    outMax.x += margin.horizontal;
    outMax.z += margin.horizontal;
    outMin.y -= margin.vertical;
    outMax.y += margin.vertical;
}

}
void QueueWaterSplash(Scene& scene, EntityID owner, const math::Vector3& worldPos, float intensity)
{
    if (!std::isfinite(intensity)) return;
    const float strength = math::Clamp01(intensity);
    WaterInteractionImpulse impulse{WaterInteractionKind::SURFACE_FOAM, worldPos,
        {0.0f, 1.5f + strength * 3.0f, 0.0f}, 0.2f + strength * 0.3f, strength, 1.2f};
    impulse.emitSurfaceRipple = false;
    static_cast<void>(QueueWaterInteraction(scene, owner, impulse));
}

namespace {
std::array<math::Vector4, 3> WorldToLocalRows(const math::Vector3& position,
    const math::Quaternion& inverseRotation, const math::Vector3& inverseScale)
{
    const auto x = inverseRotation * math::Vector3::RIGHT;
    const auto y = inverseRotation * math::Vector3::UP;
    const auto z = inverseRotation * math::Vector3::FORWARD;
    const math::Vector3 rows[3] = {
        {x.x * inverseScale.x, y.x * inverseScale.x, z.x * inverseScale.x},
        {x.y * inverseScale.y, y.y * inverseScale.y, z.y * inverseScale.y},
        {x.z * inverseScale.z, y.z * inverseScale.z, z.z * inverseScale.z}
    };
    return {{{rows[0].x, rows[0].y, rows[0].z, -math::Vector3::Dot(rows[0], position)},
             {rows[1].x, rows[1].y, rows[1].z, -math::Vector3::Dot(rows[1], position)},
             {rows[2].x, rows[2].y, rows[2].z, -math::Vector3::Dot(rows[2], position)}}};
}

float MediumSetting(const asset::MaterialAssetAuthoring* material, const char* name,
    float fallback, float minimum, float maximum)
{
    const float value = WGetF(material, name, fallback);
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

renderer::WaterMediumInput BuildMediumInput(const WaterComponent& water, const Transform& transform,
    EntityID owner, const asset::MaterialAssetAuthoring* material, const WaterMesh& mesh, const WaveMargin& margin)
{
    renderer::WaterMediumInput medium;
    medium.stableOwnerKey = (static_cast<uint64_t>(owner.generation) << 32u) | owner.index;
    const auto inverseRotation = transform.worldRotation.Inverse();
    medium.worldToLocalRows = WorldToLocalRows(transform.worldPosition, inverseRotation,
        {1.0f / transform.worldScale.x, 1.0f / transform.worldScale.y, 1.0f / transform.worldScale.z});
    float minimumX = std::numeric_limits<float>::max(), minimumZ = minimumX;
    float maximumX = -minimumX, maximumZ = -minimumX;
    for (uint32_t corner = 0; corner < 8u; ++corner) {
        const math::Vector3 local = {
            (corner & 1u) ? mesh.aabbMax.x : mesh.aabbMin.x,
            (corner & 2u) ? mesh.aabbMax.y : mesh.aabbMin.y,
            (corner & 4u) ? mesh.aabbMax.z : mesh.aabbMin.z
        };
        const auto world = transform.worldPosition + transform.worldRotation * math::Vector3{
            local.x * transform.worldScale.x, local.y * transform.worldScale.y, local.z * transform.worldScale.z};
        minimumX = (std::min)(minimumX, world.x); minimumZ = (std::min)(minimumZ, world.z);
        maximumX = (std::max)(maximumX, world.x); maximumZ = (std::max)(maximumZ, world.z);
    }
    medium.worldBoundsXZ = {minimumX - margin.horizontal, minimumZ - margin.horizontal,
                           maximumX + margin.horizontal, maximumZ + margin.horizontal};
    const auto optics = (material ? asset::ResolveWaterOpticalSettings(*material) : asset::WaterOpticalSettings{});
    medium.opticalEnabled = optics.enabled;
    medium.absorptionPhaseG = {optics.absorption.x, optics.absorption.y, optics.absorption.z, optics.phaseG};
    medium.scatteringLight = {optics.scattering.x, optics.scattering.y, optics.scattering.z,
        MediumSetting(material, "underwaterLightStrength", 1.0f, 0.0f, 8.0f)};
    medium.surfaceY = transform.worldPosition.y;
    medium.depthLimit = water.buoyancyDepth;
    medium.analyticRectangle = !UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
    const auto localExtent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
    medium.localHalfExtent = localExtent * 0.5f;
    medium.bottomY = medium.surfaceY - medium.depthLimit;
    medium.maxDistance = MediumSetting(material, "underwaterMaxDistance", 120.0f, 1.0f, 10000.0f);
    medium.transitionDepth = MediumSetting(material, "underwaterTransitionDepth", 0.15f, 0.001f, 2.0f);
    medium.distortion = MediumSetting(material, "underwaterDistortion", 0.001f, 0.0f, 0.01f);
    medium.lightSteps = static_cast<int>(MediumSetting(material, "underwaterLightSteps", 48.0f, 16.0f, 64.0f));
    medium.causticsEnabled = MediumSetting(material, "enableCaustics", 0.0f, 0.0f, 1.0f) >= 0.5f;
    medium.causticsIntensity = MediumSetting(material, "causticsIntensity", 1.0f, 0.0f, 100.0f);
    if (const auto* exclusions = WaterSurfaceExclusions(water, WaterSurfaceReadView::SIMULATION)) {
        medium.exclusions.reserve(exclusions->boxes.size());
        for (const auto& box : exclusions->boxes) {
            medium.exclusions.push_back({WorldToLocalRows(box.worldPosition, box.inverseWorldRotation,
                box.inverseWorldScale), box.halfExtent});
        }
    }
    if (const auto frame = WaterInteractionSnapshot(water)) {
        medium.plumes.reserve(frame->plumes.size());
        for (const auto& plume : frame->plumes) {
            if (plume.density <= 0.0f || plume.radius <= 0.0f
                || (plume.scattering.x <= 0.0f && plume.scattering.y <= 0.0f
                    && plume.scattering.z <= 0.0f)) continue;
            medium.plumes.push_back({{plume.worldPosition.x, plume.worldPosition.y, plume.worldPosition.z, plume.radius},
                                    {plume.scattering.x, plume.scattering.y, plume.scattering.z, plume.density}});
        }
    }
    return medium;
}
} /// @note namespace

void ExtractRenderWater(RenderPassContext& ctx, renderer::RenderScene& output) {
    auto& scene = ctx.scene;
    auto& resources = ctx.resources;
    for (auto [water, transform] : scene.View<WaterComponent, Transform>()) {
        if (!water.enabled || WaterSurfaceEffectiveShape(water, WaterSurfaceReadView::SIMULATION)
                != WaterSurfaceShape::RECTANGLE
            || !IsWaterSurfaceSupported(water, transform, WaterSurfaceReadView::SIMULATION)) continue;
        water.extentX = (std::clamp)(water.extentX, 0.1f, 10000.0f);
        water.extentZ = (std::clamp)(water.extentZ, 0.1f, 10000.0f);
    }
    {
        FBZZ_PROFILE_SCOPE("WaterPrepare::Surfaces");
        PrepareWaterSceneSurfaces(scene);
    }
    if (s_waterResourceOwner != resources.GetInstanceId() || s_waterResourceVersion != resources.GetResetVersion()) {
        s_waterResourceOwner = resources.GetInstanceId(); s_waterResourceVersion = resources.GetResetVersion();
        /// @note An epoch switch invalidates the old owner/handles. Abandon them like the geometry caches; Material's Active() destructor must not release into the new manager.
        for (auto& [key, appearance] : s_appearanceCache) appearance.paramsBuffer = {};
        s_meshCache.clear(); s_texCache.clear(); s_appearanceCache.clear(); s_rippleStates.clear(); s_terrainInfoStates.clear();
    }
    const uint64_t sceneGeneration = scene.GetRenderSceneGeneration();
    for (EntityID eid : scene.GetEntities<WaterComponent>()) {
        const auto* water = scene.GetComponent<WaterComponent>(eid);
        if (water && WaterSurfaceZoneOwner(*water, WaterSurfaceReadView::SIMULATION).IsValid())
            ReleaseWaterCacheEntries(sceneGeneration, &eid, resources);
    }
    {
        std::unordered_set<WaterCacheKey, WaterCacheKeyHash> validKeys;
        for (EntityID eid : scene.GetEntities<WaterComponent>())
            validKeys.insert({sceneGeneration, eid.index, eid.generation});

        std::erase_if(s_meshCache, [&validKeys, sceneGeneration, &resources](auto& kv) {
            if (kv.first.sceneGeneration != sceneGeneration || validKeys.contains(kv.first)) return false;
            ReleaseWaterMesh(kv.second, resources);
            return true;
        });
        std::erase_if(s_appearanceCache, [&validKeys, sceneGeneration, &resources](auto& entry) {
            if (entry.first.sceneGeneration != sceneGeneration || validKeys.contains(entry.first)) return false;
            if (entry.second.paramsBuffer.IsValid()) resources.Release(entry.second.paramsBuffer);
            entry.second.paramsBuffer = {};
            return true;
        });
        std::erase_if(s_texCache, [&validKeys, sceneGeneration, &resources](auto& kv) {
            if (kv.first.sceneGeneration != sceneGeneration || validKeys.contains(kv.first)) return false;
            if (kv.second.foamMask.IsValid()) resources.Release(kv.second.foamMask);
            return true;
        });
        std::erase_if(s_rippleStates, [&validKeys, sceneGeneration, &resources](auto& kv) {
            if (kv.first.sceneGeneration != sceneGeneration || validKeys.contains(kv.first)) return false;
            if (kv.second.gpuTex.IsValid()) resources.Release(kv.second.gpuTex);
            return true;
        });
    }
    {
        FBZZ_PROFILE_SCOPE("WaterPrepare::DepthFields");
        PrepareWaterDepthFields(scene, WaterSurfaceReadView::SIMULATION);
    }
    TerrainInfoState& terrainInfo = s_terrainInfoStates[scene.GetRenderSceneGeneration()];
    /// @note 借用 Terrain pointer は抽出中だけ保持する。同一 frame の追加・削除・配列移動でも再取得する。
    WaterTerrainInfoInputs terrainInputs;
    const bool validTerrainInputs = [&]() {
        FBZZ_PROFILE_SCOPE("WaterPrepare::TerrainSources");
        return terrainInfo.cache.TryCollect(scene, terrainInputs);
    }();
    if (!validTerrainInputs) terrainInputs = {};
    std::unordered_set<uint64_t> selectedZoneOwners;
    for (const auto& selected : ctx.settings.selectedObjects) {
        const EntityID selectedId{selected.index, selected.generation};
        const WaterComponent* selectedWater = scene.GetComponent<WaterComponent>(selectedId);
        if (selectedWater) {
            const EntityID owner = WaterSurfaceZoneOwner(*selectedWater, WaterSurfaceReadView::SIMULATION);
            if (owner.IsValid())
                selectedZoneOwners.insert((static_cast<uint64_t>(owner.generation) << 32u) | owner.index);
        }
    }
    const WaterCandidateLease candidates(scene);
    for (const EntityID eid : candidates.Entities()) {
        auto* ownerObject = scene.GetGameObject(eid);
        auto* waterComponent = scene.GetComponent<WaterComponent>(eid);
        if (!ownerObject || !waterComponent) continue;
        auto& water = *waterComponent;
        const auto& transform = ownerObject->transform;
        if (!water.enabled || WaterSurfaceZoneOwner(water, WaterSurfaceReadView::SIMULATION).IsValid()) continue;
        const Transform& surfaceTransform = WaterSurfaceTransform(water, transform,
            WaterSurfaceReadView::SIMULATION);
        if (!IsWaterSurfaceSupported(water, surfaceTransform, WaterSurfaceReadView::SIMULATION)) continue;
        /// @note 無効な除外設定を空のmaskへ置き換えると乾燥領域が水没するため、描画はfail-closed。
        if (const auto* exclusions = WaterSurfaceExclusions(water, WaterSurfaceReadView::SIMULATION);
            exclusions && !exclusions->valid) continue;
        const bool splineSurface = UsesWaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
        const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, WaterSurfaceReadView::SIMULATION);
        if (splineSurface && !geometry) continue;
        water.resolutionX = std::clamp(water.resolutionX, 1u, 512u);
        water.resolutionZ = std::clamp(water.resolutionZ, 1u, 512u);
        water.chunkCount  = std::clamp(water.chunkCount,  1u, 64u);
        if (!splineSurface) {
            water.extentX = std::clamp(water.extentX, 0.1f, 10000.0f);
            water.extentZ = std::clamp(water.extentZ, 0.1f, 10000.0f);
        }
        water.lodPatchResolution = (std::clamp)(water.lodPatchResolution, 2u, 64u);
        uint32_t patchCells = 2;
        while (patchCells < water.lodPatchResolution) patchCells *= 2;
        water.lodPatchResolution = patchCells;

        const WaterCacheKey cacheKey{scene.GetRenderSceneGeneration(), eid.index, eid.generation};
        if (!ownerObject->activeInHierarchy()) continue;

        const WaveMargin margin = ComputeWaveMargin(water);
        asset::MaterialAsset::AuthoringSnapshotPtr matAuthoringSnapshot;
        const asset::MaterialAssetAuthoring* mat = nullptr;
        if (!water.materialPath.empty()) {
            const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(water.materialPath);
            {
                matAuthoringSnapshot = asset::ReadMaterialAuthoring(handle);
                mat = matAuthoringSnapshot ? &matAuthoringSnapshot->Data() : nullptr;
            }
        }

        const float foamThreshold = WGetF(mat, "foamThreshold", 0.3f);
        const float foamFade      = WGetF(mat, "foamFade",      0.5f);
        WaterTerrainBakeSettings terrainSettings;
        const auto terrainExtent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
        /// @note 地形情報の密度は描画 LOD と独立。最大 257 平方の範囲で約 1 m/texel を目標とする。
        terrainSettings.width = static_cast<uint32_t>((std::clamp)(
            std::ceil(terrainExtent.x * std::abs(surfaceTransform.worldScale.x)), 64.0f, 257.0f));
        terrainSettings.height = static_cast<uint32_t>((std::clamp)(
            std::ceil(terrainExtent.y * std::abs(surfaceTransform.worldScale.z)), 64.0f, 257.0f));
        terrainSettings.foamThreshold = (std::max)(foamThreshold, 0.0f);
        terrainSettings.foamFade = (std::max)(foamFade, 0.0f);
        uint64_t terrainSignature = 0;
        const bool validTerrainSettings = TryComputeWaterTerrainInfoSignature(
            terrainInputs, water, surfaceTransform, terrainSettings, terrainSignature,
            WaterSurfaceReadView::SIMULATION);
        if (const WaterDepthField* depthField = water.EffectiveDepthField(WaterSurfaceReadView::SIMULATION);
            depthField && water.EffectiveWaveAttenuationDepth(WaterSurfaceReadView::SIMULATION) > 0.0f)
            terrainSignature = depthField->signature;

        const auto cachedMesh = s_meshCache.find(cacheKey);
        const bool geometryChanged = cachedMesh != s_meshCache.end()
            && cachedMesh->second.geometry != ResolveWaterMeshGeometry(water);
        if (water.meshDirty || cachedMesh == s_meshCache.end() || geometryChanged) {
            FBZZ_PROFILE_SCOPE("WaterPrepare::Mesh");
            BuildWaterMesh(water, s_meshCache[cacheKey], resources);
            water.meshDirty = false;
            water.foamDirty = true;
        }

        if (water.texDirty || water.foamDirty || !s_texCache.contains(cacheKey)
            || s_texCache.at(cacheKey).signature != terrainSignature) {
            FBZZ_PROFILE_SCOPE("WaterPrepare::Textures");
            WaterTextures& cached = s_texCache[cacheKey];
            /// @note 焼き直しは解像度スライダー操作中フレーム毎に走るため、上書き前に解放する。
            /// @note 返さず差し替えると 1 フレームぶんの泡マスクがそのまま漏れ続ける。
            if (cached.foamMask.IsValid()) resources.Release(cached.foamMask);
            cached = BuildTextureSet(
                water, surfaceTransform, terrainInputs, validTerrainInputs && validTerrainSettings,
                resources, terrainSettings, terrainSignature);
            water.texDirty = false;
            water.foamDirty = false;
        }

        /// @note 波紋がなく既存テクスチャも有効なら CPU 更新・GPU アップロードを省く。
        /// @note hadRipples を見るのは最後の 1 つが消えたフレームに 1 回だけ焼き直すため。
        WaterRippleState& rippleState = s_rippleStates[cacheKey];
        if (rippleState.dirty || !rippleState.gpuTex.IsValid()
            || !water.ripples.empty() || rippleState.hadRipples) {
            FBZZ_PROFILE_SCOPE("WaterPrepare::Ripples");
            UpdateRippleState(rippleState, water, surfaceTransform, resources);
        }

        const WaterMesh& mesh = s_meshCache.at(cacheKey);
        renderer::RenderWaterInput input;
        input.layer = static_cast<uint32_t>(scene.GetGameObject(eid)->layer);
        const uint64_t entityKey = (static_cast<uint64_t>(eid.generation) << 32u) | eid.index;
        input.selected = ctx.settings.IsSelected(eid.index, eid.generation)
            || selectedZoneOwners.contains(entityKey);
        input.margin = margin;
        input.aabbMin = mesh.aabbMin; input.aabbMax = mesh.aabbMax;
        input.patches = mesh.chunks;
        input.adaptivePatchLod = !splineSurface && (water.adaptivePatchLod || water.farOceanEnabled);
        input.patchMorphEnabled = !splineSurface && water.lodMorphEnabled;
        input.farOceanEnabled = WaterSurfaceUsesHorizon(water, WaterSurfaceReadView::SIMULATION);
        input.farOceanCoverageScale = water.farOceanCoverageScale;
        input.farOceanRippleFadeDistance = water.farOceanRippleFadeDistance;
        input.patchLod.cellsPerPatch = water.lodPatchResolution;
        input.patchLod.maxDepth = (std::min)(water.lodMaxDepth, 10u);
        input.patchLod.maxPatches = (std::clamp)(water.lodMaxPatches, 1u, 4096u);
        input.patchLod.targetCellPixels = water.lodTargetPixels;
        input.patchLod.nearDistance = water.lodNearDistance;
        input.patchLod.nearCellSize = water.lodNearCellSize;
        input.patchLod.morphEnabled = !splineSurface && water.lodMorphEnabled;
        input.patchLod.morphTransitionWidth = water.lodMorphTransitionWidth;
        input.patchVertexBuffer = mesh.patchVertexBuffer;
        input.patchIndexBuffers = mesh.patchIndexBuffers;
        input.patchIndexCounts = mesh.patchIndexCounts;
        const auto& noise = GetWaterDetailNoise(resources);
        {
            FBZZ_PROFILE_SCOPE("WaterPrepare::Constants");
            input.constants = BuildWaterCB(water, mat, surfaceTransform, math::Matrix4::Identity(), Time::time, 1.0f, noise, resources);
        }
        if (splineSurface) {
            input.constants.gridParams.x = 0.0f;
            input.constants.farOceanParams = {};
            const float maximumWorldEdge = geometry->maximumWorldEdgeLength;
            if (std::isfinite(maximumWorldEdge) && maximumWorldEdge > 0.0f)
                input.constants.timeParams.x = input.constants.timeParams.y = maximumWorldEdge;
        }
        input.constants.terrainInfoParams = s_texCache.at(cacheKey).terrainInfoParams;
        {
            FBZZ_PROFILE_SCOPE("WaterPrepare::Medium");
            input.medium = BuildMediumInput(water, surfaceTransform, eid, mat, mesh, margin);
        }
        input.medium.depthIsUniform = s_texCache.at(cacheKey).depthIsUniform;
        input.effects = BuildWaterEffectParams(mat);
        input.detailNoise = noise.texture;
        input.foam = s_texCache.at(cacheKey).foamMask;
        input.ripple = rippleState.gpuTex;
        input.velocityField = asset::VelocityFieldAtlas::Texture(resources);
        if (mat && !mat->shaderPath.empty()) {
            FBZZ_PROFILE_SCOPE("WaterPrepare::Shader");
            renderer::Material& appearance = s_appearanceCache[cacheKey];
            appearance.shaderPath = mat->shaderPath;
            appearance.shader = resources.LoadShader(mat->shaderPath);
            input.shader = appearance.shader;
            if (const auto* shader = resources.Get(appearance.shader)) {
                /// @note Copy reflection for this publication; a successful hot reload may replace its owner later.
                const renderer::ShaderDescriptor descriptor = shader->GetDescriptor();
                asset::InitializeWaterAppearanceParameters(*mat, descriptor, appearance.paramData);
                const auto texturePaths = asset::ResolveMaterialTexturePaths(*mat);
                appearance.textures.resize(texturePaths.size());
                for (size_t slot = 0; slot < texturePaths.size(); ++slot)
                    appearance.textures[slot] = texturePaths[slot].empty()
                        ? renderer::ResourceHandle<renderer::TextureTag>{}
                        : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, texturePaths[slot]);
                appearance.Upload(resources, descriptor);
                input.appearanceParameters = appearance.paramsBuffer;
                std::copy(appearance.textures.begin(), appearance.textures.end(), input.appearanceTextures.begin());
            }
        }
        output.water.push_back(std::move(input));
    }
}

void ReleaseWaterRenderResources(Scene& scene, renderer::ResourceManager& resources)
{
    ReleaseWaterCacheEntries(scene.GetRenderSceneGeneration(), nullptr, resources);
    s_terrainInfoStates.erase(scene.GetRenderSceneGeneration());
}

void ReleaseWaterRenderResources(Scene& scene, EntityID entity, renderer::ResourceManager& resources)
{
    ReleaseWaterCacheEntries(scene.GetRenderSceneGeneration(), &entity, resources);
}
} /// @note namespace fbzz::scene
