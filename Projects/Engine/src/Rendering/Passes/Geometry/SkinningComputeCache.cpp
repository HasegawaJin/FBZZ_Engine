/// @file    SkinningComputeCache.cpp
/// @brief   入力頂点とボーンパレットの GPU 資源を管理し、フレーム全体の実績を集計する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "SkinningComputeCache.hpp"
#include "BoneBakeSkinning.hpp"
#include "GeometryPasses.hpp"
#include "../../../Scene/Systems/Animation/AnimationWorkTrace.hpp"

#include <Graphics/Renderer/Resources/Buffers/DynamicBufferPool.hpp>
#include <Graphics/Renderer/Resources/Buffers/IBuffer.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Resources/Buffers/BufferReadScope.hpp>
#include <Graphics/Renderer/Geometry/SkinningPolicy.hpp>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene {

namespace skinning_cache_detail {

namespace {

struct SourceVertexCacheKey {
    const renderer::SkinnedVertex* cpuData = nullptr;
    uint32_t vertexCount = 0;
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    uint64_t contentVersion = 0;

    bool operator==(const SourceVertexCacheKey&) const = default;
};

struct SourceVertexCacheKeyHash {
    size_t operator()(const SourceVertexCacheKey& key) const noexcept
    {
        size_t result = std::hash<const renderer::SkinnedVertex*>{}(key.cpuData);
        result ^= static_cast<size_t>(key.vertexCount) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        result ^= static_cast<size_t>(key.vertexBuffer.id) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        result ^= static_cast<size_t>(key.vertexBuffer.gen) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        result ^= std::hash<uint64_t>{}(key.contentVersion) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
        return result;
    }
};

/// @note 入力頂点はポーズに依存せず共有できる。アドレス再利用は元の GPU ハンドルの世代と内容版で区別する。
struct SourceVertexCacheEntry {
    renderer::ResourceHandle<renderer::StructuredBufferTag> handle;
    uint32_t maxBoneIndex = 0;
};
std::unordered_map<SourceVertexCacheKey,
                   SourceVertexCacheEntry,
                   SourceVertexCacheKeyHash> g_srcVertexCache;

/// @note DX12 の Upload 領域をフレーム間で使い回すと GPU が別ポーズを読むため、パレットをプールから借りる。
renderer::DynamicStructuredBufferPool g_bonePalettePool;

/// @note 同じ Animator のパレットを共有する。ComponentArray の詰め替えに備えて世代付き EntityID を引く。
std::unordered_map<EntityID,
                   renderer::ResourceHandle<renderer::StructuredBufferTag>,
                   EntityIDHash> g_framePalettes;

/// @note Scene View 専用の入力も保持できるよう、使用集合はフレーム内の全ビューで累積する。
std::unordered_set<SourceVertexCacheKey, SourceVertexCacheKeyHash> g_usedSourceKeys;
uint64_t g_cacheUsageFrame = (std::numeric_limits<uint64_t>::max)();
uint64_t g_cacheOwner = 0;
uint64_t g_cacheResetVersion = 0;
struct FrameSkinningStats { uint32_t dispatches = 0, uploads = 0; uint64_t vertices = 0, bytes = 0; };
FrameSkinningStats g_frameStats;

void ReleaseSourceSkinningCache()
{
    if (auto* resources = renderer::ResourceManager::FindOwner(g_cacheOwner);
        resources && resources->GetResetVersion() == g_cacheResetVersion) {
        for (const auto& [key, entry] : g_srcVertexCache) {
            resources->Release(entry.handle);
        }
        renderer::ReleaseSkinningRequestCaches(*resources);
    }
    g_bonePalettePool.ReleaseAll();
    g_srcVertexCache.clear();
    g_framePalettes.clear();
    g_usedSourceKeys.clear();
    g_cacheUsageFrame = (std::numeric_limits<uint64_t>::max)();
    g_cacheOwner = 0;
    g_cacheResetVersion = 0;
    g_frameStats = {};
}

}

void BeginSkinningCacheFrame(renderer::ResourceManager& resources, uint64_t frameStamp, AnimationSkinningCpuRow* trace)
{
    if (g_cacheOwner != resources.GetInstanceId() || g_cacheResetVersion != resources.GetResetVersion()) {
        ReleaseSourceSkinningCache();
        g_cacheOwner = resources.GetInstanceId();
        g_cacheResetVersion = resources.GetResetVersion();
    }
    if (g_cacheUsageFrame == frameStamp) {
        return;
    }

    if (g_cacheUsageFrame != (std::numeric_limits<uint64_t>::max)()) {
        for (auto it = g_srcVertexCache.begin(); it != g_srcVertexCache.end();) {
            if (g_usedSourceKeys.contains(it->first)) {
                ++it;
                continue;
            }
            if (it->second.handle.IsValid()) {
                if(trace) {
                    ++trace->sourceReleaseCalls;
                }
                resources.Release(it->second.handle);
            }
            it = g_srcVertexCache.erase(it);
        }
    }

    g_usedSourceKeys.clear();
    g_framePalettes.clear();
    g_frameStats = {};
    g_cacheUsageFrame = frameStamp;
}

/// @note EnsureSourceVertexBuffer — スキニング入力の StructuredBuffer を Mesh 単位で用意する。
renderer::ResourceHandle<renderer::StructuredBufferTag> EnsureSourceVertexBuffer(
    renderer::ResourceManager& resources, const renderer::Mesh& mesh,
    renderer::ResourceHandle<renderer::BufferTag> source, size_t boneCount, AnimationSkinningCpuRow* trace,
    renderer::buffer_state_detail::BufferStateReadScope* bufferReads, uint64_t* capturedVersion)
{
    // cpuSkinnedVertices はインポート時に保持される。空なら GPU スキニングは行えない。
    const auto* sourceBuffer = bufferReads ? bufferReads->Get(source) : resources.Get(source);
    if (mesh.cpuSkinnedVertices.empty() || mesh.cpuSkinnedVertices.size() > UINT32_MAX
        || !sourceBuffer || sourceBuffer->GetContentVersion() == 0
        || sourceBuffer->GetStride() != sizeof(renderer::SkinnedVertex)
        || sourceBuffer->GetSize() < mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex)) {
        return {};
    }

    const SourceVertexCacheKey key{
        mesh.cpuSkinnedVertices.data(),
        static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        source, sourceBuffer->GetContentVersion()};
    if (capturedVersion) {
        *capturedVersion = key.contentVersion;
    }
    g_usedSourceKeys.insert(key);
    const auto it = g_srcVertexCache.find(key);
    if (it != g_srcVertexCache.end()) {
        if (resources.Get(it->second.handle)) {
            if(trace) {
                ++trace->sourceCacheHits;
            }
            return it->second.maxBoneIndex < boneCount ? it->second.handle : renderer::ResourceHandle<renderer::StructuredBufferTag>{};
        }
        g_srcVertexCache.erase(it);
    }

    if(trace) {
        ++trace->sourceCacheMisses;
    }
    // 元 mesh と同じ handle でも内容更新を持つため、authoritative な版付き入力を読む。
    std::vector<renderer::SkinnedVertex> currentVertices(mesh.cpuSkinnedVertices.size());
    if (!sourceBuffer->CopyData(0, currentVertices.size() * sizeof(renderer::SkinnedVertex), currentVertices.data())) {
        return {};
    }
    if(trace) {
        trace->sourceCopyBytes+=currentVertices.size()*sizeof(renderer::SkinnedVertex);
    }
    uint32_t maxBoneIndex = 0;
    for (const auto& vertex : currentVertices)
        for (uint32_t index : vertex.boneIndices) {
            maxBoneIndex = (std::max)(maxBoneIndex, index);
        }
    // ウェイトがゼロの頂点も含め、パレットの参照が次のキャラクターの領域へはみ出さないようにする。
    if (maxBoneIndex >= boneCount) {
        return {};
    }
    if(trace) {
        ++trace->sourceCreateAttempts;
    }
    const auto handle = resources.CreateGpuLocalStructuredBuffer(
        currentVertices.data(),
        static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        static_cast<uint32_t>(sizeof(renderer::SkinnedVertex)));

    if(trace && handle.IsValid()) {
        ++trace->sourceCreateSuccesses;
    }
    g_srcVertexCache[key] = {handle, maxBoneIndex};
    return handle;
}

/// @note 頂点ごとの動的インデックス参照が定数キャッシュの高速経路を外れやすいため、StructuredBuffer を使う。
renderer::ResourceHandle<renderer::StructuredBufferTag> AcquireBonePalette(
    renderer::ResourceManager& resources,
    EntityID animatorEntity,
    const std::vector<math::Matrix4>& matrices, AnimationSkinningCpuRow* trace)
{
    if (matrices.empty()) {
        return {};
    }
    if (const auto it = g_framePalettes.find(animatorEntity); it != g_framePalettes.end()) {
        return it->second;
    }

    const size_t count  = matrices.size();
    const auto   handle = g_bonePalettePool.Acquire(
        resources, count, static_cast<uint32_t>(sizeof(math::Matrix4)));
    if (handle.IsValid()) {
        resources.Update(handle, matrices.data(), count * sizeof(math::Matrix4));
        if(trace){++trace->requests.bonePaletteUploadCalls;trace->requests.bonePaletteUploadBytes+=count*sizeof(math::Matrix4);}
    }
    g_framePalettes[animatorEntity] = handle;
    return handle;
}

void ResetSkinningPalettes()
{
    g_framePalettes.clear();
}

bool HasSkinningPalette(EntityID entity)
{
    return g_framePalettes.contains(entity);
}

void RetainSkinningSource(const renderer::Mesh& mesh, renderer::ResourceHandle<renderer::BufferTag> source,
    uint64_t contentVersion)
{
    g_usedSourceKeys.insert({mesh.cpuSkinnedVertices.data(), static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        source, contentVersion});
}

void RecordDeferredSkinningDispatches(uint32_t dispatches, uint64_t vertices)
{
    g_frameStats.dispatches += dispatches;
    g_frameStats.vertices += vertices;
}

void AccumulateSkinningFrameStats(RenderPassContext& ctx)
{
    g_frameStats.dispatches += ctx.statsSkinningDispatchCount;
    g_frameStats.vertices += ctx.statsSkinningVertexCount;
    g_frameStats.uploads += ctx.statsSkinningPaletteUploads;
    g_frameStats.bytes += ctx.statsSkinningPaletteUploadBytes;
    // ビューごとに、先行するビューで準備して再利用した分も含め、エンジンの同一フレーム内の実処理を報告する。
    ctx.statsSkinningDispatchCount = g_frameStats.dispatches;
    ctx.statsSkinningVertexCount = g_frameStats.vertices;
    ctx.statsSkinningPaletteUploads = g_frameStats.uploads;
    ctx.statsSkinningPaletteUploadBytes = g_frameStats.bytes;
}

}

/// @note Scene 切り替えやデバイスリセット時に退役させ、旧デバイス世代のハンドルを再利用しない。
void ReleaseSkinningComputeCaches()
{
    ReleaseBoneBakeSkinningCaches();
    skinning_cache_detail::ReleaseSourceSkinningCache();
}

}
