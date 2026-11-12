/// @file    RenderSceneExtractor.cpp
/// @brief   更新済み Scene のメッシュ・姿勢・材質・GPU 資源を描画入力へ固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include "MaterialDemandTrace.hpp"
#include <Graphics/Renderer/MaterialTransferScope.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <Core/Profiler/ProfileScope.hpp>
#include <Graphics/Renderer/BuiltinShader.hpp>
#include <Engine/Scene/Systems/RenderCustomPostExtractor.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Systems/RenderEnvironmentExtractor.hpp>
#include <Engine/Scene/Systems/RenderTrailExtractor.hpp>
#include <Engine/Scene/Systems/RenderDecalExtractor.hpp>
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/Systems/RenderFiberExtractor.hpp>
#include <Engine/Scene/Systems/RenderMeshTrailExtractor.hpp>
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderPasses/Debug/SelectionPasses.hpp"
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include <atomic>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {
namespace material_demand_detail {
namespace {
thread_local const RenderPassContext* s_bindingDemandContext = nullptr;
thread_local BindingDemandScope* s_bindingDemandScope = nullptr;
thread_local uint32_t s_bindingDemandDepth = 0;
thread_local uint32_t s_extractionDepth = 0;
thread_local uint32_t s_bindingCompletionDepth = 0;
}

/// @note Exact decision inputs exclude padding and pointer identity; copy frustum plane values to observe in-place edits.
using BindingDemandWitness = std::array<uint32_t, 44 + math::Frustum::PLANE_COUNT * 4>;

BindingDemandWitness CaptureBindingDemandWitness(const View& view)
{
    BindingDemandWitness result{};
    size_t offset = 0;
    const auto put = [&](float value) { result[offset++] = std::bit_cast<uint32_t>(value); };
    for (const auto& vector : {view.culling.position, view.culling.forward}) {
        put(vector.x); put(vector.y); put(vector.z);
    }
    put(view.culling.projectionScaleY); put(view.culling.smallObjectScreenHeight);
    put(view.maxDistance); put(view.boundsPadding);
    for (float distance : view.layerDistances) put(distance);
    result[offset++] = view.mask;
    result[offset++] = static_cast<uint32_t>(view.culling.frustum != nullptr)
        | (static_cast<uint32_t>(view.culling.distanceSpherical) << 1)
        | (static_cast<uint32_t>(view.culling.orthographic) << 2)
        | (static_cast<uint32_t>(view.hasLayerDistances) << 3)
        | (static_cast<uint32_t>(view.staticGpuFrustumPossible) << 4)
        | (static_cast<uint32_t>(view.directionalShadowPotential) << 5)
        | (static_cast<uint32_t>(view.punctualShadowPotential) << 6)
        | (static_cast<uint32_t>(view.reflectionProbePotential) << 7)
        | (static_cast<uint32_t>(view.lightProbePotential) << 8)
        | (static_cast<uint32_t>(view.rayPotential) << 9)
        | (static_cast<uint32_t>(view.consumerScopeCertified) << 10)
        | (static_cast<uint32_t>(view.materialCoverageKnown) << 11);
    if (view.culling.frustum) {
        for (int index = 0; index < math::Frustum::PLANE_COUNT; ++index) {
            const auto plane = view.culling.frustum->GetPlane(index);
            put(plane.normal.x); put(plane.normal.y); put(plane.normal.z); put(plane.distance);
        }
    }
    return result;
}

struct PendingBindings {
    std::shared_ptr<const renderer::RenderScene> snapshot;
    std::vector<uint32_t> items;
    uint64_t resourceOwner = 0, resourceEpoch = 0, resourceFrame = 0;
    int assetGeneration = 0;
    std::optional<BindingDemandWitness> noDemandWitness;
};

BindingDemandScope::BindingDemandScope(const RenderPassContext& context) : m_previous(s_bindingDemandContext)
{
    m_previousScope = s_bindingDemandScope;
    s_bindingDemandScope = this;
    s_bindingDemandContext = &context;
    ++s_bindingDemandDepth;
}

BindingDemandScope::~BindingDemandScope()
{
    s_bindingDemandScope = m_previousScope;
    s_bindingDemandContext = m_previous;
    --s_bindingDemandDepth;
}

class ExtractionDepthScope {
public:
    ExtractionDepthScope() { ++s_extractionDepth; }
    ~ExtractionDepthScope() { --s_extractionDepth; }
};

class CompletionDepthScope {
public:
    CompletionDepthScope() { ++s_bindingCompletionDepth; }
    ~CompletionDepthScope() { --s_bindingCompletionDepth; }
};

bool BindingScopeCertified(const RenderPassContext& context, bool completing = false)
{
    return s_bindingDemandContext == &context && s_bindingDemandDepth == 1 && s_bindingCompletionDepth == 0
        && (s_extractionDepth == 1 || (completing && s_extractionDepth == 0));
}

void RetainPendingBindings(const RenderPassContext& context, std::shared_ptr<const renderer::RenderScene> snapshot,
                           std::vector<uint32_t> items)
{
    if (!BindingScopeCertified(context) || !s_bindingDemandScope || items.empty()) return;
    auto pending = std::make_unique<PendingBindings>();
    pending->snapshot = std::move(snapshot);pending->items = std::move(items);
    pending->resourceOwner = context.resources.GetInstanceId();pending->resourceEpoch = context.resources.GetResetVersion();
    pending->resourceFrame = context.resources.FrameStamp();pending->assetGeneration = asset::AssetManager::GetAssetGeneration();
    s_bindingDemandScope->m_pending = std::move(pending);
}

void RetargetBindings(const std::shared_ptr<const renderer::RenderScene>& previous,
    const std::shared_ptr<const renderer::RenderScene>& completed)
{
    if (s_bindingDemandScope && s_bindingDemandScope->m_pending
        && s_bindingDemandScope->m_pending->snapshot == previous)
    {
        s_bindingDemandScope->m_pending->snapshot = completed;
        s_bindingDemandScope->m_pending->noDemandWitness.reset();
    }
}
} /// @note namespace material_demand_detail

namespace {

std::atomic<uint64_t> s_renderSceneSerial{ 0 };

/// @note Retain empty allocation capacity only: two item buffers up to 16 MiB each and two object buffers up to 8 MiB each per extraction thread.
class RenderGeometryStorage {
public:
    static constexpr size_t MAXIMUM_OBJECT_BYTES = 8u * 1024u * 1024u;
    void Borrow(renderer::RenderScene& output)
    {
        const std::lock_guard lock(m_mutex);
        Take(m_items, output.items);
        Take(m_objects, output.objects);
    }
    void RecycleItems(std::vector<renderer::RenderMeshItem>&& items)
    {
        /// @note Release skinning tickets before taking the pool lock; an idle pool never extends native resource ownership.
        items.clear();
        Return(m_items, std::move(items), 16u * 1024u * 1024u);
    }
    void RecycleObjects(std::vector<renderer::RenderObject>&& objects)
    {
        objects.clear();
        Return(m_objects, std::move(objects), MAXIMUM_OBJECT_BYTES);
    }
private:
    template<typename T>
    static void Take(std::array<std::vector<T>, 2>& buffers, std::vector<T>& output)
    {
        auto* selected = &buffers.front();
        for (auto& buffer : buffers)
            if (buffer.capacity() > selected->capacity()) selected = &buffer;
        output.swap(*selected);
    }
    template<typename T>
    void Return(std::array<std::vector<T>, 2>& buffers, std::vector<T>&& values, size_t maximumBytes)
    {
        if (values.capacity() == 0 || values.capacity() > maximumBytes / sizeof(T)) return;
        /// @note The last immutable owner may retire on a render worker; it returns storage to its origin pool rather than that worker's TLS.
        const std::lock_guard lock(m_mutex);
        for (auto& buffer : buffers) {
            if (buffer.capacity() != 0) continue;
            buffer = std::move(values);
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
    explicit RenderSnapshotLease(std::shared_ptr<RenderGeometryStorage> owner) : storage(std::move(owner))
    {
        storage->Borrow(snapshot);
    }
    ~RenderSnapshotLease()
    {
        storage->RecycleItems(std::move(snapshot.items));
        storage->RecycleObjects(std::move(snapshot.objects));
    }
    std::shared_ptr<RenderGeometryStorage> storage;
    renderer::RenderScene snapshot;
};

std::shared_ptr<renderer::RenderScene> MakeRenderSnapshot()
{
    auto lease = std::make_shared<RenderSnapshotLease>(GeometryStorage());
    auto* snapshot = &lease->snapshot;
    /// @note Aliasing ownership recycles storage only after the last const snapshot owner retires, including nested extraction and retained views.
    /// @see https://eel.is/c++draft/util.smartptr.shared.const Aliasing shared_ptr constructors preserve the ownership group's lifetime.
    return std::shared_ptr<renderer::RenderScene>(std::move(lease), snapshot);
}

struct RenderObjectLease {
    RenderObjectLease(std::shared_ptr<RenderGeometryStorage> owner, std::vector<renderer::RenderObject>&& values)
        : storage(std::move(owner)), objects(std::move(values)) {}
    ~RenderObjectLease() { storage->RecycleObjects(std::move(objects)); }
    std::shared_ptr<RenderGeometryStorage> storage;
    std::vector<renderer::RenderObject> objects;
};

std::shared_ptr<const std::vector<renderer::RenderObject>> FreezeRenderObjects(std::vector<renderer::RenderObject>&& objects)
{
    /// @note Oversized snapshots keep the original owning allocation; their capacity is never retained by the bounded pool.
    if (objects.capacity() > RenderGeometryStorage::MAXIMUM_OBJECT_BYTES / sizeof(renderer::RenderObject))
        return std::make_shared<const std::vector<renderer::RenderObject>>(std::move(objects));
    auto lease = std::make_shared<RenderObjectLease>(GeometryStorage(), std::move(objects));
    const auto* values = &lease->objects;
    return std::shared_ptr<const std::vector<renderer::RenderObject>>(std::move(lease), values);
}

/// @note 候補収集から姿勢抽出までは callback を呼ばず、終了時に ID を消してスレッド別の容量だけを保持する。
thread_local std::vector<EntityID> s_renderGeometryCandidates;

/// @note Only values cross frames; Scene identity includes its lifetime generation and component replacement includes its lifetime token.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine Cached inputs and resource lifetime.
struct StaticGeometryEntry {
    EntityID entity;
    uint64_t lifetime = 0;
    uint64_t lastExtractFrame = UINT64_MAX, lastPoseChangeFrame = UINT64_MAX;
    const renderer::Mesh* mesh = nullptr;
    std::array<uint32_t, 10> pose{};
    std::array<uint32_t, 14> meshInputs{};
    math::Matrix4 world, inverseTranspose;
    math::Vector3 center{};
    float radius = 0;
    float windPadding = 0, occluderRadius = 0;
    bool reliable = false, valid = false;
};
struct SpatialObjectIdentity {
    EntityID entity;
    uint64_t lifetime = 0;
    bool skinned = false;
    bool operator==(const SpatialObjectIdentity&) const = default;
};
struct StaticSceneGeometry {
    const Scene* scene = nullptr;
    uint64_t generation = 0, revision = 0;
    bool primed = false;
    bool highChangeLastBuild = false;
    uint32_t denseBuildsRemaining = 0;
    std::vector<StaticGeometryEntry> entries;
    uint64_t spatialVersion = 0;
    std::vector<SpatialObjectIdentity> spatialObjects;
};
/// @note Geometry traversal is callback-free; release every reference before material extraction can reenter or evict a slot.
thread_local std::array<StaticSceneGeometry, 4> s_staticGeometry;
thread_local size_t s_nextStaticGeometry = 0;
thread_local uint64_t s_staticGeometryRevision = 0;
std::atomic<uint64_t> s_spatialUpdateVersion{0};

uint64_t NextSpatialUpdateVersion()
{
    auto version = s_spatialUpdateVersion.load(std::memory_order_relaxed);
    while (version != UINT64_MAX) {
        if (s_spatialUpdateVersion.compare_exchange_weak(version, version + 1, std::memory_order_relaxed))
            return version + 1;
    }
    return 0;
}

/// @note Bounds dirtiness comes from the existing static witness/rebuild path; skinned and uncached bounds remain conservatively dirty.
/// @note Identity checks are fused with mandatory object append; no second Objects projection or sphere-witness traversal is introduced.
class SpatialUpdateBuilder {
public:
    explicit SpatialUpdateBuilder(StaticSceneGeometry& cache) : m_cache(cache)
    {
        const char* mode = std::getenv("FBZZ_MESH_SPATIAL_PRODUCER");
        if (!mode || std::strcmp(mode, "journal") != 0) {
            m_cache.spatialVersion = 0;
            m_cache.spatialObjects.clear();
            return;
        }
        const uint64_t version = NextSpatialUpdateVersion();
        if (version == 0) {
            m_cache.spatialVersion = 0;
            m_cache.spatialObjects.clear();
            return;
        }
        m_update = std::make_shared<renderer::RenderSpatialUpdate>();
        m_update->version = version;
        m_update->baseVersion = m_cache.spatialVersion;
        m_update->fullReplacement = m_cache.spatialVersion == 0;
    }

    bool Enabled() const { return m_update != nullptr; }

    void Append(EntityID entity, uint64_t lifetime, bool skinned, bool boundsChanged)
    {
        if (!m_update) return;
        const size_t ordinal = m_update->objectCount++;
        const SpatialObjectIdentity identity{entity, lifetime, skinned};
        if (ordinal == m_cache.spatialObjects.size()) {
            m_cache.spatialObjects.push_back(identity);
            m_update->fullReplacement = true;
        } else if (m_cache.spatialObjects[ordinal] != identity) {
            m_cache.spatialObjects[ordinal] = identity;
            m_update->fullReplacement = true;
        }
        if (boundsChanged && ordinal <= UINT32_MAX)
            m_update->changedObjectOrdinals.push_back(static_cast<uint32_t>(ordinal));
    }

    void Publish(renderer::RenderScene& output)
    {
        if (!m_update) return;
        if (m_update->objectCount != m_cache.spatialObjects.size()) {
            m_cache.spatialObjects.resize(m_update->objectCount);
            m_update->fullReplacement = true;
        }
        if (m_update->objectCount > UINT32_MAX) {
            m_cache.spatialVersion = 0;
            return;
        }
        if (m_update->fullReplacement) m_update->baseVersion = 0;
        m_cache.spatialVersion = m_update->version;
        output.spatialUpdate = std::move(m_update);
    }
private:
    StaticSceneGeometry& m_cache;
    std::shared_ptr<renderer::RenderSpatialUpdate> m_update;
};

StaticSceneGeometry* FindStaticGeometry(Scene& scene, bool create)
{
    for (auto& cache : s_staticGeometry)
        if (cache.scene == &scene && cache.generation == scene.GetRenderSceneGeneration()) return &cache;
    if (!create) return nullptr;
    auto& cache = s_staticGeometry[s_nextStaticGeometry];
    s_nextStaticGeometry = (s_nextStaticGeometry + 1) % s_staticGeometry.size();
    cache = {};
    cache.scene = &scene;
    cache.generation = scene.GetRenderSceneGeneration();
    cache.revision = ++s_staticGeometryRevision;
    return &cache;
}

std::array<uint32_t, 10> StaticPoseInputs(const Transform& transform)
{
    const auto& p = transform.worldPosition;
    const auto& q = transform.worldRotation;
    const auto& s = transform.worldScale;
    return std::bit_cast<std::array<uint32_t, 10>>(std::array<float, 10>{p.x, p.y, p.z, q.x, q.y, q.z, q.w, s.x, s.y, s.z});
}

std::array<uint32_t, 14> StaticMeshInputs(const renderer::Mesh& mesh)
{
    const auto& c = mesh.boundsCenter;
    const auto& e = mesh.boundsExtents;
    return {mesh.vertexBuffer.id, mesh.vertexBuffer.gen, mesh.indexBuffer.id, mesh.indexBuffer.gen,
        mesh.vertexCount, mesh.indexCount, std::bit_cast<uint32_t>(c.x), std::bit_cast<uint32_t>(c.y),
        std::bit_cast<uint32_t>(c.z), std::bit_cast<uint32_t>(mesh.boundsRadius),
        std::bit_cast<uint32_t>(e.x), std::bit_cast<uint32_t>(e.y), std::bit_cast<uint32_t>(e.z),
        std::bit_cast<uint32_t>(mesh.occlusionRadius)};
}

/// @note Only internally published immutable snapshots use this bounded index; unknown or evicted snapshots retain full discovery.
struct PreparedSnapshotItems {
    const renderer::RenderScene* identity = nullptr;
    std::weak_ptr<const renderer::RenderScene> owner;
    std::shared_ptr<const std::vector<uint32_t>> indices;
};
struct PreparedSnapshotLookup {
    bool known = false;
    std::shared_ptr<const std::vector<uint32_t>> indices;
};
thread_local std::array<PreparedSnapshotItems, 32> s_preparedSnapshotItems;
thread_local size_t s_nextPreparedSnapshot = 0;

PreparedSnapshotLookup FindPreparedSnapshotItems(const std::shared_ptr<const renderer::RenderScene>& snapshot)
{
    for (size_t offset = 0; offset < s_preparedSnapshotItems.size(); ++offset) {
        const size_t index = (s_nextPreparedSnapshot + s_preparedSnapshotItems.size() - 1 - offset) % s_preparedSnapshotItems.size();
        const auto& entry = s_preparedSnapshotItems[index];
        if (entry.identity == snapshot.get() && !entry.owner.expired()) return {true, entry.indices};
    }
    return {};
}

/// @note A null index is a known empty set, not missing metadata; weak ownership cannot extend GPU ticket or snapshot lifetimes.
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

material_demand_detail::View CurrentMaterialDemandView(const RenderPassContext& ctx, bool rayInputs, bool completing = false)
{
    material_demand_detail::View view;
    view.culling = {ctx.camera.m_position, ctx.cullCameraForward,
        ctx.frustumCullingEnabled ? ctx.cameraFrustum : nullptr,
        ctx.cullProjScaleY, ctx.smallObjectScreenHeight, ctx.cullDistanceSpherical, ctx.cullOrthographic};
    std::copy(std::begin(ctx.cullLayerDistances), std::end(ctx.cullLayerDistances), view.layerDistances.begin());
    view.maxDistance = ctx.cullMaxDistance;
    view.boundsPadding = ctx.cullingBoundsPadding;
    view.mask = ctx.cullingMask;
    view.hasLayerDistances = ctx.hasLayerCullDistances;
    view.staticGpuFrustumPossible = ctx.settings.gpuInstancing
        && (ctx.settings.geometryDrawMode == renderer::GeometryDrawMode::GPU_INDIRECT
            || ctx.settings.geometryDrawMode == renderer::GeometryDrawMode::GPU_INSTANCE_INDIRECT);
    /// @note Failed/custom materials can become opaque ErrorSurface during synchronization; preserve every potential shadow consumer.
    view.directionalShadowPotential = ctx.settings.shadowEnabled;
    view.punctualShadowPotential = ctx.settings.shadowEnabled && ctx.punctualShadowViewCount > 0;
    view.rayPotential = rayInputs;
    view.consumerScopeCertified = material_demand_detail::BindingScopeCertified(ctx, completing)
        && ctx.scene.GetUserRenderPasses().empty() && !ctx.objectMaskEnabled
        && ctx.settings.objectMaskRequests.empty() && ctx.settings.selectedObjects.empty();
    /// @note Consumer boundaries repeat this census after resource callbacks; an empty pass list cannot rule out a native setup hook.
    /// @note ManagedScriptAdapter is final and does not override native render setup/pre/post hooks.
    if (view.consumerScopeCertified) {
        for (EntityID id : ctx.scene.GetEntities<ScriptComponent>()) {
            const auto* scripts = ctx.scene.GetComponent<ScriptComponent>(id);
            if (!scripts) continue;
            for (const auto& entry : scripts->scripts) {
                if (entry.script && !ManagedScriptRuntime::Instance().FindAdapter(entry.script.get())) {
                    view.consumerScopeCertified = false;
                    break;
                }
            }
            if (!view.consumerScopeCertified) break;
        }
    }
    for (EntityID id : ctx.scene.GetEntities<ReflectionProbeComponent>()) {
        const auto* object = ctx.scene.GetGameObject(id);
        const auto* probe = ctx.scene.GetComponent<ReflectionProbeComponent>(id);
        if (object && object->activeInHierarchy() && probe && probe->enabled
            && probe->captureMode == ReflectionProbeCaptureMode::DynamicScene) {
            view.reflectionProbePotential = true;
            break;
        }
    }
    for (EntityID id : ctx.scene.GetEntities<LightProbeVolumeComponent>()) {
        const auto* object = ctx.scene.GetGameObject(id);
        const auto* volume = ctx.scene.GetComponent<LightProbeVolumeComponent>(id);
        if (object && object->activeInHierarchy() && volume && volume->enabled) {
            view.lightProbePotential = true;
            break;
        }
    }
    return view;
}

/// @note Independent bounded count capture; disabled extraction never constructs the row store or scans the Scene.
struct MaterialDemandTraceConfig {
    std::string path;
    uint32_t warmup = 120;
    uint32_t samples = 240;
    uint64_t requestedSamples = 240;
    MaterialDemandTraceConfig()
    {
        if (const char* value = std::getenv("FBZZ_MATERIAL_DEMAND_TRACE_OUTPUT")) path = value;
        if (const char* value = std::getenv("FBZZ_MATERIAL_DEMAND_TRACE_WARMUP"))
            warmup = static_cast<uint32_t>((std::min)(std::strtoul(value, nullptr, 10), 100000ul));
        if (const char* value = std::getenv("FBZZ_MATERIAL_DEMAND_TRACE_SAMPLES")) {
            const auto requested = std::strtoul(value, nullptr, 10);
            requestedSamples = requested > 0 ? requested : 240;
            samples = static_cast<uint32_t>((std::min)(requestedSamples, uint64_t{512}));
        }
    }
};

const MaterialDemandTraceConfig& MaterialDemandConfig()
{
    static const MaterialDemandTraceConfig config;
    return config;
}

struct MaterialDemandCaptureData;
void WriteMaterialDemandCapture(const MaterialDemandCaptureData& capture);

struct MaterialDemandCaptureData {
    static constexpr uint32_t CAPACITY = 512;
    struct Ordinal {
        uint64_t owner = 0, epoch = 0, frame = 0, value = 0;
    };
    std::mutex mutex;
    std::array<material_demand_detail::Row, CAPACITY> rows{};
    std::array<Ordinal, 64> ordinals{};
    uint32_t warmed = 0, reserved = 0, finished = 0, activeScopes = 0;
    bool sealed = false, writeStarted = false;
    ~MaterialDemandCaptureData()
    {
        /// @note Shutdown exports a partial capture only when no extraction scope can still write a row.
        bool exportReady = false;
        {
            std::lock_guard lock(mutex);
            if (writeStarted) return;
            sealed = true;
            writeStarted = true;
            exportReady = activeScopes == 0;
        }
        if (!exportReady) {
            std::fprintf(stderr, "MaterialDemandTrace: incomplete capture has active extraction scopes; export withheld.\n");
            return;
        }
        WriteMaterialDemandCapture(*this);
    }
};

MaterialDemandCaptureData& MaterialDemandCapture()
{
    static MaterialDemandCaptureData capture;
    return capture;
}

void WriteMaterialDemandCapture(const MaterialDemandCaptureData& capture)
{
    /// @note Capture is sealed and all scopes finished; no lock is held and no future row mutation is admitted.
    const auto& config = MaterialDemandConfig();
    std::ofstream csv(config.path + ".material-demand.csv", std::ios::binary);
    bool first = true;
#define FBZZ_MATERIAL_DEMAND_HEADER(name) if (!first) csv << ','; first = false; csv << #name;
    FBZZ_MATERIAL_DEMAND_COLUMNS(FBZZ_MATERIAL_DEMAND_HEADER)
#undef FBZZ_MATERIAL_DEMAND_HEADER
    csv << '\n';
    uint32_t incomplete = 0, unknownOrdinals = 0, changedIdentities = 0;
    for (uint32_t index = 0; index < capture.reserved; ++index) {
        const auto& row = capture.rows[index];
        if (row.complete == 0) ++incomplete;
        if (row.ordinalKnown == 0) ++unknownOrdinals;
        if (row.identityChangedDuringInvocation != 0) ++changedIdentities;
        first = true;
#define FBZZ_MATERIAL_DEMAND_VALUE(name) if (!first) csv << ','; first = false; csv << row.name;
        FBZZ_MATERIAL_DEMAND_COLUMNS(FBZZ_MATERIAL_DEMAND_VALUE)
#undef FBZZ_MATERIAL_DEMAND_VALUE
        csv << '\n';
    }
    csv.flush();
    csv.close();
    const bool csvWritten = csv.good();
    const bool capacityLimited = config.requestedSamples > MaterialDemandCaptureData::CAPACITY;
    const bool completed = csvWritten && capture.reserved == config.samples && incomplete == 0
        && unknownOrdinals == 0 && changedIdentities == 0 && !capacityLimited;
    std::ofstream status(config.path + ".material-demand.status.json", std::ios::binary);
    status << "{\n  \"complete\":" << (completed ? "true" : "false")
        << ",\n  \"csvWritten\":" << (csvWritten ? "true" : "false")
        << ",\n  \"rowCapacity\":" << MaterialDemandCaptureData::CAPACITY
        << ",\n  \"requestedInvocations\":" << config.requestedSamples
        << ",\n  \"sampleInvocations\":" << config.samples
        << ",\n  \"warmupInvocations\":" << config.warmup
        << ",\n  \"capturedRows\":" << capture.reserved
        << ",\n  \"finishedRows\":" << capture.finished
        << ",\n  \"incompleteRows\":" << incomplete
        << ",\n  \"droppedRows\":0"
        << ",\n  \"capacityLimited\":" << (capacityLimited ? "true" : "false")
        << ",\n  \"unknownOrdinalRows\":" << unknownOrdinals
        << ",\n  \"identityChangedRows\":" << changedIdentities
        << ",\n  \"activeScopesAtExport\":0,\n  \"sealed\":true\n}\n";
    status.flush();
    status.close();
    if (!csvWritten || !status.good())
        std::fprintf(stderr, "MaterialDemandTrace: CSV/status write failed for configured output.\n");
}

thread_local uint64_t s_materialDemandDepth = 0;

class MaterialDemandTraceScope {
public:
    explicit MaterialDemandTraceScope(const RenderPassContext& ctx)
    {
        const auto& config = MaterialDemandConfig();
        if (config.path.empty()) return;
        auto& capture = MaterialDemandCapture();
        const auto owner = ctx.resources.GetInstanceId();
        const auto epoch = ctx.resources.GetResetVersion();
        const auto frame = ctx.frameStamp;
        {
            std::lock_guard lock(capture.mutex);
            if (capture.writeStarted) return;
            ++capture.activeScopes;
            m_tracked = true;
            ++s_materialDemandDepth;
            uint64_t passOrdinal = 0;
            for (auto& ordinal : capture.ordinals) {
                if (ordinal.owner != 0 && ordinal.owner != owner) continue;
                if (ordinal.owner == 0 || ordinal.epoch != epoch || ordinal.frame != frame)
                    ordinal = {owner, epoch, frame, 0};
                passOrdinal = ++ordinal.value;
                break;
            }
            if (capture.warmed < config.warmup) ++capture.warmed;
            else if (!capture.sealed) {
                m_index = capture.reserved++;
                m_row.emplace();
                auto& row = *m_row;
                row.resourceInstanceId = owner;
                row.resourceEpoch = epoch;
                row.resourceFrame = frame;
                row.sceneGeneration = ctx.scene.GetRenderSceneGeneration();
                row.reentryDepth = s_materialDemandDepth - 1;
                row.passOrdinalWithinResourceFrame = passOrdinal;
                row.ordinalKnown = passOrdinal != 0;
                if (capture.reserved == config.samples) capture.sealed = true;
            }
        }
        if (!m_row) return;
        auto& row = *m_row;
        row.sceneObjects = ctx.scene.GameObjectCount();
        row.sceneMeshRenderers = ctx.scene.GetEntities<MeshRenderer>().size();
        row.sceneSkinnedRenderers = ctx.scene.GetEntities<SkinnedMeshRenderer>().size();
        row.sceneMaterialComponents = ctx.scene.GetEntities<MaterialComponent>().size();
        row.userPassesAtExtraction = ctx.scene.GetUserRenderPasses().size();
        const bool hasProbes = !ctx.scene.GetEntities<ReflectionProbeComponent>().empty()
            || !ctx.scene.GetEntities<LightProbeVolumeComponent>().empty();
        if (hasProbes) {
            for (auto& go : ctx.scene.GameObjects()) {
                ++row.probeCensusObjects;
                if (!go.activeInHierarchy()) continue;
                if (const auto* probe = go.GetComponent<ReflectionProbeComponent>(); probe && probe->enabled
                    && probe->captureMode == ReflectionProbeCaptureMode::DynamicScene)
                    ++row.activeReflectionProbes;
                if (const auto* volume = go.GetComponent<LightProbeVolumeComponent>(); volume && volume->enabled)
                    ++row.activeLightProbeVolumes;
            }
        }
    }
    MaterialDemandTraceScope(const MaterialDemandTraceScope&) = delete;
    MaterialDemandTraceScope& operator=(const MaterialDemandTraceScope&) = delete;
    ~MaterialDemandTraceScope()
    {
        if (!m_tracked) return;
        auto& capture = MaterialDemandCapture();
        bool write = false;
        {
            std::lock_guard lock(capture.mutex);
            if (m_row) {
                capture.rows[m_index] = *m_row;
                ++capture.finished;
            }
            --capture.activeScopes;
            if (capture.sealed && capture.activeScopes == 0 && !capture.writeStarted) {
                capture.writeStarted = true;
                write = true;
            }
        }
        --s_materialDemandDepth;
        if (write) WriteMaterialDemandCapture(capture);
    }
    bool Active() const { return m_row.has_value(); }
    material_demand_detail::Row& Counters() { return *m_row; }
    void Complete(const RenderPassContext& ctx)
    {
        if (!m_row) return;
        m_row->identityChangedDuringInvocation = m_row->resourceEpoch != ctx.resources.GetResetVersion()
            || m_row->sceneGeneration != ctx.scene.GetRenderSceneGeneration() || m_row->resourceFrame != ctx.resources.FrameStamp();
        m_row->complete = 1;
    }
    void CountPotential(const material_demand_detail::View& view, const renderer::RenderMeshItem& item,
        const material_demand_detail::Result& result)
    {
        if (m_row->syncEligibleItems == 1) m_row->consumerScopeCertified = view.consumerScopeCertified;
        else m_row->consumerScopeCertified = m_row->consumerScopeCertified != 0 && view.consumerScopeCertified;
        if (result.KnownConsumerPotential()) m_row->knownPotentialDeclaredVertices += item.vertexCount;
        material_demand_detail::CountPotential(*m_row, result);
    }
private:
    std::optional<material_demand_detail::Row> m_row;
    uint32_t m_index = 0;
    bool m_tracked = false;
};

uint64_t EntityKey(EntityID id)
{
    return (static_cast<uint64_t>(id.index) << 32) | id.generation;
}

struct RayLodSelection {
    bool visible = false;
    bool ambiguous = false;
    uint64_t group = 0;
};

/// @note Diagnostic phases accumulate nanoseconds without adding a recorder sample for every part; disabled captures never read the clock.
class InstancePhaseTimer {
public:
    InstancePhaseTimer(bool enabled, int64_t& elapsedNs) : m_elapsedNs(enabled ? &elapsedNs : nullptr)
    {
        if (m_elapsedNs) m_start = std::chrono::steady_clock::now();
    }
    InstancePhaseTimer(const InstancePhaseTimer&) = delete;
    InstancePhaseTimer& operator=(const InstancePhaseTimer&) = delete;
    ~InstancePhaseTimer() noexcept { Finish(); }
    void Finish()
    {
        if (!m_elapsedNs) return;
        *m_elapsedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
        m_elapsedNs = nullptr;
    }
private:
    std::chrono::steady_clock::time_point m_start{};
    int64_t* m_elapsedNs = nullptr;
};

/// @note Aggregate only the explicit Materials diagnostic; the false instantiation removes slot-level clocks and trace branches.
template<bool DIAGNOSTICS_ENABLED>
class MaterialPhaseTimer {
public:
    explicit MaterialPhaseTimer(int64_t& elapsedNs, bool enabled=true)
    {
        if constexpr(DIAGNOSTICS_ENABLED){
            if(enabled){m_elapsedNs=&elapsedNs;m_start=std::chrono::steady_clock::now();}
        }else{(void)elapsedNs;(void)enabled;}
    }
    MaterialPhaseTimer(const MaterialPhaseTimer&)=delete;
    MaterialPhaseTimer& operator=(const MaterialPhaseTimer&)=delete;
    ~MaterialPhaseTimer() noexcept {Finish();}
    void Finish()
    {
        if constexpr(DIAGNOSTICS_ENABLED){
            if(!m_elapsedNs)return;
            *m_elapsedNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-m_start).count();
            m_elapsedNs=nullptr;
        }
    }
private:
    int64_t* m_elapsedNs=nullptr;
    std::chrono::steady_clock::time_point m_start{};
};

struct GBufferCopyField {
    size_t variableIndex = SIZE_MAX;
    uint32_t sourceOffset = 0, destinationOffset = 0, bytes = 0;
    bool writable = false;
};

bool IsGBufferCopyVariable(const renderer::ShaderVarDesc& variable, uint32_t bytes)
{
    return variable.IsWritable() && variable.varType == renderer::ShaderVarType::Float
        && variable.varClass == (bytes == sizeof(float) ? renderer::ShaderVarClass::Scalar : renderer::ShaderVarClass::Vector)
        && variable.rows == 1 && variable.elements == 0 && variable.size == bytes;
}

/// @note Retain only lookup indices and offsets; current descriptor names/types and current source byte ranges are still verified.
struct GBufferCopyPlan {
    std::array<GBufferCopyField, 13> fields{};

    explicit GBufferCopyPlan(const renderer::ShaderDescriptor& descriptor)
    {
        struct FieldSpec { std::string_view name; uint32_t offset, bytes; };
        constexpr std::array<FieldSpec, 13> specs{{
            {"albedo", 0, 16}, {"metallic", 16, 4}, {"roughness", 20, 4},
            {"normalStrength", 24, 4}, {"occlusionStrength", 28, 4}, {"emissiveColor", 32, 12},
            {"emissiveScale", 44, 4}, {"uvTiling", 48, 8}, {"uvOffset", 56, 8}, {"alphaCutoff", 64, 4},
            {"leafWindStrength", 68, 4}, {"leafWindFrequency", 72, 4}, {"leafWindRootHeight", 76, 4}}};
        for (size_t index = 0; index < fields.size(); ++index) {
            auto& field = fields[index];
            field.destinationOffset = specs[index].offset; field.bytes = specs[index].bytes;
            if (const auto* variable = descriptor.FindVar(specs[index].name)) {
                field.variableIndex = static_cast<size_t>(variable - descriptor.vars.data());
                field.sourceOffset = variable->offset;
                field.writable = IsGBufferCopyVariable(*variable, field.bytes);
            }
        }
    }

    bool Matches(const renderer::ShaderDescriptor& prepared, const renderer::ShaderDescriptor& current) const
    {
        if (prepared.cbufferSize != current.cbufferSize || prepared.vars.size() != current.vars.size()) return false;
        /// @note Full name order preserves FindVar's first match, including absent fields and duplicate names after in-place edits.
        for (size_t index = 0; index < prepared.vars.size(); ++index)
            if (prepared.vars[index].name != current.vars[index].name) return false;
        for (const auto& field : fields) {
            if (field.variableIndex == SIZE_MAX) continue;
            const auto& variable = current.vars[field.variableIndex];
            if (IsGBufferCopyVariable(variable, field.bytes) != field.writable
                || (field.writable && variable.offset != field.sourceOffset)) return false;
        }
        return true;
    }

    void Copy(const std::vector<uint8_t>& source, std::array<uint8_t, 96>& output) const
    {
        for (const auto& field : fields)
            if (field.writable && field.sourceOffset <= source.size() && field.bytes <= source.size() - field.sourceOffset)
                std::memcpy(output.data() + field.destinationOffset, source.data() + field.sourceOffset, field.bytes);
    }
};

/// @note 一回の抽出だけで能力照会を共有し、次の抽出では変更・削除を再確認する。
class GeometryMaterialCache {
public:
    explicit GeometryMaterialCache(const renderer::ResourceManager* resources = nullptr)
        : m_resources(resources), m_resourceOwner(resources ? resources->GetInstanceId() : 0),
          m_resetVersion(resources ? resources->GetResetVersion() : 0),
          m_shaderVersion(resources ? resources->GetShaderVersion() : 0),
          m_assetGeneration(asset::AssetManager::GetAssetGeneration()) {}

    renderer::GeometryMaterialInput Extract(const MaterialSlot& slot)
    {
        const auto slotSnapshot = slot.ReadAuthoring();
        const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
        return ExtractCapabilities(slotSnapshot->Data(), snapshot);
    }

    /// @note Resolve one retained publication pair after indexed material loading; no per-item state survives this call.
    void ApplyState(const MaterialSlot& slot, renderer::RenderMaterial& output)
    {
        RefreshCapabilitiesEpoch();
        const auto slotSnapshot = slot.ReadAuthoring();
        const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
        const auto& values = slotSnapshot->Data();
        const auto* material = snapshot ? &snapshot->Data() : nullptr;
        output.capabilities = ExtractCapabilities(values, snapshot);
        output.renderQueue = values.hasRenderQueueOverride ? values.renderQueueOverride
            : values.hasPresentationDefaults ? values.presentationRenderQueue
            : material ? material->renderQueue : renderer::RenderQueue::GEOMETRY;
        output.doubleSided = values.hasDoubleSidedOverride ? values.doubleSidedOverride
            : values.hasPresentationDefaults ? values.presentationDoubleSided
            : material && material->doubleSided;
        output.depthBias = material ? material->depthBias : 0;
        output.depthBiasSlope = material ? material->depthBiasSlope : 0.0f;
    }

private:
    renderer::GeometryMaterialInput ExtractCapabilities(const MaterialSlotAuthoring& slot,
        const asset::MaterialAsset::AuthoringSnapshotPtr& snapshot)
    {
        const auto* material = snapshot ? &snapshot->Data() : nullptr;
        if (!material) return ExtractGeometryMaterial(slot, nullptr, false);
        const bool sharedRoute = slot.paramOverrides.empty() && !slot.hasBlendModeOverride;
        if (sharedRoute) {
            const auto cached = m_geometryDefaults.find(snapshot);
            if (cached != m_geometryDefaults.end()) return cached->second;
        }
        auto found = m_shaderCapabilities.find(material->shaderPath);
        if (found == m_shaderCapabilities.end()) {
            FBZZ_PROFILE_SCOPE("RenderSceneExtractor::ShaderCapabilities");
            found = m_shaderCapabilities.emplace(material->shaderPath,
                IsGBufferEquivalentShader(material->shaderPath)).first;
        }
        const auto input = ExtractGeometryMaterial(slot, material, found->second);
        if (sharedRoute) m_geometryDefaults.emplace(snapshot, input);
        return input;
    }

    /// @note Asset generation and native reload/reset can change shader resolution inside a preceding slot's callback without replacing either publication.
    void RefreshCapabilitiesEpoch()
    {
        const auto owner = m_resources ? m_resources->GetInstanceId() : 0;
        const auto reset = m_resources ? m_resources->GetResetVersion() : 0;
        const auto shader = m_resources ? m_resources->GetShaderVersion() : 0;
        const auto assets = asset::AssetManager::GetAssetGeneration();
        if (m_resourceOwner == owner && m_resetVersion == reset && m_shaderVersion == shader && m_assetGeneration == assets) return;
        m_resourceOwner = owner; m_resetVersion = reset; m_shaderVersion = shader;
        m_assetGeneration = assets;
        m_shaderCapabilities.clear(); m_geometryDefaults.clear();
    }

public:
    const PreparedMaterialParams* Prepare(MaterialComponent& component, size_t slotIndex,
        renderer::ResourceManager& resources, bool skinned)
    {
        const auto slotSnapshot = component.SlotAt(slotIndex).ReadAuthoring();
        const auto handle = component.SlotAt(slotIndex).GetMaterialAsset();
        const auto snapshot = asset::ReadMaterialAuthoring(handle);
        if (!snapshot) return nullptr;
        const auto& values = snapshot->Data();
        if (values.shaderPath.empty() || values.renderPath != asset::RenderPath::Auto
            || (skinned && values.meshType == asset::MeshType::Surface)
            || (!skinned && values.meshType == asset::MeshType::Skinned)) return nullptr;
        auto [found, inserted] = m_prepared.try_emplace(snapshot);
        auto& cached = found->second;
        if (inserted || cached.prepared.resourceOwner != resources.GetInstanceId()
            || cached.prepared.resetVersion != resources.GetResetVersion()
            || cached.prepared.shaderVersion != resources.GetShaderVersion()) {
            PreparedMaterialParams candidate;
            candidate.source = snapshot; candidate.shaderPath = values.shaderPath;
            candidate.resourceOwner = resources.GetInstanceId(); candidate.resetVersion = resources.GetResetVersion();
            candidate.shaderVersion = resources.GetShaderVersion();
            candidate.shader = resources.LoadShader(values.shaderPath);
            const auto* shader = resources.Get(candidate.shader);
            if (shader) {
                candidate.descriptor = std::make_shared<const renderer::ShaderDescriptor>(shader->GetDescriptor());
                const auto vertex = resources.GetShaderCapabilities(candidate.shader).vertex;
                candidate.standardDescriptorVerified = vertex == renderer::ShaderVertexContract::STANDARD_SURFACE
                    || vertex == renderer::ShaderVertexContract::STANDARD_SKINNED;
                candidate.bytes.assign(candidate.descriptor->cbufferSize, 0u);
                asset::InitDefaultMaterialParams(*candidate.descriptor, candidate.bytes);
                asset::ApplyMaterialAssetParams(values, *candidate.descriptor, candidate.bytes);
            }
            /// @note Re-resolve the current slot after native shader callbacks; never publish an old owner/layout over a reentrant edit.
            if (candidate.resourceOwner != resources.GetInstanceId() || candidate.resetVersion != resources.GetResetVersion()
                || candidate.shaderVersion != resources.GetShaderVersion() || asset::ReadMaterialAuthoring(handle) != snapshot
                || component.SlotAt(slotIndex).ReadAuthoring() != slotSnapshot || component.SlotAt(slotIndex).GetMaterialAsset() != handle)
                return nullptr;
            cached.prepared = std::move(candidate); cached.packedVariants.clear(); cached.synchronized.clear();
            cached.copyPlan.reset();
            if (cached.prepared.standardDescriptorVerified && cached.prepared.descriptor)
                cached.copyPlan.emplace(*cached.prepared.descriptor);
            cached.hasTextureRequests = std::any_of(values.textures.begin(), values.textures.end(),
                [](const auto& entry) { return !entry.second.empty(); });
        }
        return cached.prepared.descriptor ? &cached.prepared : nullptr;
    }

    /// @note Only asset defaults share GPU bindings; overrides and presentation values retain their slot-owned synchronization.
    /// @note Entries expire with this extraction, so edits, texture residency and subsequent views are reevaluated.
    /// @note Textured object slots keep their own buffers because another submesh can synchronize the same slot at a different projected size.
    bool CopySynchronized(const MaterialSlot& slot, renderer::ResourceManager& resources, bool skinned,
        float screenPixels, renderer::RenderMaterial& output, bool shareTexturedDefaults = false) const
    {
        if (!CanShareSynchronization(slot)) return false;
        const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
        const auto found = m_prepared.find(snapshot);
        if (found == m_prepared.end() || (found->second.hasTextureRequests && !shareTexturedDefaults)) return false;
        const uint32_t textureRequest = found->second.hasTextureRequests ? std::bit_cast<uint32_t>(screenPixels) : 0;
        for (const auto& entry : found->second.synchronized) {
            if (entry.skinned == skinned && entry.textureRequest == textureRequest
                && entry.shaderVersion == resources.GetShaderVersion()
                && entry.resetVersion == resources.GetResetVersion()) {
                output = entry.material;
                return true;
            }
        }
        return false;
    }

    void RememberSynchronized(const MaterialSlot& slot, renderer::ResourceManager& resources, bool skinned,
        float screenPixels, const renderer::RenderMaterial& material, bool shareTexturedDefaults = false)
    {
        if (!material.valid || !material.paramsBuffer.IsValid() || !CanShareSynchronization(slot)) return;
        const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
        const auto found = m_prepared.find(snapshot);
        if (found == m_prepared.end() || material.shader != found->second.prepared.shader
            || (found->second.hasTextureRequests && !shareTexturedDefaults)) return;
        auto& cached = found->second;
        /// @note Distinct projected texture requests bypass sharing after the bound; texture quality is never quantized to fit the cache.
        constexpr size_t MAXIMUM_SYNCHRONIZED_VARIANTS = 32;
        if (cached.synchronized.size() >= MAXIMUM_SYNCHRONIZED_VARIANTS) return;
        cached.synchronized.push_back({material, resources.GetShaderVersion(), resources.GetResetVersion(),
            cached.hasTextureRequests ? std::bit_cast<uint32_t>(screenPixels) : 0, skinned});
    }

    bool CopyPacked(const MaterialSlot& slot, const renderer::Material& source, renderer::RenderMaterial& output,
        const renderer::ResourceManager& resources, const GBufferCopyPlan*& copyPlan,
        const renderer::ShaderDescriptor*& layout)
    {
        copyPlan = nullptr; layout = nullptr;
        const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
        const auto found = m_prepared.find(snapshot);
        if (found == m_prepared.end()) return false;
        const auto shaderVersion = resources.GetShaderVersion();
        for (const auto& packed : found->second.packedVariants) {
            if (packed.shaderVersion != shaderVersion || packed.shader != source.shader || packed.sourceBytes != source.paramData) continue;
            output.gbufferParams = packed.params; output.directGBufferParams = packed.direct;
            return true;
        }
        const auto& cached = found->second;
        const auto& prepared = cached.prepared;
        const auto vertex = output.shaderCapabilities.vertex;
        if (cached.copyPlan && prepared.descriptor && prepared.shader == source.shader
            && prepared.resourceOwner == resources.GetInstanceId() && prepared.resetVersion == resources.GetResetVersion()
            && prepared.shaderVersion == shaderVersion
            && (vertex == renderer::ShaderVertexContract::STANDARD_SURFACE || vertex == renderer::ShaderVertexContract::STANDARD_SKINNED)) {
            /// @note Borrow only until the next callback-free native descriptor read; cache ownership remains local to this extraction.
            copyPlan = &*cached.copyPlan; layout = prepared.descriptor.get();
        }
        return false;
    }

    void RememberPacked(const MaterialSlot& slot, const renderer::Material& source, const renderer::RenderMaterial& output,
        uint64_t shaderVersion)
    {
        const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
        const auto found = m_prepared.find(snapshot);
        if (found == m_prepared.end()) return;
        auto& variants = found->second.packedVariants;
        /// @note 固有上書きが大量にあるassetは通常経路へ戻し、同値共有のための追加メモリをasset当たり32種類へ制限する。
        constexpr size_t MAXIMUM_PACKED_VARIANTS = 32;
        if (variants.size() >= MAXIMUM_PACKED_VARIANTS) return;
        variants.push_back({source.shader, shaderVersion, source.paramData, output.gbufferParams, output.directGBufferParams});
    }

private:
    static bool CanShareSynchronization(const MaterialSlot& slot)
    {
        const auto& values = slot.Authoring();
        return slot.runtimeTextureOverrides.empty() && values.paramOverrides.empty() && values.integerParamOverrides.empty() && values.textureOverrides.empty()
            && values.presentationParamDefaults.empty() && values.presentationTextureDefaults.empty()
            && !values.hasPresentationDefaults && !values.hasPresentationMaterialOverride
            && !values.hasBlendModeOverride && !values.hasDoubleSidedOverride && !values.hasRenderQueueOverride;
    }

    struct SynchronizedMaterial {
        renderer::RenderMaterial material;
        uint64_t shaderVersion = 0, resetVersion = 0;
        uint32_t textureRequest = 0;
        bool skinned = false;
    };
    struct PackedVariant {
        renderer::ResourceHandle<renderer::ShaderTag> shader;
        uint64_t shaderVersion = 0;
        std::vector<uint8_t> sourceBytes;
        std::array<uint8_t, 96> params{};
        bool direct = false;
    };
    struct CachedMaterial {
        PreparedMaterialParams prepared;
        std::optional<GBufferCopyPlan> copyPlan;
        std::vector<PackedVariant> packedVariants;
        std::vector<SynchronizedMaterial> synchronized;
        bool hasTextureRequests = false;
    };
    const renderer::ResourceManager* m_resources = nullptr;
    uint64_t m_resourceOwner = 0, m_resetVersion = 0, m_shaderVersion = 0;
    int m_assetGeneration = -1;
    std::unordered_map<std::string, bool> m_shaderCapabilities;
    /// @note Per-instance lobe and blend overrides bypass the shared route; asset-only decisions last for one extraction.
    std::unordered_map<asset::MaterialAsset::AuthoringSnapshotPtr, renderer::GeometryMaterialInput> m_geometryDefaults;
    /// @note Retained publications distinguish same-handle replacement and keep defaults alive across callbacks; the map expires with this extraction.
    std::unordered_map<asset::MaterialAsset::AuthoringSnapshotPtr, CachedMaterial> m_prepared;
};

/// @note LOD0 は全ビュー共通の正本。GUID fallback は Raster と同じで、カメラ・crossfade は参照しない。
std::unordered_map<uint64_t, RayLodSelection> CollectRayLodRenderers(Scene& scene,
    std::vector<renderer::RenderRayLodDiagnostic>& diagnostics)
{
    std::unordered_map<uint64_t, RayLodSelection> result;
    for (EntityID id : scene.GetEntities<LODGroupComponent>()) {
        const auto* group = scene.GetComponent<LODGroupComponent>(id);
        const auto* owner = scene.GetGameObject(id);
        if (!group || !group->enabled || !owner || !owner->activeInHierarchy()) continue;
        bool canonicalMissing = false;
        for (size_t levelIndex = 0; levelIndex < group->levels.size(); ++levelIndex) {
            const auto& level = group->levels[levelIndex];
            for (const auto& reference : level.renderers) {
                EntityID renderer = reference.entity;
                if (!scene.IsValid(renderer) && !reference.instanceId.empty()) {
                    if (const auto* resolved = scene.FindByGuid(reference.instanceId))
                        renderer = resolved->GetID();
                }
                if (!scene.IsValid(renderer)) {
                    canonicalMissing |= levelIndex == 0;
                    continue;
                }
                const auto* target = scene.GetGameObject(renderer);
                canonicalMissing |= levelIndex == 0 && (!target
                    || (!scene.GetComponent<MeshRenderer>(renderer) && !scene.GetComponent<SkinnedMeshRenderer>(renderer)));
                const auto [found, inserted] = result.emplace(EntityKey(renderer),
                    RayLodSelection{levelIndex == 0, false, EntityKey(id)});
                if (!inserted) {
                    found->second.visible |= levelIndex == 0;
                    found->second.ambiguous |= found->second.group != EntityKey(id);
                }
            }
        }
        /// @note 未解決の canonical renderer の layer は証明できず、下位 LOD の layer で除外しない。
        if (canonicalMissing)
            diagnostics.push_back({id.index, id.generation, UINT32_MAX});
    }
    return result;
}

template<typename T>
math::Matrix4 AdvanceWorld(T& component, const math::Matrix4& world, uint64_t frame)
{
    if (component.prevWorldFrame != frame) {
        component.prevWorldMatrix = frame > 0 && component.prevWorldFrame == frame - 1
            ? component.worldMatrixSnapshot : world;
        component.worldMatrixSnapshot = world;
        component.prevWorldFrame = frame;
    }
    return component.prevWorldMatrix;
}

template<typename T>
renderer::RenderObject ExtractObject(GameObject& go, T& component, uint64_t frame)
{
    renderer::RenderObject object;
    object.sourceIndex = go.GetID().index;
    object.sourceGeneration = go.GetID().generation;
    object.layer = static_cast<uint32_t>(go.layer);
    object.world = go.transform.GetWorldMatrix();
    object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(object.world);
    object.previousWorld = AdvanceWorld(component, object.world, frame);
    object.sortPosition = go.transform.position;
    object.castShadows = component.castShadows;
    object.lodVisible = component.lodVisible;
    object.lodDither = component.lodDither;
    return object;
}

float WorldOccluderRadius(const math::Matrix4& world, float localRadius)
{
    /// @note The renderer-world transform is TRS; its minimum axis length keeps the inscribed sphere inside negative/nonuniformly scaled geometry.
    const float sx = math::Vector3{world.m[0][0], world.m[1][0], world.m[2][0]}.Length();
    const float sy = math::Vector3{world.m[0][1], world.m[1][1], world.m[2][1]}.Length();
    const float sz = math::Vector3{world.m[0][2], world.m[1][2], world.m[2][2]}.Length();
    return localRadius > 0.0f ? localRadius * (std::min)({sx, sy, sz}) : 0.0f;
}

renderer::RenderMeshItem ExtractMesh(const math::Matrix4& world, float maximumScale, const renderer::Mesh& mesh)
{
    renderer::RenderMeshItem item;
    item.vertexBuffer = mesh.vertexBuffer;
    item.skinningVertexBuffer = mesh.vertexBuffer;
    item.indexBuffer = mesh.indexBuffer;
    item.indexCount = mesh.indexCount;
    item.vertexCount = mesh.vertexCount;
    item.clusterLod = mesh.ClusterLod();
    item.vertexStride = mesh.isSkinned ? sizeof(renderer::SkinnedVertex) : sizeof(renderer::Vertex);
    item.reliableOccluder = IsReliableOccluder(mesh);
    const auto bounds = ComputeWorldBounds(world, maximumScale, mesh);
    item.boundsCenter = bounds.center;
    item.boundsRadius = mesh.boundsRadius > 0.0f ? bounds.radius : 0.0f;
    item.occluderRadius = WorldOccluderRadius(world, mesh.occlusionRadius);
    return item;
}

/// @note Instance mesh resolution can invoke callbacks; read the current transform at its existing extraction boundary.
renderer::RenderMeshItem ExtractMesh(GameObject& go, const renderer::Mesh& mesh)
{
    const math::Matrix4 world = go.transform.GetWorldMatrix();
    return ExtractMesh(world, ComputeWorldBoundsScale(world), mesh);
}

void CopyMaterial(renderer::RenderMaterial& output, const renderer::Material* material,
                  renderer::ResourceManager& resources, const MaterialSlot& slot, GeometryMaterialCache& cache)
{
    output.valid = material != nullptr;
    if (!material) return;
    output.shader = material->shader;
    output.shaderCapabilities = resources.GetShaderCapabilities(material->shader);
    output.paramsBuffer = material->paramsBuffer;
    for (size_t i = 0; i < output.textures.size() && i < material->textures.size(); ++i)
        output.textures[i] = material->textures[i];
    /// @note 異なるシェーダーの CB レイアウトを GBuffer の 96 バイトへ正規化する。
    output.directGBufferParams = false;
    output.gbufferParams = {};
    if (renderer::IsErrorShader(material->shaderPath)) {
        /// @note 壊れた材質の透過・色・テクスチャ上書きで診断表示が消えないよう、実効状態を揃える。
        output.capabilities = {};
        output.capabilities.blend = renderer::BlendMode::OPAQUE_BLEND;
        output.renderQueue = renderer::RenderQueue::GEOMETRY;
        output.doubleSided = true;
        output.depthBias = 0;
        output.depthBiasSlope = 0;
        output.paramsBuffer = {};
        output.textures = {};
        const float magenta[] = {1, 0, 1, 1};
        std::memcpy(output.gbufferParams.data(), magenta, sizeof(magenta));
        return;
    }
    const GBufferCopyPlan* copyPlan = nullptr;
    const renderer::ShaderDescriptor* preparedLayout = nullptr;
    if (cache.CopyPacked(slot, *material, output, resources, copyPlan, preparedLayout)) return;
    const float roughness = 0.5f;
    const float tiling[] = { 1.0f, 1.0f };
    std::memcpy(output.gbufferParams.data() + 20, &roughness, sizeof(roughness));
    std::memcpy(output.gbufferParams.data() + 48, tiling, sizeof(tiling));
    const auto* shader = resources.Get(material->shader);
    if (!shader) return;
    const auto& descriptor = shader->GetDescriptor();
    if (copyPlan && preparedLayout && copyPlan->Matches(*preparedLayout, descriptor))
        copyPlan->Copy(material->paramData, output.gbufferParams);
    else {
        const auto copy = [&](std::string_view name, uint32_t offset, uint32_t bytes) {
            const auto* variable = descriptor.FindVar(name);
            if (variable && IsGBufferCopyVariable(*variable, bytes)
                && variable->offset <= material->paramData.size()
                && bytes <= material->paramData.size() - variable->offset)
                std::memcpy(output.gbufferParams.data() + offset,
                            material->paramData.data() + variable->offset, bytes);
        };
        copy("albedo", 0, 16);
        copy("metallic", 16, 4);
        copy("roughness", 20, 4);
        copy("normalStrength", 24, 4);
        copy("occlusionStrength", 28, 4);
        copy("emissiveColor", 32, 12);
        copy("emissiveScale", 44, 4);
        copy("uvTiling", 48, 8);
        copy("uvOffset", 56, 8);
        copy("alphaCutoff", 64, 4);
        copy("leafWindStrength", 68, 4);
        copy("leafWindFrequency", 72, 4);
        copy("leafWindRootHeight", 76, 4);
    }
    if (descriptor.textureMaskOffset != UINT32_MAX &&
        descriptor.textureMaskOffset <= material->paramData.size() &&
        4 <= material->paramData.size() - descriptor.textureMaskOffset)
        std::memcpy(output.gbufferParams.data() + 80,
                    material->paramData.data() + descriptor.textureMaskOffset, 4);
    /// @note 同じ 96 bytes でも変数の位置は異なり得る。正規化した値が一致するときだけ元 CB を共有する。
    output.directGBufferParams = material->paramData.size() == output.gbufferParams.size()
        && std::memcmp(material->paramData.data(), output.gbufferParams.data(), output.gbufferParams.size()) == 0;
    cache.RememberPacked(slot, *material, output, resources.GetShaderVersion());
}

void ExtractRayMaterialInputs(renderer::RenderMaterial& material,
    const renderer::Material* source, const MaterialSlot& slot, renderer::ResourceManager& resources,
    bool currentDeformationVerified)
{
    const auto contract = material.shaderCapabilities;
    /// @note 成功した shader 版の能力だけを使い、失敗した reload の新宣言を描画へ混ぜない。
    material.capabilities.gbufferEquivalentShader = contract.SupportsGBuffer();
    const bool deformationVerified = !contract.SupportsSkinning() || currentDeformationVerified;
    const bool pbr = contract.IsStandardPbr() && deformationVerified;
    const bool unclipped = contract.HasOpaqueOutput();
    const auto* shader = resources.Get(material.shader);
    const bool standardGeometry = material.valid && shader && contract.HasStandardGeometry() && deformationVerified;
    /// @note Lit / Fallback は texture alpha に関係なく alpha=1 を返し、clip を実行しない。
    float alpha = 0, cutoff = 0;
    uint32_t textureMask = 0;
    std::memcpy(&alpha, material.gbufferParams.data() + 12, sizeof(alpha));
    std::memcpy(&cutoff, material.gbufferParams.data() + 64, sizeof(cutoff));
    std::memcpy(&textureMask, material.gbufferParams.data() + 80, sizeof(textureMask));
    material.rayCapabilities = standardGeometry && unclipped
        ? renderer::RayMaterialCapabilities{true, renderer::RayOpacity::OPAQUE_SURFACE}
        : renderer::ResolveStaticRayMaterialCapabilities(standardGeometry,
            material.capabilities.blend, alpha, cutoff, (textureMask & 1u) != 0);
    uint32_t declaredTextureMask = 0;
    bool unresolvedTexture = false;
    const auto declareTexture = [&](const std::string& name, const std::string& texturePath) {
        if (texturePath.empty()) return;
        for (uint32_t i = 0; i < renderer::kMaterialTextureSlotCount; ++i) {
            if (name != renderer::kMaterialTextureSlots[i].key) continue;
            declaredTextureMask |= 1u << i;
            return;
        }
        unresolvedTexture = true;
    };
    const auto materialAssetSnapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
    const auto slotAuthoringSnapshot = slot.ReadAuthoring();
    const auto* materialAsset = materialAssetSnapshot ? &materialAssetSnapshot->Data() : nullptr;
    /// @note 未ロードや無効な GPU handle でも、元 .mat の texture 指定を定数材質と誤認しない。
    if (materialAsset) {
        for (const auto& [name, texturePath] : materialAsset->textures) {
            const auto replacement = slotAuthoringSnapshot->Data().textureOverrides.find(name);
            declareTexture(name, replacement != slotAuthoringSnapshot->Data().textureOverrides.end() ? replacement->second : texturePath);
        }
    }
    for (const auto& [name, texturePath] : slotAuthoringSnapshot->Data().textureOverrides)
        declareTexture(name, texturePath);
    bool advancedLobes = false;
    if (pbr && shader && source) {
        /// @note GBuffer の 96 bytes 外にある追加ローブを、定数 metallic / roughness と誤認しない。
        for (const auto* name : {"clearcoat", "sheen", "anisotropy"}) {
            const auto* variable = shader->GetDescriptor().FindVar(name);
            if (!variable) continue;
            if (variable->offset > source->paramData.size()
                || sizeof(float) > source->paramData.size() - variable->offset) {
                advancedLobes = true;
                continue;
            }
            float amount = 0;
            std::memcpy(&amount, source->paramData.data() + variable->offset, sizeof(amount));
            advancedLobes |= !std::isfinite(amount) || amount != 0;
        }
    }
    material.surface = source ? source->ResolveRaySurface(material.gbufferParams, standardGeometry && pbr,
        declaredTextureMask, unresolvedTexture, advancedLobes, resources)
        : renderer::ResolveConstantSurfaceMaterial(material.gbufferParams, false);
    /// @note Surface resolution includes authored-but-not-yet-uploaded textures; never share an opaque BLAS variant for them.
    if (!unclipped)
        material.rayCapabilities = renderer::ResolveStaticRayMaterialCapabilities(standardGeometry,
            material.capabilities.blend, alpha, cutoff, (material.surface.textureMask & 1u) != 0);
}

/// @brief Shared mesh fragments stream directly into the dense render-item array without an intermediate actor copy.
/// @note Shared fragment separation: https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine
class DenseGeometryBatch {
public:
    DenseGeometryBatch(renderer::RenderScene& output, size_t hint) : m_output(output)
    { m_output.objects.reserve(hint); m_output.items.reserve(hint); m_meshes.reserve(32); }
    void MarkMovingGeometry()
    {
        if (m_movingSeen) return;
        m_movingSeen = true;
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::MovingGeometry");
    }
    void MarkExistingChange() { ++m_existingChanges; }
    size_t ExistingChanges() const { return m_existingChanges; }
    renderer::RenderMeshItem& Append(const renderer::Mesh& mesh, const math::Matrix4& world, float maximumScale)
    {
        if (m_lastMesh != &mesh) {
            auto [found, inserted] = m_meshIndices.try_emplace(&mesh, m_meshes.size());
            if (inserted) m_meshes.push_back({mesh.vertexBuffer, mesh.indexBuffer, mesh.vertexCount, mesh.indexCount,
                mesh.isSkinned ? sizeof(renderer::SkinnedVertex) : sizeof(renderer::Vertex),
                mesh.boundsCenter, mesh.boundsRadius, mesh.occlusionRadius, IsReliableOccluder(mesh), mesh.ClusterLod()});
            m_lastMesh = &mesh; m_lastIndex = found->second;
        }
        const auto& shared = m_meshes[m_lastIndex];
        auto& item = m_output.items.emplace_back();
        item.vertexBuffer = shared.vertices; item.indexBuffer = shared.indices;
        item.skinningVertexBuffer = shared.vertices;
        item.vertexCount = shared.vertexCount; item.indexCount = shared.indexCount;
        item.clusterLod = shared.clusterLod;
        item.vertexStride = static_cast<uint32_t>(shared.stride); item.reliableOccluder = shared.reliable;
        const auto& c = shared.center;
        item.boundsCenter = {
            world.m[0][0]*c.x + world.m[0][1]*c.y + world.m[0][2]*c.z + world.m[0][3],
            world.m[1][0]*c.x + world.m[1][1]*c.y + world.m[1][2]*c.z + world.m[1][3],
            world.m[2][0]*c.x + world.m[2][1]*c.y + world.m[2][2]*c.z + world.m[2][3]};
        item.boundsRadius = shared.radius > 0.0f ? shared.radius * maximumScale + 0.0f : 0.0f;
        item.occluderRadius = WorldOccluderRadius(world, shared.occlusionRadius);
        return item;
    }
private:
    struct SharedMeshFragment {
        renderer::ResourceHandle<renderer::BufferTag> vertices, indices;
        uint32_t vertexCount, indexCount;
        size_t stride;
        math::Vector3 center;
        float radius;
        float occlusionRadius;
        bool reliable;
        std::shared_ptr<const renderer::MeshletClusterData> clusterLod;
    };
    renderer::RenderScene& m_output;
    std::vector<SharedMeshFragment> m_meshes;
    std::unordered_map<const renderer::Mesh*, size_t> m_meshIndices;
    const renderer::Mesh* m_lastMesh = nullptr;
    size_t m_lastIndex = 0;
    bool m_movingSeen = false;
    size_t m_existingChanges = 0;
};

/// @note A cold Scene uses the original path; repeatedly moving entries avoid copying current matrices back into the persistent cache.
/// @see https://learn.microsoft.com/en-us/cpp/cpp/inline-functions-cpp __forceinline keeps the aggregate-return helper eligible for caller substitution.
__forceinline renderer::RenderObject ExtractUncachedStaticObject(GameObject& go, MeshRenderer& component, uint64_t frame,
    renderer::RenderScene& output, DenseGeometryBatch& denseGeometry)
{
    auto object = ExtractObject(go, component, frame);
    object.firstItem = static_cast<uint32_t>(output.items.size());
    auto& item = denseGeometry.Append(*component.mesh, object.world, ComputeWorldBoundsScale(object.world));
    const float windPadding = ComputeLeafWindBoundsPadding(go, 1);
    object.leafWindBoundsPadding = windPadding;
    if (windPadding < 0.0f) item.boundsRadius = 0.0f;
    else if (item.boundsRadius > 0.0f) item.boundsRadius += windPadding;
    if (windPadding != 0.0f) item.occluderRadius = 0.0f;
    object.boundsCenter = item.boundsCenter; object.boundsRadius = item.boundsRadius;
    object.itemCount = 1;
    item.objectIndex = static_cast<uint32_t>(output.objects.size());
    return object;
}

/// @note Exact current-input witnesses rebuild only changed math; notifications invalidate shared snapshots, including presentation-only writes.
/// @see https://github.com/godotengine/godot/blob/master/servers/rendering/renderer_scene_cull.h Instance dependency notifications.
renderer::RenderObject ExtractStaticObject(const ComponentArray<MeshRenderer>& staticMeshes, StaticSceneGeometry& cache,
    GameObject& go, MeshRenderer& component, uint64_t frame, renderer::RenderScene& output, DenseGeometryBatch& denseGeometry,
    bool& boundsChanged)
{
    boundsChanged = false;
    const auto entity = go.GetID();
    /// @note Component の格納位置は有効状態や Scene の表示順で変わらない。
    const size_t slot = staticMeshes.GetDenseIndex(entity);
    if (slot >= cache.entries.size()) cache.entries.resize(slot + 1);
    auto& entry = cache.entries[slot];
    const auto lifetime = staticMeshes.GetLifetime(entity);
    const auto pose = StaticPoseInputs(go.transform);
    const bool sameOwner = entry.entity == entity && entry.lifetime == lifetime && entry.mesh == component.mesh;
    const bool poseChanged = entry.pose != pose;
    const bool newFrame = entry.lastExtractFrame != frame;
    entry.lastExtractFrame = frame;
    if (sameOwner && poseChanged && newFrame && frame > 0 && entry.lastPoseChangeFrame == frame - 1) {
        boundsChanged = true;
        entry.pose = pose; entry.lastPoseChangeFrame = frame; entry.valid = false;
        denseGeometry.MarkExistingChange();
        denseGeometry.MarkMovingGeometry();
        return ExtractUncachedStaticObject(go, component, frame, output, denseGeometry);
    }
    if (!sameOwner) entry.lastPoseChangeFrame = UINT64_MAX;
    else if (poseChanged) entry.lastPoseChangeFrame = frame;
    const auto meshInputs = StaticMeshInputs(*component.mesh);
    const float windPadding = ComputeLeafWindBoundsPadding(go, 1);
    if (!entry.valid || entry.entity != entity || entry.lifetime != lifetime || entry.mesh != component.mesh
        || entry.pose != pose || entry.meshInputs != meshInputs || entry.windPadding != windPadding) {
        boundsChanged = true;
        if (entry.entity.IsValid()) denseGeometry.MarkExistingChange();
        std::optional<profiler::ProfileScope> rebuildProfile;
        if (profiler::Profiler::IsEnabled()) rebuildProfile.emplace(
            profiler::ProfilerMarker("RenderSceneExtractor::StaticGeometryRebuild"));
        entry.entity = entity; entry.lifetime = lifetime; entry.mesh = component.mesh;
        entry.pose = pose; entry.meshInputs = meshInputs; entry.windPadding = windPadding;
        entry.world = go.transform.GetWorldMatrix();
        entry.inverseTranspose = math::Matrix4::InverseTransposeAffine(entry.world);
        const auto bounds = ComputeWorldBounds(entry.world, ComputeWorldBoundsScale(entry.world), *component.mesh);
        entry.center = bounds.center;
        entry.radius = component.mesh->boundsRadius > 0.0f && windPadding >= 0.0f ? bounds.radius + windPadding : 0.0f;
        entry.occluderRadius = windPadding == 0.0f ? WorldOccluderRadius(entry.world, component.mesh->occlusionRadius) : 0.0f;
        entry.reliable = IsReliableOccluder(*component.mesh);
        entry.valid = true;
    }
    renderer::RenderObject object;
    object.sourceIndex = entity.index; object.sourceGeneration = entity.generation;
    object.layer = static_cast<uint32_t>(go.layer);
    object.world = entry.world; object.worldInvTranspose = entry.inverseTranspose;
    object.previousWorld = AdvanceWorld(component, object.world, frame);
    object.sortPosition = go.transform.position;
    object.castShadows = component.castShadows; object.lodVisible = component.lodVisible; object.lodDither = component.lodDither;
    object.firstItem = static_cast<uint32_t>(output.items.size()); object.itemCount = 1;
    object.boundsCenter = entry.center; object.boundsRadius = entry.radius;
    object.leafWindBoundsPadding = entry.windPadding;
    auto& item = output.items.emplace_back();
    item.objectIndex = static_cast<uint32_t>(output.objects.size());
    item.vertexBuffer = item.skinningVertexBuffer = component.mesh->vertexBuffer;
    item.indexBuffer = component.mesh->indexBuffer;
    item.vertexCount = component.mesh->vertexCount; item.indexCount = component.mesh->indexCount;
    item.clusterLod = component.mesh->ClusterLod();
    item.vertexStride = sizeof(renderer::Vertex);
    item.boundsCenter = entry.center; item.boundsRadius = entry.radius; item.reliableOccluder = entry.reliable;
    item.occluderRadius = entry.occluderRadius;
    return object;
}

struct SkinnedObjectRange {
    size_t begin = 0, end = 0;
    void Append(size_t ordinal)
    {
        if (begin == end) begin = ordinal;
        end = ordinal + 1;
    }
};

/// @note Compare both paths in one executable; unrecognized and absent values preserve the legacy scans.
bool UseFusedExtractionScans()
{
    const char* mode = std::getenv("FBZZ_RENDER_EXTRACTION_SCAN_MODE");
    const bool fused = mode && std::strcmp(mode, "fused") == 0;
    static std::atomic<bool> configurationLogged{false};
    if (mode && !configurationLogged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "RENDER_EXTRACTION_SCAN_CONFIG mode=%s\n", fused ? "fused" : "legacy");
    return fused;
}

renderer::RenderScene BuildRenderSceneGeometry(Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette, bool rayTracingInputs,
    GeometryMaterialCache& materialCache, const renderer::ResourceManager* resources = nullptr,
    bool extractItemMaterialState = true, uint64_t* staticRevision = nullptr, renderer::RenderScene* reusableStorage = nullptr,
    bool fuseColorEligibility = false, SkinnedObjectRange* skinnedObjects = nullptr)
{
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Geometry");
    if (skinnedObjects) *skinnedObjects = {};
    fuseColorEligibility = fuseColorEligibility && !extractItemMaterialState;
    if (fuseColorEligibility && profiler::Profiler::IsEnabled())
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::FusedColorEligibility");
    renderer::RenderScene output;
    if (reusableStorage) {
        output.items.swap(reusableStorage->items);
        output.objects.swap(reusableStorage->objects);
    }
    output.snapshotSerial = ++s_renderSceneSerial;
    output.sceneGeneration = scene.GetRenderSceneGeneration();
    output.frameStamp = frameStamp;
    const auto lodRenderers = rayTracingInputs ? CollectRayLodRenderers(scene, output.rayLodDiagnostics)
        : std::unordered_map<uint64_t, RayLodSelection>{};
    std::optional<std::unordered_map<uint64_t, RayLodSelection>> bakeLodRenderers;
    const auto objectsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::GeometryObjects", "Rendering"));
    auto& candidates = s_renderGeometryCandidates;
    scene.CopyRendererEntities(candidates);
    DenseGeometryBatch denseGeometry(output, candidates.size());
    auto& staticGeometry = *FindStaticGeometry(scene, true);
    SpatialUpdateBuilder spatialUpdates(staticGeometry);
    const bool denseOnly = staticGeometry.primed && staticGeometry.denseBuildsRemaining > 0;
    if (denseOnly) {
        --staticGeometry.denseBuildsRemaining;
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::DenseGeometryFallback");
    } else if (!staticGeometry.primed) {
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::ColdGeometry");
    }
    const bool reuseStaticGeometry = staticGeometry.primed && !denseOnly;
    const auto& staticMeshes = detail::ComponentStorageAccess<MeshRenderer>::Get(scene);
    if (reuseStaticGeometry) {
        const size_t meshCount = staticMeshes.Count();
        if (staticGeometry.entries.size() > meshCount) staticGeometry.entries.resize(meshCount);
        if (staticGeometry.entries.capacity() < meshCount) staticGeometry.entries.reserve(meshCount);
    }
    size_t staticCount = 0;
    for (EntityID id : candidates) {
        auto* source = scene.GetGameObject(id);
        if (!source) continue;
        auto& go = *source;
        if (!go.activeInHierarchy()) continue;
        std::optional<bool> colorEligibility;
        const auto applyColorEligibility = [&](renderer::RenderObject& object) {
            if (!fuseColorEligibility) return;
            if (!colorEligibility.has_value()) {
                const auto* material = go.GetComponent<MaterialComponent>();
                colorEligibility = !material || material->enabled;
            }
            object.colorEligible = *colorEligibility;
        };
        if (auto* mr = go.GetComponent<MeshRenderer>(); mr && mr->enabled && mr->mesh && !mr->mesh->isSkinned) {
            bool boundsChanged = true;
            auto object = reuseStaticGeometry
                ? ExtractStaticObject(staticMeshes, staticGeometry, go, *mr, frameStamp, output, denseGeometry, boundsChanged)
                : ExtractUncachedStaticObject(go, *mr, frameStamp, output, denseGeometry);
            ++staticCount;
            if (const auto lod = lodRenderers.find(EntityKey(go.GetID())); lod != lodRenderers.end()) {
                object.rayVisible = lod->second.visible;
                object.rayLodSelectionRequired = lod->second.ambiguous;
            }
            if (mr->bakeStaticShadow) {
                if (!rayTracingInputs && !bakeLodRenderers) {
                    std::vector<renderer::RenderRayLodDiagnostic> unusedDiagnostics;
                    bakeLodRenderers = CollectRayLodRenderers(scene, unusedDiagnostics);
                }
                const auto& bakeLods = rayTracingInputs ? lodRenderers : *bakeLodRenderers;
                object.shadowBakeStatic = !bakeLods.contains(EntityKey(go.GetID()));
                if (object.shadowBakeStatic)
                    output.shadowBakeMeshObjects.push_back(static_cast<uint32_t>(output.objects.size()));
            }
            if (spatialUpdates.Enabled())
                spatialUpdates.Append(id, staticMeshes.GetLifetime(id), false, boundsChanged);
            applyColorEligibility(object);
            output.objects.push_back(object);
        } else if (reuseStaticGeometry && mr && mr->enabled) {
            /// @note メッシュの解除やスキン付きへの切り替えを観測したら、静的形状の再利用を打ち切る。
            const size_t slot = staticMeshes.GetDenseIndex(id);
            if (slot < staticGeometry.entries.size()) staticGeometry.entries[slot].entity = EntityID::INVALID;
        }
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->enabled || !smr->model) continue;
        auto object = ExtractObject(go, *smr, frameStamp);
        if (const auto lod = lodRenderers.find(EntityKey(go.GetID())); lod != lodRenderers.end()) {
            object.rayVisible = lod->second.visible;
            object.rayLodSelectionRequired = lod->second.ambiguous;
        }
        object.skinned = true;
        object.leafWindBoundsPadding = ComputeLeafWindBoundsPadding(go, smr->SubmeshCount());
        object.firstItem = static_cast<uint32_t>(output.items.size());
        /// @note No callbacks run in this object traversal; every submesh uses the same current transform without retaining it across views.
        const float maximumScale = ComputeWorldBoundsScale(object.world);
        WorldBounds bounds{};
        if (ComputeSkinnedWorldBounds(go, *smr, bounds)) {
            object.boundsCenter = bounds.center;
            object.boundsRadius = bounds.radius;
        }
        const auto* animator = FindAnimator(go);
        object.skinningPalette = ResolveSkinningCB(animator ? animator->skinningBuffer.Get()
            : renderer::ResourceHandle<renderer::ConstantBufferTag>{}, smr->model, identityPalette);
        object.previousSkinningValid = animator && animator->prevBoneMatricesValid &&
            animator->skinningBuffer.IsValid() && animator->prevSkinningBuffer.IsValid();
        object.previousSkinningPalette = object.previousSkinningValid
            ? animator->prevSkinningBuffer.Get() : object.skinningPalette;
        if (animator) {
            const auto& bake = animator->boneBake;
            if (resources && bake.active && bake.gpuFrame == resources->FrameStamp()
                && bake.gpuOwner == resources->GetInstanceId() && bake.gpuEpoch == resources->GetResetVersion()) {
                object.gpuBones = {bake.gpuPalette, bake.gpuPreviousPalette, bake.gpuOffset, bake.gpuPreviousOffset};
            }
            /// @note A source retired during a resource callback has no current draw input; never present its deferred CPU seed.
            if (bake.cpuDeferred && !object.gpuBones.current.IsValid()) { object.lodVisible = false; object.rayVisible = false; }
        }
        for (size_t slot = 0; slot < smr->SubmeshCount(); ++slot) {
            const auto* mesh = smr->SubmeshMesh(slot);
            if (!mesh) continue;
            auto& item = denseGeometry.Append(*mesh, object.world, maximumScale);
            item.objectIndex = static_cast<uint32_t>(output.objects.size());
            item.materialSlot = static_cast<uint32_t>(slot);
            item.sourceSubmesh = smr->SubmeshAt(slot);
            item.skinningVertexBuffer = smr->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer);
            /// @note 古いフレームやモーフ対象に残った GPU 出力を再利用しない。
            if (smr->gpuSkinningFrame == frameStamp && smr->skinnedBufferModel == smr->model)
                item.deformedVertexBuffer = smr->ResolveSlotSkinnedVertexBuffer(slot);
        }
        object.itemCount = static_cast<uint32_t>(output.items.size()) - object.firstItem;
        if (spatialUpdates.Enabled())
            spatialUpdates.Append(id, scene.GetComponentLifetime<SkinnedMeshRenderer>(id), true, true);
        applyColorEligibility(object);
        if (skinnedObjects) skinnedObjects->Append(output.objects.size());
        output.objects.push_back(object);
    }
    if (reuseStaticGeometry) {
        /// @note Two high-change observations avoid penalizing one teleport; bounded dense builds amortize tracking in continuously volatile Scenes.
        const bool highChange = staticCount >= 64 && denseGeometry.ExistingChanges() >= staticCount - staticCount / 4;
        if (highChange && staticGeometry.highChangeLastBuild) {
            staticGeometry.denseBuildsRemaining = 16;
            staticGeometry.highChangeLastBuild = false;
        } else staticGeometry.highChangeLastBuild = highChange;
    }
    staticGeometry.primed = true;
    spatialUpdates.Publish(output);
    if (staticRevision) *staticRevision = staticGeometry.revision;
    candidates.clear();
    profiler::Profiler::EndScope(objectsProfile);
    const auto materialCapabilitiesProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::GeometryMaterialCapabilities", "Rendering"));
    if (!fuseColorEligibility) {
        for (auto& object : output.objects) {
            auto* go = scene.GetGameObject({ object.sourceIndex, object.sourceGeneration });
            const auto* component = go ? go->GetComponent<MaterialComponent>() : nullptr;
            if (!component) {
                object.colorEligible = true;
                continue;
            }
            object.colorEligible = component->enabled;
            /// @note Full extraction derives every item's current state after EnsureMaterialAsset; standalone geometry still publishes it here.
            if (!extractItemMaterialState) continue;
            for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
                auto& item = output.items[i];
                const auto& slot = component->SlotAt(item.materialSlot);
                item.slotVisible = slot.visible;
                item.visible = component->enabled && slot.visible;
                item.material.capabilities = materialCache.Extract(slot);
            }
        }
    }
    profiler::Profiler::EndScope(materialCapabilitiesProfile);
    return output;
}

/// @note Only geometry crosses the view boundary; material bindings, visibility and selection start fresh in each output.
RenderFrameGeometryItem CompactGeometryItem(const renderer::RenderMeshItem& item)
{
    return {item.objectIndex, item.materialSlot, item.sourceSubmesh,
        item.vertexBuffer, item.indexBuffer, item.skinningVertexBuffer, item.deformedVertexBuffer,
        item.preparedSkinning, item.preparedSkinningWrite, item.indexCount, item.vertexCount,
        item.vertexContentVersion, item.indexContentVersion, item.deformedContentVersion,
        item.vertexStride, item.vertexPositionOffset, item.reliableOccluder, item.boundsCenter, item.boundsRadius,
        item.occluderRadius, item.clusterLod};
}

/// @pre item retains default per-view state; only geometry fields may have been restored into it.
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

/// @note A caller-replaced geometry owner cannot inherit compact rows or a deferred source from the previous publication.
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
        && source->sharedObjects == geometry->sharedObjects && source->items.size() == itemCount;
}

/// @note Static builders declare Vertex layout and no content versions/tickets; later binding repair may observe Ray settings changed by a callback.
RenderFrameGeometryItem CompactStaticRasterGeometryItem(const renderer::RenderMeshItem& item)
{
    return {item.objectIndex, item.materialSlot, item.sourceSubmesh,
        item.vertexBuffer, item.indexBuffer, item.skinningVertexBuffer, {}, {}, UINT32_MAX,
        item.indexCount, item.vertexCount, 0, 0, 0, sizeof(renderer::Vertex), 0,
        item.reliableOccluder, item.boundsCenter, item.boundsRadius, item.occluderRadius, item.clusterLod};
}

/// @note Only static Raster seeds are deferred; material/selection state and any later Ray binding-repair metadata are never copied.
/// @note Consume the owned source before any view callback; no cache entry or source item reference crosses a callback boundary.
void MaterializeFrameGeometryItems(RenderFrameGeometryCache& cache)
{
    if (!cache.deferredItemGeometry) return;
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::GeometryCacheMaterialize");
    auto source = std::move(cache.deferredItemGeometry);
    cache.itemGeometry.reserve(source->items.size());
    for (const auto& item : source->items)
        cache.itemGeometry.push_back(CompactStaticRasterGeometryItem(item));
}

/// @note The old restore stays the default. Only an explicit comparison opts into static Raster prototype reuse.
bool UseGeometryRestoreReuse()
{
    const char* mode = std::getenv("FBZZ_RENDER_GEOMETRY_RESTORE_MODE");
    return mode && std::strcmp(mode, "reuse") == 0;
}

/// @note The private prototype never receives per-view material writes or crosses a callback boundary.
void RestoreFrameGeometryItems(const RenderFrameGeometryCache& cache, renderer::RenderScene& output)
{
    renderer::RenderMeshItem prototype;
    output.items.reserve(cache.itemGeometry.size());
    for (const auto& item : cache.itemGeometry) {
        RestoreGeometryItemInto(item, prototype);
        output.items.push_back(prototype);
    }
}

/// @note Explicit diagnostic only. The requested filter can transition one frame before collection;
/// @note BeginScope still applies the active filter and recording state, so no mismatched sample is emitted.
bool CollectFrameGeometryDiagnostics()
{
    if (!profiler::Profiler::IsEnabled()) return false;
    const auto filter = profiler::Profiler::GetRequestedCaptureFilter();
    return filter.scope == "RenderSystem::FrameGeometry"
        && (filter.category.empty() || filter.category == "Rendering");
}
/// @note Color eligibility is stored with the shared pose, but MaterialComponent remains public mutable
/// @note per-view state. Rebuild on either enabled transition (or an invalid owner) without changing any
/// @note retained snapshot. Missing MaterialComponent matches the geometry builder's eligible default.
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
        /// @note Leaf authoring can change between views without a pose notification; the frozen witness also observes asset edits and unresolved-to-ready transitions.
        const auto* skin = object.skinned ? go->GetComponent<SkinnedMeshRenderer>() : nullptr;
        if (ComputeLeafWindBoundsPadding(*go, skin ? skin->SubmeshCount() : 1) != object.leafWindBoundsPadding) return false;
    }
    return true;
}
} /// @note namespace

void NotifyMeshRendererChanged(Scene& scene, EntityID entity)
{
    if (!entity.IsValid()) return;
    if (auto* cache = FindStaticGeometry(scene, false)) cache->revision = ++s_staticGeometryRevision;
}


renderer::RenderScene ExtractRenderSceneGeometry(Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette, bool rayTracingInputs)
{
    GeometryMaterialCache materialCache;
    return BuildRenderSceneGeometry(scene, frameStamp, identityPalette, rayTracingInputs, materialCache);
}

float EstimateRenderTexturePixels(const renderer::RenderObject& object,
    const renderer::RenderMeshItem& item, const math::Vector3& cameraPosition,
    float projectionScaleY, uint32_t height, bool orthographic)
{
    const auto center = object.skinned ? object.boundsCenter : item.boundsCenter;
    const float radius = object.skinned ? object.boundsRadius : item.boundsRadius;
    if (!(projectionScaleY > 0.0f) || !(radius > 0.0f) || height == 0) return 0.0f;
    const float distance = (center - cameraPosition).Length();
    if (!orthographic && distance <= radius) return static_cast<float>(height);
    return radius * projectionScaleY * static_cast<float>(height) / (orthographic ? 1.0f : distance);
}

namespace {
bool CurrentRayMaterialInputs(const RenderPassContext& ctx)
{
    return ctx.experimentalRayTracingEnabled && (ctx.settings.modeRequest.mode != renderer::RenderMode::RASTER
        || renderer::IsRayDebugView(ctx.settings.viewMode));
}

bool LeafWindBoundsMayHaveExpanded(Scene& scene, const renderer::RenderObject& object)
{
    if (!(object.boundsRadius > 0.0f)) return false;
    auto* owner = scene.GetGameObject({object.sourceIndex, object.sourceGeneration});
    if (!owner) return false;
    const auto* skin = object.skinned ? owner->GetComponent<SkinnedMeshRenderer>() : nullptr;
    const float padding = ComputeLeafWindBoundsPadding(*owner, skin ? skin->SubmeshCount() : 1);
    return padding < 0.0f || padding > object.leafWindBoundsPadding;
}

/// @note Importer/native callbacks may grow another material after geometry was frozen; detach only affected publications and keep older views immutable.
void FailOpenChangedLeafWindBounds(Scene& scene, renderer::RenderScene& snapshot)
{
    const auto objects = snapshot.Objects();
    std::vector<renderer::RenderObject> detached;
    bool changed = false;
    for (size_t ordinal = 0; ordinal < objects.size(); ++ordinal) {
        const auto& object = objects[ordinal];
        if (!LeafWindBoundsMayHaveExpanded(scene, object)) continue;
        if (!changed) detached.assign(objects.begin(), objects.end());
        changed = true;
        detached[ordinal].boundsRadius = 0.0f;
        for (uint32_t index = object.firstItem; index < object.firstItem + object.itemCount; ++index) {
            snapshot.items[index].boundsRadius = 0.0f;
            snapshot.items[index].occluderRadius = 0.0f;
        }
    }
    if (!changed) return;
    snapshot.objects = std::move(detached);
    snapshot.sharedObjects.reset();
    /// @note The retained geometry journal certifies its original spheres; no old dirty-set may describe this fail-open publication.
    snapshot.spatialUpdate.reset();
}

bool NeedsMaterialBindings(const material_demand_detail::View& view, const renderer::RenderScene& snapshot,
                           std::span<const uint32_t> items, Scene& scene)
{
    for (uint32_t index : items) {
        if (index >= snapshot.items.size()) return true;
        const auto& item = snapshot.items[index];
        if (item.objectIndex >= snapshot.Objects().size()) return true;
        if (LeafWindBoundsMayHaveExpanded(scene, snapshot.Objects()[item.objectIndex])) return true;
        const auto result = material_demand_detail::Evaluate(view, snapshot.Objects()[item.objectIndex], item);
        if (result.reasons != 0 || (!result.cameraEvaluated && result.matchesView)) return true;
    }
    return false;
}

/// @note Repair visits every omitted row once without demand tests; its callbacks cannot trigger an unbounded retry or retain borrowed material state.
void RestoreMaterialBindings(RenderPassContext& ctx, renderer::RenderScene& snapshot, std::span<const uint32_t> items,
                             MaterialDemandTraceScope* trace = nullptr)
{
    GeometryMaterialCache cache(&ctx.resources);
    const bool rayInputs = CurrentRayMaterialInputs(ctx);
    if (trace) {
        auto& row = trace->Counters();
        ++row.bindingFallbacks;row.restoredBindingItems += items.size();row.omittedBindingItems -= items.size();
        row.provenNoDemandItems -= items.size();row.conservativeUnionItems += items.size();row.unknownConsumerItems += items.size();
        row.consumerScopeCertified = 0;
    }
    for (uint32_t index : items) {
        if (index >= snapshot.items.size()) continue;
        auto& item = snapshot.items[index];
        if (item.objectIndex >= snapshot.Objects().size()) continue;
        const auto& object = snapshot.Objects()[item.objectIndex];
        auto* owner = ctx.scene.GetGameObject({object.sourceIndex, object.sourceGeneration});
        auto* component = owner ? owner->GetComponent<MaterialComponent>() : nullptr;
        std::optional<MaterialComponent> unassigned;
        if (!component) component = &unassigned.emplace();
        component->EnsureMaterialAsset(item.materialSlot);
        auto& slot = component->SlotAt(item.materialSlot);
        item.slotVisible = slot.visible;item.visible = component->enabled && slot.visible;
        auto& material = item.material;
        cache.ApplyState(slot, material);
        if (!object.colorEligible || !item.visible) continue;
        const bool projected = !ctx.isDeferred || renderer::ResolveGeometryRoute(material.capabilities, true) == renderer::GeometryRoute::GBuffer;
        const float pixels = projected ? EstimateRenderTexturePixels(object, item, ctx.camera.m_position,
            ctx.cullProjScaleY, ctx.height, ctx.cullOrthographic) : 0.0f;
        if (trace) {
            if (projected) ++trace->Counters().projectedTextureRequestItems;
            if (pixels > 0) ++trace->Counters().positiveScreenPixelRequests;
        }
        if (!rayInputs && cache.CopySynchronized(slot, ctx.resources, object.skinned, pixels, material)) {
            item.forwardMaterial = material;
            if (trace) {
                ++trace->Counters().synchronizedReuseItems;
                if (material.valid) trace->Counters().canonicalCopiedLogicalBytes += material.gbufferParams.size();
            }
            continue;
        }
        const auto* prepared = cache.Prepare(*component, item.materialSlot, ctx.resources, object.skinned);
        const auto* source = SyncMaterialSlotPrepared(*component, item.materialSlot, ctx.resources, object.skinned, pixels, prepared);
        if (trace) {
            ++trace->Counters().syncAttempts;
            if (source) {
                ++trace->Counters().syncNonNullItems;trace->Counters().syncReturnedParameterBytes += source->paramData.size();
                if (renderer::IsErrorShader(source->shaderPath)) ++trace->Counters().errorFallbackItems;
            }
        }
        CopyMaterial(material, source, ctx.resources, component->SlotAt(item.materialSlot), cache);
        material.capabilities.gbufferEquivalentShader = ctx.resources.GetShaderCapabilities(material.shader).SupportsGBuffer();
        if (rayInputs) {
            const auto* vertices = ctx.resources.Get(item.vertexBuffer);
            const auto* indices = ctx.resources.Get(item.indexBuffer);
            const auto* deformed = ctx.resources.Get(item.deformedVertexBuffer);
            item.vertexContentVersion = vertices ? vertices->GetContentVersion() : 0;
            item.indexContentVersion = indices ? indices->GetContentVersion() : 0;
            item.deformedContentVersion = deformed ? deformed->GetContentVersion() : 0;
            if (vertices) item.vertexStride = vertices->GetStride();
            const bool verified = object.skinned && deformed && item.deformedContentVersion != 0
                && deformed->GetStride() == sizeof(renderer::Vertex);
            ExtractRayMaterialInputs(material, source, component->SlotAt(item.materialSlot), ctx.resources, verified);
        } else cache.RememberSynchronized(component->SlotAt(item.materialSlot), ctx.resources, object.skinned, pixels, material);
        item.forwardMaterial = material;
        if (trace && material.valid) trace->Counters().canonicalCopiedLogicalBytes += material.gbufferParams.size();
    }
}
} /// @note namespace

namespace material_demand_detail {
void CompleteBindings(RenderPassContext& context)
{
    if (s_bindingCompletionDepth != 0 || s_bindingDemandContext != &context
        || !s_bindingDemandScope || !s_bindingDemandScope->m_pending) return;
    auto& pending = *s_bindingDemandScope->m_pending;
    if (context.renderScene != pending.snapshot) { s_bindingDemandScope->m_pending.reset();return; }
    const auto view = CurrentMaterialDemandView(context, CurrentRayMaterialInputs(context), true);
    const bool changed = pending.resourceOwner != context.resources.GetInstanceId()
        || pending.resourceEpoch != context.resources.GetResetVersion() || pending.resourceFrame != context.resources.FrameStamp()
        || pending.snapshot->sceneGeneration != context.scene.GetRenderSceneGeneration()
        || pending.assetGeneration != asset::AssetManager::GetAssetGeneration()
        || std::any_of(pending.items.begin(), pending.items.end(), [&](uint32_t index) {
            return index < pending.snapshot->items.size()
                && pending.snapshot->items[index].objectIndex < pending.snapshot->Objects().size()
                && LeafWindBoundsMayHaveExpanded(context.scene, pending.snapshot->Objects()[pending.snapshot->items[index].objectIndex]);
        });
    if (!changed) {
        const auto witness = CaptureBindingDemandWitness(view);
        if (pending.noDemandWitness && *pending.noDemandWitness == witness) return;
        if (!NeedsMaterialBindings(view, *pending.snapshot, pending.items, context.scene)) {
            pending.noDemandWitness = witness;
            return;
        }
    }
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::CompleteMaterialBindings");
    /// @note Detach before resource callbacks; nested completion cannot repair the same publication twice.
    auto owned = std::move(s_bindingDemandScope->m_pending);
    const auto preparedItems = FindPreparedSnapshotItems(owned->snapshot);
    auto completed = std::make_shared<renderer::RenderScene>(*owned->snapshot);
    completed->snapshotSerial = ++s_renderSceneSerial;
    renderer::material_transfer_detail::MaterialTransferPayloadScope materialTransferPayloads(context.resources);
    CompletionDepthScope completionDepth;
    FailOpenChangedLeafWindBounds(context.scene, *completed);
    RestoreMaterialBindings(context, *completed, owned->items);
    FailOpenChangedLeafWindBounds(context.scene, *completed);
    if (preparedItems.known) RememberPreparedSnapshotItems(completed, preparedItems.indices);
    context.renderScene = std::move(completed);
}
} /// @note namespace material_demand_detail

std::shared_ptr<const renderer::RenderScene> CompletePreparedSkinningSnapshot(RenderPassContext& ctx)
{
    if (!ctx.renderScene) return {};
    const auto completedOutput = [](const renderer::RenderMeshItem& item) {
        if (!item.preparedSkinning) return renderer::PreparedSkinningWrite{};
        const auto writes = item.preparedSkinning->Writes();
        return item.preparedSkinningWrite < writes.size() && item.preparedSkinning->IsWriteCurrent(item.preparedSkinningWrite) ? writes[item.preparedSkinningWrite]
            : renderer::PreparedSkinningWrite{};
    };
    const auto indexed = FindPreparedSnapshotItems(ctx.renderScene);
    if (indexed.known && !indexed.indices) return {};
    std::vector<uint32_t> discovered;
    if (!indexed.known) {
        for (size_t i = 0; i < ctx.renderScene->items.size(); ++i)
            if (ctx.renderScene->items[i].preparedSkinning) discovered.push_back(static_cast<uint32_t>(i));
    }
    const std::span<const uint32_t> indices = indexed.known ? std::span<const uint32_t>(*indexed.indices)
        : std::span<const uint32_t>(discovered);
    bool changed = false;
    for (uint32_t index : indices) {
        const auto& item = ctx.renderScene->items[index];
        const auto write = completedOutput(item);
        if (item.deformedVertexBuffer != (write.recorded ? write.output : renderer::ResourceHandle<renderer::BufferTag>{})
            || item.deformedContentVersion != (write.recorded ? write.resultingVersion : 0)) { changed = true; break; }
    }
    if (!changed) return {};
    auto previous = ctx.renderScene;
    auto completed = std::make_shared<renderer::RenderScene>(*previous);
    completed->snapshotSerial = ++s_renderSceneSerial;
    for (uint32_t index : indices) {
        auto& item = completed->items[index];
        const auto write = completedOutput(item);
        item.deformedVertexBuffer = write.recorded ? write.output : renderer::ResourceHandle<renderer::BufferTag>{};
        item.deformedContentVersion = write.recorded ? write.resultingVersion : 0;
    }
    RememberPreparedSnapshotItems(completed, indexed.known ? indexed.indices : OwnPreparedSnapshotItems(std::move(discovered)));
    ctx.renderScene = completed;
    material_demand_detail::RetargetBindings(previous, completed);
    return previous;
}

void ExtractRenderScene(RenderPassContext& ctx, RenderFrameGeometryCache* frameGeometry)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::ExtractRenderScene");
    material_demand_detail::ExtractionDepthScope extractionDepth;
    renderer::material_transfer_detail::MaterialTransferPayloadScope materialTransferPayloads(ctx.resources);
    const uint64_t resourceFrame = ctx.resources.FrameStamp();
    ctx.frameStamp = resourceFrame;
    ctx.time = Time::time;
    ctx.deltaTime = Time::deltaTime;
    ctx.unscaledDeltaTime = Time::unscaledDeltaTime;
    MaterialDemandTraceScope materialDemandTrace(ctx);

    GeometryMaterialCache materialCache(&ctx.resources);
    std::shared_ptr<renderer::RenderScene> output;
    const uint64_t resetVersion = ctx.resources.GetResetVersion();
    const bool rayTracingInputs = CurrentRayMaterialInputs(ctx);
    const bool fusedScans = UseFusedExtractionScans();
    SkinnedObjectRange skinnedObjects;
    bool skinnedObjectsKnown = false;
    const auto* staticGeometry = FindStaticGeometry(ctx.scene, false);
    const uint64_t staticRevision = staticGeometry ? staticGeometry->revision : 0;
    if (staticGeometry && frameGeometry && frameGeometry->geometry && frameGeometry->scene == &ctx.scene &&
        frameGeometry->geometry->sceneGeneration == ctx.scene.GetRenderSceneGeneration() &&
        frameGeometry->resources == &ctx.resources && frameGeometry->frameStamp == Time::frameCount &&
        frameGeometry->resetVersion == resetVersion &&
        frameGeometry->staticGeometryRevision == staticRevision &&
        frameGeometry->rayTracingInputs == rayTracingInputs &&
        frameGeometry->identityPalette == ctx.handles.bindPoseSkinningCB &&
        CachedGeometryItemsMatch(*frameGeometry) &&
        CachedGeometryColorEligibilityMatches(ctx.scene, *frameGeometry->geometry)) {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::CachedGeometryInputs");
        const bool reusePrototype = fusedScans && !rayTracingInputs
            && frameGeometry->skinnedObjectRangeOwner == frameGeometry->geometry.get()
            && frameGeometry->skinnedObjectRangeSerial == frameGeometry->geometry->snapshotSerial
            && frameGeometry->skinnedObjectBegin == frameGeometry->skinnedObjectEnd && UseGeometryRestoreReuse();
        MaterializeFrameGeometryItems(*frameGeometry);
        output = MakeRenderSnapshot();
        output->sharedObjects = frameGeometry->geometry->sharedObjects;
        output->spatialUpdate = frameGeometry->geometry->spatialUpdate;
        output->shadowBakeMeshObjects = frameGeometry->geometry->shadowBakeMeshObjects;
        if (reusePrototype) {
            RestoreFrameGeometryItems(*frameGeometry, *output);
        } else {
            output->items.reserve(frameGeometry->itemGeometry.size());
            for (const auto& item : frameGeometry->itemGeometry)
                output->items.push_back(RestoreGeometryItem(item));
        }
        output->rayLodDiagnostics = frameGeometry->geometry->rayLodDiagnostics;
        output->sceneGeneration = frameGeometry->geometry->sceneGeneration;
        output->frameStamp = frameGeometry->geometry->frameStamp;
        output->snapshotSerial = ++s_renderSceneSerial;
        if (fusedScans && frameGeometry->skinnedObjectRangeOwner == frameGeometry->geometry.get()
            && frameGeometry->skinnedObjectRangeSerial != 0
            && frameGeometry->skinnedObjectRangeSerial == frameGeometry->geometry->snapshotSerial) {
            skinnedObjects = {frameGeometry->skinnedObjectBegin, frameGeometry->skinnedObjectEnd};
            skinnedObjectsKnown = true;
        }
    } else {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::GeometryBuild");
        uint64_t builtStaticRevision = 0;
        output = MakeRenderSnapshot();
        *output = BuildRenderSceneGeometry(ctx.scene, Time::frameCount, ctx.handles.bindPoseSkinningCB,
            rayTracingInputs, materialCache, &ctx.resources, false, &builtStaticRevision, output.get(),
            fusedScans, fusedScans ? &skinnedObjects : nullptr);
        skinnedObjectsKnown = fusedScans;
        if (frameGeometry) {
            FBZZ_PROFILE_SCOPE("RenderSceneExtractor::GeometryCachePublish");
            frameGeometry->scene = &ctx.scene;
            frameGeometry->resources = &ctx.resources;
            frameGeometry->frameStamp = Time::frameCount;
            frameGeometry->resetVersion = resetVersion;
            frameGeometry->staticGeometryRevision = builtStaticRevision;
            frameGeometry->rayTracingInputs = rayTracingInputs;
            frameGeometry->identityPalette = ctx.handles.bindPoseSkinningCB;
            auto geometry = std::make_shared<renderer::RenderScene>();
            output->sharedObjects = FreezeRenderObjects(std::move(output->objects));
            geometry->sharedObjects = output->sharedObjects;
            geometry->spatialUpdate = output->spatialUpdate;
            geometry->shadowBakeMeshObjects = output->shadowBakeMeshObjects;
            frameGeometry->itemGeometry.clear();
            frameGeometry->deferredItemGeometry.reset();
            /// @note Skinned inputs and Ray metadata can change before material callbacks; retain their original eager capture timing.
            if (fusedScans && !rayTracingInputs && skinnedObjects.begin == skinnedObjects.end && !output->items.empty()) {
                frameGeometry->deferredItemGeometry = output;
            } else {
                frameGeometry->itemGeometry.reserve(output->items.size());
                for (const auto& item : output->items)
                    frameGeometry->itemGeometry.push_back(CompactGeometryItem(item));
            }
            geometry->rayLodDiagnostics = output->rayLodDiagnostics;
            geometry->snapshotSerial = output->snapshotSerial;
            geometry->sceneGeneration = output->sceneGeneration;
            geometry->frameStamp = output->frameStamp;
            frameGeometry->itemGeometryOwner = geometry.get();
            frameGeometry->itemGeometrySerial = geometry->snapshotSerial;
            frameGeometry->skinnedObjectRangeOwner = skinnedObjectsKnown ? geometry.get() : nullptr;
            frameGeometry->skinnedObjectRangeSerial = skinnedObjectsKnown ? geometry->snapshotSerial : 0;
            frameGeometry->skinnedObjectBegin = skinnedObjects.begin;
            frameGeometry->skinnedObjectEnd = skinnedObjects.end;
            frameGeometry->geometry = std::move(geometry);
        }
    }
    /// @note 後から開いた Ray view が LOD0 を追加変形しても、最初の Raster view の snapshot に閉じ込めない。
    std::vector<uint32_t> preparedItems;
    const auto meshInputsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::MeshInputs", "Rendering"));
    const auto objects = output->Objects();
    const bool validSkinnedRange = skinnedObjectsKnown && skinnedObjects.begin <= skinnedObjects.end
        && skinnedObjects.end <= objects.size();
    const size_t skinnedBegin = validSkinnedRange ? skinnedObjects.begin : 0;
    const size_t skinnedEnd = validSkinnedRange ? skinnedObjects.end : objects.size();
    if (validSkinnedRange && profiler::Profiler::IsEnabled())
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::SkinnedRangeInputs");
    /// @note Static and skinned objects can interleave by Entity; retain the skinned check inside the certified enclosing range.
    for (size_t ordinal = skinnedBegin; ordinal < skinnedEnd; ++ordinal) {
        const auto& object = objects[ordinal];
        if (!object.skinned) continue;
        auto* go = ctx.scene.GetGameObject({object.sourceIndex, object.sourceGeneration});
        const auto* smr = go ? go->GetComponent<SkinnedMeshRenderer>() : nullptr;
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            auto& item = output->items[i];
            item.preparedSkinning.reset();
            item.preparedSkinningWrite = UINT32_MAX;
            item.deformedVertexBuffer = smr && smr->gpuSkinningFrame == Time::frameCount
                && smr->skinnedBufferModel == smr->model ? smr->ResolveSlotSkinnedVertexBuffer(item.materialSlot)
                : renderer::ResourceHandle<renderer::BufferTag>{};
            if (smr && smr->preparedSkinning && smr->preparedSkinningFrame == Time::frameCount
                && smr->skinnedBufferModel == smr->model
                && item.sourceSubmesh < smr->preparedSkinningRows.size()
                && smr->SubmeshAt(item.materialSlot) == item.sourceSubmesh
                && smr->SubmeshMesh(item.materialSlot)
                && smr->SubmeshMesh(item.materialSlot)->cpuSkinnedVertices.size() == item.vertexCount
                && smr->ResolveSlotVertexBuffer(item.materialSlot, smr->SubmeshMesh(item.materialSlot)->vertexBuffer) == item.skinningVertexBuffer) {
                const uint32_t row = smr->preparedSkinningRows[item.sourceSubmesh];
                const auto writes = smr->preparedSkinning->Writes();
                if (row < writes.size()) {
                    item.preparedSkinning = smr->preparedSkinning;
                    item.preparedSkinningWrite = row;
                    preparedItems.push_back(i);
                    const bool current = writes[row].recorded && smr->preparedSkinning->IsWriteCurrent(row);
                    item.deformedVertexBuffer = current ? writes[row].output : renderer::ResourceHandle<renderer::BufferTag>{};
                    item.deformedContentVersion = current ? writes[row].resultingVersion : 0;
                }
            }
        }
    }
    /// @note Buffer shadows and content generations are Ray inputs. Raster already has its declared stride and must not drain the queued RHI frame to inspect unused CPU metadata.
    if (rayTracingInputs) {
        for (auto& item : output->items) {
            const auto* vertices = ctx.resources.Get(item.vertexBuffer);
            const auto* indices = ctx.resources.Get(item.indexBuffer);
            const auto* deformed = ctx.resources.Get(item.deformedVertexBuffer);
            item.deformedContentVersion = deformed ? deformed->GetContentVersion() : 0;
            item.vertexContentVersion = vertices ? vertices->GetContentVersion() : 0;
            item.indexContentVersion = indices ? indices->GetContentVersion() : 0;
            if (vertices) item.vertexStride = vertices->GetStride();
        }
    }
    profiler::Profiler::EndScope(meshInputsProfile);

    const auto materialsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::Materials", "Rendering"));
    bool collectMaterialPhases=false;
    if(profiler::Profiler::IsEnabled()){
        const auto requestedFilter=profiler::Profiler::GetRequestedCaptureFilter();
        /// @note The requested scope must match exactly; external samples still obey the current frame's applied filter.
        collectMaterialPhases=requestedFilter.scope=="RenderSceneExtractor::Materials"
            && (requestedFilter.category.empty() || requestedFilter.category=="Rendering");
    }
    int64_t stateSlotsNs=0,projectionReuseNs=0,parameterPreparationNs=0,syncCopyTexturesNs=0,rayInputsNs=0;
    if (materialDemandTrace.Active()) {
        auto& row = materialDemandTrace.Counters();
        row.snapshotSerial = output->snapshotSerial;
        row.renderObjects = output->Objects().size();
        row.renderItems = output->items.size();
    }
    auto initialDemandView = CurrentMaterialDemandView(ctx, rayTracingInputs);
    std::optional<math::Frustum> initialDemandFrustum;
    if (initialDemandView.culling.frustum) {
        initialDemandFrustum = *initialDemandView.culling.frustum;
        initialDemandView.culling.frustum = &*initialDemandFrustum;
    }
    /// @note Share only the frozen extraction view; final and consumer-boundary repair still evaluate the current view independently.
    const material_demand_detail::PreparedView preparedDemandView(initialDemandView);
    std::vector<uint32_t> omittedBindings;
    const auto processMaterials=[&]<bool DIAGNOSTICS_ENABLED, bool DEMAND_TRACE_ENABLED>(){
        MaterialPhaseTimer<DIAGNOSTICS_ENABLED> setupTimer(stateSlotsNs);
        const bool hasSelection = !ctx.settings.selectedObjects.empty();
        if (hasSelection) output->objectSelection.reserve(output->Objects().size());
        setupTimer.Finish();
        for (const auto& object : output->Objects()) {
            material_demand_detail::ObjectEvaluator demandEvaluator(preparedDemandView, object);
            MaterialPhaseTimer<DIAGNOSTICS_ENABLED> objectStateTimer(stateSlotsNs);
            auto* go = ctx.scene.GetGameObject({ object.sourceIndex, object.sourceGeneration });
            if (hasSelection) output->objectSelection.push_back(go && IsSelectedForOutline(*go, ctx.settings));
            auto* component = go ? go->GetComponent<MaterialComponent>() : nullptr;
            /// @note Component 自体が無い場合も未割当として可視化し、シーンへ自動追加しない。
            std::optional<MaterialComponent> unassigned;
            if (!component) component = &unassigned.emplace();
            /// @note Material の解決失敗はスロット単位で代替し、他の submesh を巻き添えにしない。
            objectStateTimer.Finish();
            for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
                MaterialPhaseTimer<DIAGNOSTICS_ENABLED> slotStateTimer(stateSlotsNs);
                auto& item = output->items[i];
                component->EnsureMaterialAsset(item.materialSlot);
                auto& slot = component->SlotAt(item.materialSlot);
                item.slotVisible = slot.visible;
                item.visible = component->enabled && slot.visible;
                auto& material = item.material;
                materialCache.ApplyState(slot, material);
                slotStateTimer.Finish();
                if constexpr (DEMAND_TRACE_ENABLED) ++materialDemandTrace.Counters().consideredItems;
                if (!object.colorEligible || !item.visible) continue;
                const auto demand = demandEvaluator.Evaluate(item);
                if constexpr (DEMAND_TRACE_ENABLED) {
                    auto& row = materialDemandTrace.Counters();
                    ++row.syncEligibleItems;
                    row.eligibleDeclaredVertices += item.vertexCount;
                    row.eligibleDeclaredIndices += item.indexCount;
                    materialDemandTrace.CountPotential(initialDemandView, item, demand);
                }
                /// @note Keep geometry/authoring intact; unresolved geometry without a tested cull retains its previous synchronization contract.
                if (demand.reasons == 0 && (demand.cameraEvaluated || !demand.matchesView)) {
                    omittedBindings.push_back(i);
                    if constexpr (DEMAND_TRACE_ENABLED) ++materialDemandTrace.Counters().omittedBindingItems;
                    continue;
                }
                MaterialPhaseTimer<DIAGNOSTICS_ENABLED> projectionReuseTimer(projectionReuseNs);
                float screenPixels = 0.0f;
                const bool projectedTextureRequest = !ctx.isDeferred ||
                    renderer::ResolveGeometryRoute(material.capabilities, true) == renderer::GeometryRoute::GBuffer;
                /// @note スキンドメッシュはアニメーション姿勢を含む物体全体の境界を使い、参照姿勢の submesh 境界へ戻さない。
                if (projectedTextureRequest)
                    screenPixels = EstimateRenderTexturePixels(object, item, ctx.camera.m_position,
                        ctx.cullProjScaleY, ctx.height, ctx.cullOrthographic);
                if constexpr (DEMAND_TRACE_ENABLED) {
                    if (projectedTextureRequest) ++materialDemandTrace.Counters().projectedTextureRequestItems;
                    if (screenPixels > 0.0f) ++materialDemandTrace.Counters().positiveScreenPixelRequests;
                }
                /// @note Ray surface extraction still resolves per-object deformation and texture versions through the original slot path.
                if (!rayTracingInputs && materialCache.CopySynchronized(slot, ctx.resources, object.skinned, screenPixels, material)) {
                    item.forwardMaterial = material;
                    if constexpr (DEMAND_TRACE_ENABLED) {
                        ++materialDemandTrace.Counters().synchronizedReuseItems;
                        if (material.valid) materialDemandTrace.Counters().canonicalCopiedLogicalBytes += material.gbufferParams.size();
                    }
                    continue;
                }
                projectionReuseTimer.Finish();
                MaterialPhaseTimer<DIAGNOSTICS_ENABLED> preparationTimer(parameterPreparationNs);
                const auto* prepared = materialCache.Prepare(*component, item.materialSlot, ctx.resources, object.skinned);
                preparationTimer.Finish();
                MaterialPhaseTimer<DIAGNOSTICS_ENABLED> syncCopyTimer(syncCopyTexturesNs);
                if constexpr (DEMAND_TRACE_ENABLED) ++materialDemandTrace.Counters().syncAttempts;
                const auto* source = SyncMaterialSlotPrepared(*component, item.materialSlot, ctx.resources,
                                                       object.skinned, screenPixels, prepared);
                if constexpr (DEMAND_TRACE_ENABLED) {
                    if (source) {
                        auto& row = materialDemandTrace.Counters();
                        ++row.syncNonNullItems;
                        row.syncReturnedParameterBytes += source->paramData.size();
                        if (renderer::IsErrorShader(source->shaderPath)) ++row.errorFallbackItems;
                    }
                }
                CopyMaterial(material, source, ctx.resources, component->SlotAt(item.materialSlot), materialCache);
                material.capabilities.gbufferEquivalentShader =
                    ctx.resources.GetShaderCapabilities(material.shader).SupportsGBuffer();
                if constexpr(DIAGNOSTICS_ENABLED){if(rayTracingInputs)syncCopyTimer.Finish();}
                MaterialPhaseTimer<DIAGNOSTICS_ENABLED> rayInputsTimer(rayInputsNs,rayTracingInputs);
                const auto* deformed = rayTracingInputs && object.skinned && item.deformedContentVersion != 0
                    ? ctx.resources.Get(item.deformedVertexBuffer) : nullptr;
                const bool currentDeformationVerified = object.skinned && item.deformedContentVersion != 0
                    && deformed && deformed->GetStride() == sizeof(renderer::Vertex)
                    && deformed->GetContentVersion() == item.deformedContentVersion;
                if (rayTracingInputs)
                    ExtractRayMaterialInputs(material, source, component->SlotAt(item.materialSlot), ctx.resources, currentDeformationVerified);
                else
                    materialCache.RememberSynchronized(component->SlotAt(item.materialSlot), ctx.resources, object.skinned, screenPixels, material);
                item.forwardMaterial = material;
                if constexpr (DEMAND_TRACE_ENABLED) {
                    if (material.valid) materialDemandTrace.Counters().canonicalCopiedLogicalBytes += material.gbufferParams.size();
                }
            }
        }
    };
    if (collectMaterialPhases) {
        if (materialDemandTrace.Active()) processMaterials.template operator()<true, true>();
        else processMaterials.template operator()<true, false>();
    } else {
        if (materialDemandTrace.Active()) processMaterials.template operator()<false, true>();
        else processMaterials.template operator()<false, false>();
    }
    if(collectMaterialPhases){
        /// @note Five disjoint raw sums include clock/counter overhead; external durations leave Materials and ancestor Self unavailable.
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::StateSlots","Rendering"},static_cast<double>(stateSlotsNs)/1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::ProjectionReuse","Rendering"},static_cast<double>(projectionReuseNs)/1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::ParameterPreparation","Rendering"},static_cast<double>(parameterPreparationNs)/1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::SyncCopyTextures","Rendering"},static_cast<double>(syncCopyTexturesNs)/1000000.0);
        profiler::Profiler::PushSample({"RenderSceneExtractor::Materials::RayInputs","Rendering"},static_cast<double>(rayInputsNs)/1000000.0);
    }
    profiler::Profiler::EndScope(materialsProfile);

    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Instances");
        bool collectInstancePhases = false;
        if (profiler::Profiler::IsEnabled()) {
            const auto requestedFilter = profiler::Profiler::GetRequestedCaptureFilter();
            /// @note Only the explicit diagnostic filter reads phase clocks; PushSample still obeys the current frame's applied filter.
            collectInstancePhases = requestedFilter.scope == "RenderSceneExtractor::Instances"
                && (requestedFilter.category.empty() || requestedFilter.category == "General");
        }
        int64_t setSnapshotNs = 0, meshAndBoundsNs = 0, materialNs = 0;
        int64_t definitionNs = 0, snapshotNs = 0, ownerNs = 0;
        static uint64_t instanceOwner=0, instanceEpoch=UINT64_MAX;
        static std::unordered_map<std::string,MaterialComponent> instanceMaterials;
        if (instanceOwner!=ctx.resources.GetInstanceId() || instanceEpoch!=ctx.resources.GetResetVersion()) {
            instanceMaterials.clear(); instanceOwner=ctx.resources.GetInstanceId(); instanceEpoch=ctx.resources.GetResetVersion();
        }
        struct InstanceSourceView {
            const asset::InstanceSetAsset* definition;
            renderer::RenderInstanceInput input;
            GameObject* owner = nullptr;
        };
        std::vector<InstanceSourceView> sources;
        for (EntityID id : ctx.scene.GetEntities<InstancedMeshComponent>()) {
            InstancePhaseTimer setSnapshotTimer(collectInstancePhases, setSnapshotNs);
            InstancePhaseTimer ownerTimer(collectInstancePhases, ownerNs);
            auto* go = ctx.scene.GetGameObject(id);
            const auto* component = ctx.scene.GetComponent<InstancedMeshComponent>(id);
            if (!go || !go->activeInHierarchy() || !component || !component->m_enabled) continue;
            ownerTimer.Finish();
            InstancePhaseTimer definitionTimer(collectInstancePhases, definitionNs);
            const auto* definition = asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path);
            definitionTimer.Finish();
            if (!definition) continue;
            renderer::RenderInstanceInput input;
            InstancePhaseTimer snapshotTimer(collectInstancePhases, snapshotNs);
            input.data = component->Snapshot(*definition);
            snapshotTimer.Finish();
            InstancePhaseTimer ownerStateTimer(collectInstancePhases, ownerNs);
            input.identity = {ctx.scene.GetRenderSceneGeneration(), (static_cast<uint64_t>(id.generation) << 32) | id.index};
            input.world = go->transform.GetWorldMatrix(); input.layer = static_cast<uint32_t>(go->layer);
            input.selected = IsSelectedForOutline(*go, ctx.settings); input.castShadows = component->m_castShadows;
            input.maxDistance = component->m_maxDistance;
            const auto& budget = component->m_vegetationBudget;
            input.vegetationBudget = {budget.enabled,
                static_cast<uint32_t>(std::max(0, budget.drawDetailPerCell)),
                static_cast<uint32_t>(std::max(0, budget.nearDetailPerCell)),
                static_cast<uint32_t>(std::max(0, budget.shadowDetailPerCell))};
            sources.push_back({definition, std::move(input), go});
        }
        if (ctx.uiOptions) for (const auto& preview : ctx.uiOptions->instancePreviews) {
            const auto* definition = asset::ResolveMeshInstanceDefinition(preview.definition);
            if (!definition || !preview.data || preview.data->placements.size() > 384) continue;
            renderer::RenderInstanceInput input;
            input.data = preview.data; input.world = preview.world;
            input.identity = {ctx.scene.GetRenderSceneGeneration() ^ (1ull << 63), preview.identity};
            input.selected = preview.erase || preview.highlight; input.selectionOnly = preview.erase; input.castShadows = false;
            sources.push_back({definition, std::move(input)});
        }
        for (auto& sourceView : sources) {
            InstancePhaseTimer setSnapshotTimer(collectInstancePhases, setSnapshotNs);
            const auto& definition = *sourceView.definition;
            auto input = std::move(sourceView.input);
            if (!input.data || input.data->placements.empty()) continue;
            input.lodCount = static_cast<uint32_t>(definition.LodHeights().size());
            std::copy(definition.LodHeights().begin(), definition.LodHeights().end(), input.lodHeights.begin());
            math::Vector3 boundsCenter{}; float boundsRadius=-1, windBounds=0;
            setSnapshotTimer.Finish();
            for (const auto& sourcePart : definition.Parts()) {
                InstancePhaseTimer meshTimer(collectInstancePhases, meshAndBoundsNs);
                auto* mesh=ResolveMeshPath(sourcePart.meshPath,ctx.resources);
                if (!mesh || mesh->isSkinned || mesh->indexCount==0) { input.parts.clear(); break; }
                renderer::RenderInstancePart part;
                part.mesh = sourceView.owner ? ExtractMesh(*sourceView.owner, *mesh)
                    : ExtractMesh(input.world, ComputeWorldBoundsScale(input.world), *mesh);
                part.lod=sourcePart.lod;
                meshTimer.Finish();
                InstancePhaseTimer materialTimer(collectInstancePhases, materialNs);
                auto& componentMaterial=instanceMaterials[sourcePart.materialPath];
                componentMaterial.SetMaterialPath(sourcePart.materialPath); componentMaterial.EnsureMaterialAsset();
                auto& material=part.mesh.material;
                materialCache.ApplyState(componentMaterial,material);
                if (!materialCache.CopySynchronized(componentMaterial,ctx.resources,false,0,material,true)) {
                    const auto* prepared=materialCache.Prepare(componentMaterial,0,ctx.resources,false);
                    const auto* source=SyncMaterialSlotPrepared(componentMaterial,0,ctx.resources,false,0,prepared);
                    CopyMaterial(material,source,ctx.resources,componentMaterial,materialCache);
                    material.capabilities.gbufferEquivalentShader=ctx.resources.GetShaderCapabilities(material.shader).SupportsGBuffer();
                    materialCache.RememberSynchronized(componentMaterial,ctx.resources,false,0,material,true);
                }
                part.mesh.forwardMaterial=material;
                float windStrength = 0; std::memcpy(&windStrength, material.gbufferParams.data() + 68, sizeof(windStrength));
                windBounds = std::max(windBounds, std::abs(windStrength) * 2.0f);
                materialTimer.Finish();
                if (!material.valid || material.capabilities.blend!=renderer::BlendMode::OPAQUE_BLEND) { input.parts.clear(); break; }
                InstancePhaseTimer boundsTimer(collectInstancePhases, meshAndBoundsNs);
                const auto c=mesh->boundsCenter; const float radius=mesh->boundsRadius;
                const auto delta=c-boundsCenter; const float distance=delta.Length();
                /// @note Enclose all LOD parts without expanding their spheres into a box and back; overstated bounds delay LOD reduction.
                if (boundsRadius<0 || radius>=distance+boundsRadius) { boundsCenter=c; boundsRadius=radius; }
                else if (distance+radius>boundsRadius) {
                    const float merged=(boundsRadius+distance+radius)*0.5f;
                    if (distance>0.00001f) boundsCenter+=delta*((merged-boundsRadius)/distance);
                    boundsRadius=merged;
                }
                input.parts.push_back(std::move(part));
            }
            if (input.parts.empty()) continue;
            if (sourceView.owner) input.world = sourceView.owner->transform.GetWorldMatrix();
            input.localBoundsCenter=boundsCenter; input.localBoundsRadius=boundsRadius + windBounds;
            output->instances.push_back(std::move(input));
        }
        if (collectInstancePhases) {
            /// @note General matches Instances; Definition/Snapshot/Owner subdivide SetSnapshot, so diagnostic sums overlap and ancestor Self is unavailable.
            profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::SetSnapshot", "General"}, static_cast<double>(setSnapshotNs) / 1000000.0);
            profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::MeshAndBounds", "General"}, static_cast<double>(meshAndBoundsNs) / 1000000.0);
            profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::Material", "General"}, static_cast<double>(materialNs) / 1000000.0);
            profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::Definition", "General"}, static_cast<double>(definitionNs) / 1000000.0);
            profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::Snapshot", "General"}, static_cast<double>(snapshotNs) / 1000000.0);
            profiler::Profiler::PushSample({"RenderSceneExtractor::Instances::Owner", "General"}, static_cast<double>(ownerNs) / 1000000.0);
        }
    }

    const auto objectMasksProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::ObjectMasks", "Rendering"));
    constexpr uint32_t NO_OBJECT = UINT32_MAX;
    std::vector<uint32_t> firstObject;
    std::vector<uint32_t> nextObject;
    if (std::any_of(ctx.settings.objectMaskRequests.begin(), ctx.settings.objectMaskRequests.end(),
            [&output](const auto& request) { return renderer::IsObjectMaskRequestLive(request, output->frameStamp); })) {
        firstObject.assign(ctx.scene.EntitySlotCount(), NO_OBJECT);
        nextObject.assign(output->Objects().size(), NO_OBJECT);
        /// @note 同一 Entity に静的・スキンド両方がある場合も、元の描画物体順で全候補を保持する。
        for (size_t i = output->Objects().size(); i > 0; --i) {
            const uint32_t index = static_cast<uint32_t>(i - 1);
            const auto entityIndex = output->Objects()[index].sourceIndex;
            if (entityIndex >= firstObject.size()) continue;
            nextObject[index] = firstObject[entityIndex];
            firstObject[entityIndex] = index;
        }
    }
    for (const auto& request : ctx.settings.objectMaskRequests) {
        if (!renderer::IsObjectMaskRequestLive(request, output->frameStamp)) continue;
        ++output->liveMaskRequests;
        auto* root = ctx.scene.GetGameObject({ request.id.index, request.id.generation });
        if (!root) continue;
        renderer::RenderMaskGroup group;
        group.payload = { request.color[0], request.color[1], request.color[2], std::clamp(request.value, 0.02f, 1.0f) };
        group.visibleOnly = request.visibleOnly;
        const auto collect = [&](const auto& self, GameObject& go) -> void {
            if (!ShouldRenderGameObject(go, ctx.cullingMask)) return;
            const auto entityIndex = go.GetID().index;
            if (entityIndex >= firstObject.size()) return;
            for (uint32_t i = firstObject[entityIndex]; i != NO_OBJECT; i = nextObject[i]) {
                const auto& object = output->Objects()[i];
                if (object.sourceIndex == go.GetID().index && object.sourceGeneration == go.GetID().generation)
                    group.objectIndices.push_back(i);
            }
            if (request.includeChildren)
                for (int i = 0; i < go.GetChildCount(); ++i)
                    if (auto* child = go.GetChild(i)) self(self, *child);
        };
        collect(collect, *root);
        output->maskGroups.push_back(std::move(group));
    }
    profiler::Profiler::EndScope(objectMasksProfile);

    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::CustomPost");
        ExtractRenderCustomPost(ctx, *output);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Trails");
        ExtractRenderTrails(ctx, *output);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Decals");
        ExtractRenderDecals(ctx, *output);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Terrains");
        ExtractRenderTerrains(ctx, *output);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Water");
        ExtractRenderWater(ctx, *output);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Fibers");
        ExtractRenderFibers(ctx, *output);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Particles");
        ExtractRenderParticles(ctx, *output);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::MeshTrails");
        ExtractRenderMeshTrails(ctx, *output);
    }
    {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Environment");
        ctx.environment = ExtractRenderEnvironment(ctx);
        output->environment = ctx.environment;
        output->lighting = static_cast<const renderer::RenderLightingInput&>(ctx);
    }
    FailOpenChangedLeafWindBounds(ctx.scene, *output);
    if (!omittedBindings.empty()) {
        const auto finalView = CurrentMaterialDemandView(ctx, CurrentRayMaterialInputs(ctx));
        const bool changed = resetVersion != ctx.resources.GetResetVersion()
            || output->sceneGeneration != ctx.scene.GetRenderSceneGeneration() || resourceFrame != ctx.resources.FrameStamp();
        if (changed || NeedsMaterialBindings(finalView, *output, omittedBindings, ctx.scene)) {
            FBZZ_PROFILE_SCOPE("RenderSceneExtractor::RestoreMaterialBindings");
            RestoreMaterialBindings(ctx, *output, omittedBindings, materialDemandTrace.Active() ? &materialDemandTrace : nullptr);
            omittedBindings.clear();
            FailOpenChangedLeafWindBounds(ctx.scene, *output);
        }
    }
    /// @note 材質/環境の callback が ticket を完成しても、未公開 geometry 自体は再抽出しない。
    for (uint32_t index : preparedItems) {
        auto& item = output->items[index];
        const auto writes = item.preparedSkinning->Writes();
        if (item.preparedSkinningWrite >= writes.size()) continue;
        const auto& write = writes[item.preparedSkinningWrite];
        const bool current = write.recorded && item.preparedSkinning->IsWriteCurrent(item.preparedSkinningWrite);
        item.deformedVertexBuffer = current ? write.output : renderer::ResourceHandle<renderer::BufferTag>{};
        item.deformedContentVersion = current ? write.resultingVersion : 0;
    }
    RememberPreparedSnapshotItems(output, OwnPreparedSnapshotItems(std::move(preparedItems)));
    ctx.renderScene = std::move(output);
    material_demand_detail::RetainPendingBindings(ctx, ctx.renderScene, std::move(omittedBindings));
    materialDemandTrace.Complete(ctx);
}

} /// @note namespace fbzz::scene
