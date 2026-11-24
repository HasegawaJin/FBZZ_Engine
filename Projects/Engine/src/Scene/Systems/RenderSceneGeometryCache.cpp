/// @file    RenderSceneGeometryCache.cpp
/// @brief   静的メッシュ・フレーム共有ジオメトリ・スナップショットの寿命を管理する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneExtraction.hpp"
#include "RenderSceneGeometryCache.hpp"
#include "RenderSceneMaterialDemand.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"

#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>


namespace fbzz::scene {

namespace render_extraction_detail {

namespace {

std::atomic<uint64_t> s_renderSceneSerial{ 0 };

enum class SnapshotBuffers { NONE, ITEMS, ITEMS_AND_OBJECTS };

/// @note 抽出スレッドごとに空配列の容量だけを保持する。項目は各 16 MiB、個体は各 8 MiB を最大 2 本ずつ。
class RenderGeometryStorage {
public:
    static constexpr size_t MAXIMUM_OBJECT_BYTES = 8u * 1024u * 1024u;
    void Borrow(renderer::RenderScene& output, SnapshotBuffers buffers)
    {
        const std::lock_guard lock(m_mutex);
        BorrowLargestBuffer(m_items, output.items);
        if (buffers == SnapshotBuffers::ITEMS_AND_OBJECTS) BorrowLargestBuffer(m_objects, output.geometry.EditObjects());
    }
    void RecycleItems(std::vector<renderer::RenderMeshItem>&& items)
    {
        /// @note 空きプールが GPU 資源の寿命を延ばさないよう、ロック前に変形チケットを解放する。
        items.clear();
        ReturnBuffer(m_items, std::move(items), 16u * 1024u * 1024u);
    }
    void RecycleObjects(std::vector<renderer::RenderObject>&& objects)
    {
        objects.clear();
        ReturnBuffer(m_objects, std::move(objects), MAXIMUM_OBJECT_BYTES);
    }
private:
    template<typename T>
    static void BorrowLargestBuffer(std::array<std::vector<T>, 2>& availableBuffers, std::vector<T>& output)
    {
        auto* largestBuffer = &availableBuffers.front();
        for (auto& buffer : availableBuffers)
            if (buffer.capacity() > largestBuffer->capacity()) largestBuffer = &buffer;
        output.swap(*largestBuffer);
    }
    template<typename T>
    void ReturnBuffer(std::array<std::vector<T>, 2>& availableBuffers, std::vector<T>&& retiredValues, size_t maximumBytes)
    {
        if (retiredValues.capacity() == 0 || retiredValues.capacity() > maximumBytes / sizeof(T)) return;
        /// @note 最後の描画側所有者が別スレッドでも、元の抽出スレッドのプールへ返す。
        const std::lock_guard lock(m_mutex);
        for (auto& buffer : availableBuffers) {
            if (buffer.capacity() != 0) continue;
            buffer = std::move(retiredValues);
            return;
        }
    }
    std::mutex m_mutex;
    std::array<std::vector<renderer::RenderMeshItem>, 2> m_items;
    std::array<std::vector<renderer::RenderObject>, 2> m_objects;
};

const std::shared_ptr<RenderGeometryStorage>& GeometryStorage()
{
    thread_local const auto storage = std::make_shared<RenderGeometryStorage>();
    return storage;
}

struct RenderSnapshotLease {
    RenderSnapshotLease(std::shared_ptr<RenderGeometryStorage> owner, SnapshotBuffers buffers) : storage(std::move(owner))
    {
        if (buffers != SnapshotBuffers::NONE) storage->Borrow(snapshot, buffers);
    }
    ~RenderSnapshotLease()
    {
        storage->RecycleItems(std::move(snapshot.items));
        if (!snapshot.geometry.IsShared()) storage->RecycleObjects(std::move(snapshot.geometry.EditObjects()));
    }
    std::shared_ptr<RenderGeometryStorage> storage;
    renderer::RenderScene snapshot;
};

std::shared_ptr<renderer::RenderScene> MakeRenderSnapshot(SnapshotBuffers buffers)
{
    auto lease = std::make_shared<RenderSnapshotLease>(GeometryStorage(), buffers);
    auto* snapshot = &lease->snapshot;
    /// @note 再入抽出や過去のビューを含め、最後の所有者が手放してから容量を返す。
    /// @see https://eel.is/c++draft/util.smartptr.shared.const 別名 shared_ptr の所有期間。
    return std::shared_ptr<renderer::RenderScene>(std::move(lease), snapshot);
}

/// @note キャッシュの形状スナップショットが姿勢と差分を直接所有する。各ビューはその既存の所有権だけを共有する。
void ShareFrameGeometry(renderer::RenderScene& view, const std::shared_ptr<const renderer::RenderScene>& geometry)
{
    view.geometry.UseShared(std::shared_ptr<const renderer::RenderObjectGeometry>(geometry, &geometry->geometry.Read()));
}

std::shared_ptr<renderer::RenderScene> PublishObjectGeometry(renderer::RenderScene& source)
{
    auto geometry = MakeRenderSnapshot(SnapshotBuffers::NONE);
    geometry->geometry = std::move(source.geometry);
    ShareFrameGeometry(source, geometry);
    return geometry;
}

/// @note 材質抽出の再入で置換され得るため、コールバックをまたいでスロットを参照しない。
thread_local std::array<StaticSceneGeometry, 4> s_staticGeometry;
thread_local size_t s_nextStaticGeometry = 0;
thread_local uint64_t s_staticGeometryRevision = 0;

/// @note 内部公開済みのスナップショットだけを記録し、索引のない場合は全項目から探索する。
struct PreparedSnapshotItems {
    const renderer::RenderScene* identity = nullptr;
    std::weak_ptr<const renderer::RenderScene> owner;
    std::shared_ptr<const std::vector<uint32_t>> indices;
};
thread_local std::array<PreparedSnapshotItems, 32> s_preparedSnapshotItems;
thread_local size_t s_nextPreparedSnapshot = 0;

/// @note 材質・可視性・選択状態はビューごとに初期状態から解決する。
RenderFrameGeometryItem CompactGeometryItem(const renderer::RenderMeshItem& item)
{
    return {item.objectIndex, item.materialSlot, item.sourceSubmesh,
        item.vertexBuffer, item.indexBuffer, item.skinningVertexBuffer, item.deformedVertexBuffer,
        item.preparedSkinning, item.preparedSkinningWrite, item.indexCount, item.vertexCount,
        item.vertexContentVersion, item.indexContentVersion, item.deformedContentVersion,
        item.vertexStride, item.vertexPositionOffset, item.reliableOccluder, item.boundsCenter, item.boundsRadius,
        item.occluderRadius, item.clusterLod};
}

/// @pre 復元先には初期状態のビュー固有値と、復元済みのジオメトリだけが入っていること。
void RestoreGeometryItemInto(const RenderFrameGeometryItem& geometry, renderer::RenderMeshItem& item)
{
    item.objectIndex = geometry.objectIndex;
    item.materialSlot = geometry.materialSlot;
    item.sourceSubmesh = geometry.sourceSubmesh;
    item.vertexBuffer = geometry.vertexBuffer;
    item.indexBuffer = geometry.indexBuffer;
    item.skinningVertexBuffer = geometry.skinningVertexBuffer;
    item.deformedVertexBuffer = geometry.deformedVertexBuffer;
    item.preparedSkinning = geometry.preparedSkinning;
    item.preparedSkinningWrite = geometry.preparedSkinningWrite;
    item.indexCount = geometry.indexCount;
    item.vertexCount = geometry.vertexCount;
    item.vertexContentVersion = geometry.vertexContentVersion;
    item.indexContentVersion = geometry.indexContentVersion;
    item.deformedContentVersion = geometry.deformedContentVersion;
    item.vertexStride = geometry.vertexStride;
    item.vertexPositionOffset = geometry.vertexPositionOffset;
    item.reliableOccluder = geometry.reliableOccluder;
    item.boundsCenter = geometry.boundsCenter;
    item.boundsRadius = geometry.boundsRadius;
    item.occluderRadius = geometry.occluderRadius;
    item.clusterLod = geometry.clusterLod;
}

renderer::RenderMeshItem RestoreGeometryItem(const RenderFrameGeometryItem& geometry)
{
    renderer::RenderMeshItem item;
    RestoreGeometryItemInto(geometry, item);
    return item;
}

static_assert(sizeof(RenderFrameGeometryItem) < sizeof(renderer::RenderMeshItem) / 2);

/// @note キャッシュの公開元が差し替えられた場合は、以前の圧縮行や遅延取得元を引き継がない。
bool CachedGeometryItemsMatch(const RenderFrameGeometryCache& cache)
{
    const auto& geometry = cache.geometry;
    if (!geometry || cache.itemGeometryOwner != geometry.get() || cache.itemGeometrySerial == 0
        || cache.itemGeometrySerial != geometry->snapshotSerial) return false;
    const auto objects = geometry->Objects();
    const size_t itemCount = objects.empty() ? 0
        : static_cast<size_t>(objects.back().firstItem) + objects.back().itemCount;
    const auto& source = cache.deferredItemGeometry;
    if (!source) return cache.itemGeometry.size() == itemCount;
    return !cache.rayTracingInputs && cache.itemGeometry.empty()
        && source->snapshotSerial == geometry->snapshotSerial
        && source->sceneGeneration == geometry->sceneGeneration && source->frameStamp == geometry->frameStamp
        && &source->geometry.Read() == &geometry->geometry.Read() && source->items.size() == itemCount;
}

/// @note コールバックが Ray 設定を変える前に宣言した静的メッシュのレイアウトだけを捕捉する。
RenderFrameGeometryItem CompactStaticRasterGeometryItem(const renderer::RenderMeshItem& item)
{
    return {item.objectIndex, item.materialSlot, item.sourceSubmesh,
        item.vertexBuffer, item.indexBuffer, item.skinningVertexBuffer, {}, {}, UINT32_MAX,
        item.indexCount, item.vertexCount, 0, 0, 0, sizeof(renderer::Vertex), 0,
        item.reliableOccluder, item.boundsCenter, item.boundsRadius, item.occluderRadius, item.clusterLod};
}

/// @note 遅延取得は静的 Raster 入力に限り、材質・選択・後から補修した Ray 入力を取り込まない。
/// @note ビューのコールバック前に所有済みの取得元を消費する。
void MaterializeFrameGeometryItems(RenderFrameGeometryCache& cache)
{
    if (!cache.deferredItemGeometry) return;
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::GeometryCacheMaterialize");
    auto source = std::move(cache.deferredItemGeometry);
    cache.itemGeometry.reserve(source->items.size());
    for (const auto& item : source->items)
        cache.itemGeometry.push_back(CompactStaticRasterGeometryItem(item));
}

/// @note 静的 Raster のプロトタイプ再利用は明示された比較設定でのみ有効になる。
bool UseGeometryRestoreReuse()
{
    const char* mode = std::getenv("FBZZ_RENDER_GEOMETRY_RESTORE_MODE");
    return mode && std::strcmp(mode, "reuse") == 0;
}

/// @note 復元用のプロトタイプはビューのコールバック前に破棄し、材質書き込みから隔離する。
void RestoreFrameGeometryItems(const RenderFrameGeometryCache& cache, renderer::RenderScene& output)
{
    renderer::RenderMeshItem prototype;
    output.items.reserve(cache.itemGeometry.size());
    for (const auto& item : cache.itemGeometry) {
        RestoreGeometryItemInto(item, prototype);
        output.items.push_back(prototype);
    }
}

/// @note 要求中の診断フィルターが先に切り替わっても、BeginScope が記録中のフィルターを検証する。
bool CollectFrameGeometryDiagnostics()
{
    if (!profiler::Profiler::IsEnabled()) return false;
    const auto filter = profiler::Profiler::GetRequestedCaptureFilter();
    return filter.scope == "RenderSystem::FrameGeometry"
        && (filter.category.empty() || filter.category == "Rendering");
}
/// @note 材質の有効状態は同フレーム中も変化するため、現在の値を照合してから共有姿勢を再利用する。
bool CachedGeometryColorEligibilityMatches(Scene& scene, const renderer::RenderScene& geometry)
{
    std::optional<profiler::ProfileScope> eligibilityProfile;
    if (CollectFrameGeometryDiagnostics()) eligibilityProfile.emplace(
        profiler::ProfilerMarker("RenderSystem::FrameGeometry::Eligibility", "Rendering"));
    for (const auto& object : geometry.Objects()) {
        auto* go = scene.GetGameObject({ object.sourceIndex, object.sourceGeneration });
        if (!go) return false;
        const auto* component = go->GetComponent<MaterialComponent>();
        if (object.colorEligible != (!component || component->enabled)) return false;
        /// @note 風境界は姿勢通知なしのアセット編集や、未解決だったアセットの準備完了でも変化する。
        const auto* skin = object.skinned ? go->GetComponent<SkinnedMeshRenderer>() : nullptr;
        if (ComputeLeafWindBoundsPadding(*go, skin ? skin->SubmeshCount() : 1) != object.leafWindBoundsPadding) return false;
    }
    return true;
}

/// @note 同じ実行物で比較するため、設定なしと未知の値は従来の走査を選ぶ。
bool UseFusedExtractionScans()
{
    const char* mode = std::getenv("FBZZ_RENDER_EXTRACTION_SCAN_MODE");
    const bool fused = mode && std::strcmp(mode, "fused") == 0;
    static std::atomic<bool> configurationLogged{false};
    if (mode && !configurationLogged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "RENDER_EXTRACTION_SCAN_CONFIG mode=%s\n", fused ? "fused" : "legacy");
    return fused;
}

FrameGeometryView RestoreCachedFrameGeometry(RenderFrameGeometryCache& cache, bool fusedScans, bool rayTracingInputs)
{
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::CachedGeometryInputs");
    const bool reusePrototype = fusedScans && !rayTracingInputs
        && cache.skinnedObjectRangeOwner == cache.geometry.get()
        && cache.skinnedObjectRangeSerial == cache.geometry->snapshotSerial
        && cache.skinnedObjectBegin == cache.skinnedObjectEnd && UseGeometryRestoreReuse();
    MaterializeFrameGeometryItems(cache);
    auto snapshot = MakeRenderSnapshot(SnapshotBuffers::ITEMS);
    ShareFrameGeometry(*snapshot, cache.geometry);
    snapshot->shadowBakeMeshObjects = cache.geometry->shadowBakeMeshObjects;
    if (reusePrototype) {
        RestoreFrameGeometryItems(cache, *snapshot);
    } else {
        snapshot->items.reserve(cache.itemGeometry.size());
        for (const auto& item : cache.itemGeometry)
            snapshot->items.push_back(RestoreGeometryItem(item));
    }
    snapshot->rayLodDiagnostics = cache.geometry->rayLodDiagnostics;
    snapshot->sceneGeneration = cache.geometry->sceneGeneration;
    snapshot->frameStamp = cache.geometry->frameStamp;
    snapshot->snapshotSerial = NextRenderSnapshotSerial();
    if (fusedScans && cache.skinnedObjectRangeOwner == cache.geometry.get()
        && cache.skinnedObjectRangeSerial != 0
        && cache.skinnedObjectRangeSerial == cache.geometry->snapshotSerial) {
        return { std::move(snapshot), SkinnedObjectRange{ cache.skinnedObjectBegin, cache.skinnedObjectEnd } };
    }
    return { std::move(snapshot), std::nullopt };
}

void PublishFrameGeometry(RenderFrameGeometryCache& cache, const RenderPassContext& context,
    const std::shared_ptr<renderer::RenderScene>& snapshot, bool rayTracingInputs, uint64_t resourceResetVersion,
    uint64_t staticGeometryRevision, const std::optional<SkinnedObjectRange>& skinnedObjects)
{
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::GeometryCachePublish");
    cache.scene = &context.scene;
    cache.resources = &context.resources;
    cache.frameStamp = Time::frameCount;
    cache.resetVersion = resourceResetVersion;
    cache.staticGeometryRevision = staticGeometryRevision;
    cache.rayTracingInputs = rayTracingInputs;
    cache.identityPalette = context.handles.bindPoseSkinningCB;
    auto geometry = PublishObjectGeometry(*snapshot);
    geometry->shadowBakeMeshObjects = snapshot->shadowBakeMeshObjects;
    cache.itemGeometry.clear();
    cache.deferredItemGeometry.reset();
    /// @note スキン入力と Ray メタデータは材質コールバックの前に確定する。
    if (skinnedObjects && !rayTracingInputs && skinnedObjects->begin == skinnedObjects->end && !snapshot->items.empty()) {
        cache.deferredItemGeometry = snapshot;
    } else {
        cache.itemGeometry.reserve(snapshot->items.size());
        for (const auto& item : snapshot->items)
            cache.itemGeometry.push_back(CompactGeometryItem(item));
    }
    geometry->rayLodDiagnostics = snapshot->rayLodDiagnostics;
    geometry->snapshotSerial = snapshot->snapshotSerial;
    geometry->sceneGeneration = snapshot->sceneGeneration;
    geometry->frameStamp = snapshot->frameStamp;
    cache.itemGeometryOwner = geometry.get();
    cache.itemGeometrySerial = geometry->snapshotSerial;
    cache.skinnedObjectRangeOwner = skinnedObjects ? geometry.get() : nullptr;
    cache.skinnedObjectRangeSerial = skinnedObjects ? geometry->snapshotSerial : 0;
    cache.skinnedObjectBegin = skinnedObjects ? skinnedObjects->begin : 0;
    cache.skinnedObjectEnd = skinnedObjects ? skinnedObjects->end : 0;
    cache.geometry = std::move(geometry);
}

}

uint64_t NextRenderSnapshotSerial()
{
    return ++s_renderSceneSerial;
}

void RetainSpatialUpdateForCompletion(renderer::RenderScene& snapshot)
{
    /// @note 後段で材質・スキニングを補完する場合も、元のビューと同じ形状に対して検証した差分を保持する。
    if (!snapshot.geometry.IsShared() && snapshot.geometry.SpatialUpdate()) PublishObjectGeometry(snapshot);
}

StaticSceneGeometry* FindStaticGeometry(Scene& scene, bool createIfMissing)
{
    for (auto& cache : s_staticGeometry)
        if (cache.scene == &scene && cache.generation == scene.GetRenderSceneGeneration()) return &cache;
    if (!createIfMissing) return nullptr;
    auto& cache = s_staticGeometry[s_nextStaticGeometry];
    s_nextStaticGeometry = (s_nextStaticGeometry + 1) % s_staticGeometry.size();
    cache = {};
    cache.scene = &scene;
    cache.generation = scene.GetRenderSceneGeneration();
    cache.revision = ++s_staticGeometryRevision;
    return &cache;
}

PreparedSnapshotLookup FindPreparedSnapshotItems(const std::shared_ptr<const renderer::RenderScene>& snapshot)
{
    for (size_t offset = 0; offset < s_preparedSnapshotItems.size(); ++offset) {
        const size_t index = (s_nextPreparedSnapshot + s_preparedSnapshotItems.size() - 1 - offset) % s_preparedSnapshotItems.size();
        const auto& entry = s_preparedSnapshotItems[index];
        if (entry.identity == snapshot.get() && !entry.owner.expired()) return {true, entry.indices};
    }
    return {};
}

/// @note null の索引は既知の空集合。弱参照はスナップショットや GPU チケットの寿命を延ばさない。
void RememberPreparedSnapshotItems(const std::shared_ptr<const renderer::RenderScene>& snapshot,
    std::shared_ptr<const std::vector<uint32_t>> indices)
{
    s_preparedSnapshotItems[s_nextPreparedSnapshot] = {snapshot.get(), snapshot, std::move(indices)};
    s_nextPreparedSnapshot = (s_nextPreparedSnapshot + 1) % s_preparedSnapshotItems.size();
}

std::shared_ptr<const std::vector<uint32_t>> OwnPreparedSnapshotItems(std::vector<uint32_t> indices)
{
    return indices.empty() ? nullptr : std::make_shared<const std::vector<uint32_t>>(std::move(indices));
}

void RecordPreparedSnapshotItems(const std::shared_ptr<const renderer::RenderScene>& snapshot,
    std::vector<uint32_t> preparedItemIndices)
{
    RememberPreparedSnapshotItems(snapshot, OwnPreparedSnapshotItems(std::move(preparedItemIndices)));
}

FrameGeometryView AcquireFrameGeometry(RenderPassContext& context, RenderFrameGeometryCache* frameCache,
    bool rayTracingInputs, uint64_t resourceResetVersion)
{
    const bool fusedScans = UseFusedExtractionScans();
    const auto* staticGeometry = FindStaticGeometry(context.scene, false);
    const uint64_t staticGeometryRevision = staticGeometry ? staticGeometry->revision : 0;
    if (staticGeometry && frameCache && frameCache->geometry && frameCache->scene == &context.scene &&
        frameCache->geometry->sceneGeneration == context.scene.GetRenderSceneGeneration() &&
        frameCache->resources == &context.resources && frameCache->frameStamp == Time::frameCount &&
        frameCache->resetVersion == resourceResetVersion &&
        frameCache->staticGeometryRevision == staticGeometryRevision &&
        frameCache->rayTracingInputs == rayTracingInputs &&
        frameCache->identityPalette == context.handles.bindPoseSkinningCB &&
        CachedGeometryItemsMatch(*frameCache) &&
        CachedGeometryColorEligibilityMatches(context.scene, *frameCache->geometry)) {
        return RestoreCachedFrameGeometry(*frameCache, fusedScans, rayTracingInputs);
    }

    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::GeometryBuild");
    uint64_t builtStaticRevision = 0;
    auto snapshot = MakeRenderSnapshot(SnapshotBuffers::ITEMS_AND_OBJECTS);
    std::optional<SkinnedObjectRange> skinnedObjects;
    if (fusedScans) skinnedObjects.emplace();
    const GeometryBuildOptions options{
        .resources = &context.resources,
        .rayTracingInputs = rayTracingInputs,
        .extractItemMaterialState = false,
        .fuseColorEligibility = fusedScans,
    };
    *snapshot = BuildRenderSceneGeometry(context.scene, Time::frameCount, context.handles.bindPoseSkinningCB,
        options, &builtStaticRevision, snapshot.get(), skinnedObjects ? &*skinnedObjects : nullptr);
    if (frameCache) {
        PublishFrameGeometry(*frameCache, context, snapshot, rayTracingInputs, resourceResetVersion,
            builtStaticRevision, skinnedObjects);
    }
    return { std::move(snapshot), skinnedObjects };
}

}

using namespace render_extraction_detail;

void NotifyMeshRendererChanged(Scene& scene, EntityID entity)
{
    if (!entity.IsValid()) return;
    if (auto* cache = FindStaticGeometry(scene, false)) cache->revision = ++s_staticGeometryRevision;
}

std::shared_ptr<const renderer::RenderScene> CompletePreparedSkinningSnapshot(RenderPassContext& context)
{
    if (!context.renderScene) return {};
    const auto findCurrentWrite = [](const renderer::RenderMeshItem& item) {
        if (!item.preparedSkinning) return renderer::PreparedSkinningWrite{};
        const auto writes = item.preparedSkinning->Writes();
        return item.preparedSkinningWrite < writes.size() && item.preparedSkinning->IsWriteCurrent(item.preparedSkinningWrite) ? writes[item.preparedSkinningWrite]
            : renderer::PreparedSkinningWrite{};
    };
    const auto cachedPreparedItems = FindPreparedSnapshotItems(context.renderScene);
    if (cachedPreparedItems.known && !cachedPreparedItems.indices) return {};
    std::vector<uint32_t> discoveredPreparedItems;
    if (!cachedPreparedItems.known) {
        for (size_t itemIndex = 0; itemIndex < context.renderScene->items.size(); ++itemIndex)
            if (context.renderScene->items[itemIndex].preparedSkinning) discoveredPreparedItems.push_back(static_cast<uint32_t>(itemIndex));
    }
    const std::span<const uint32_t> preparedItemIndices = cachedPreparedItems.known ? std::span<const uint32_t>(*cachedPreparedItems.indices)
        : std::span<const uint32_t>(discoveredPreparedItems);
    bool hasChangedOutput = false;
    for (uint32_t itemIndex : preparedItemIndices) {
        const auto& item = context.renderScene->items[itemIndex];
        const auto write = findCurrentWrite(item);
        if (item.deformedVertexBuffer != (write.recorded ? write.output : renderer::ResourceHandle<renderer::BufferTag>{})
            || item.deformedContentVersion != (write.recorded ? write.resultingVersion : 0)) { hasChangedOutput = true; break; }
    }
    if (!hasChangedOutput) return {};
    auto previous = context.renderScene;
    auto completed = std::make_shared<renderer::RenderScene>(*previous);
    completed->snapshotSerial = NextRenderSnapshotSerial();
    for (uint32_t itemIndex : preparedItemIndices) {
        auto& item = completed->items[itemIndex];
        const auto write = findCurrentWrite(item);
        item.deformedVertexBuffer = write.recorded ? write.output : renderer::ResourceHandle<renderer::BufferTag>{};
        item.deformedContentVersion = write.recorded ? write.resultingVersion : 0;
    }
    RememberPreparedSnapshotItems(completed, cachedPreparedItems.known ? cachedPreparedItems.indices : OwnPreparedSnapshotItems(std::move(discoveredPreparedItems)));
    /// @note ここで差し替えるのは変形出力だけなので、元の公開値の選択添字を新しい公開値へ引き継げる。
    if (const auto selected = previous->PublishedMeshSelection())
        completed->selection.Publish({selected->begin(), selected->end()});
    context.renderScene = completed;
    material_demand_detail::RetargetBindings(previous, completed);
    return previous;
}

}
