/// @file    RenderSceneGeometry.cpp
/// @brief   Scene 順にメッシュと境界を抽出し、空間更新履歴とともに公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneExtraction.hpp"
#include "RenderSceneGeometryCache.hpp"
#include "RenderSceneSkinningGeometry.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"

#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Graphics/Renderer/MeshletGeometry.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>


namespace fbzz::scene {

namespace render_extraction_detail {

namespace {

thread_local std::vector<EntityID> s_renderGeometryCandidates;

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

/// @note 静的キャッシュの再構築時に境界変更を記録し、スキン付きとキャッシュ外の境界は常に変更扱いとする。
/// @note 個体の追加順で寿命を照合するため、公開後に境界を再走査する必要がない。
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
        m_update.emplace();
        m_update->version = version;
        m_update->baseVersion = m_cache.spatialVersion;
        m_update->fullReplacement = m_cache.spatialVersion == 0;
    }

    bool Enabled() const { return m_update.has_value(); }

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

    std::optional<renderer::RenderSpatialUpdate> Finish()
    {
        if (!m_update) return std::nullopt;
        if (m_update->objectCount != m_cache.spatialObjects.size()) {
            m_cache.spatialObjects.resize(m_update->objectCount);
            m_update->fullReplacement = true;
        }
        if (m_update->objectCount > UINT32_MAX) {
            m_cache.spatialVersion = 0;
            return std::nullopt;
        }
        if (m_update->fullReplacement) m_update->baseVersion = 0;
        m_cache.spatialVersion = m_update->version;
        return std::move(m_update);
    }
private:
    StaticSceneGeometry& m_cache;
    std::optional<renderer::RenderSpatialUpdate> m_update;
};

std::array<uint32_t, 10> StaticPoseInputs(const Transform& transform)
{
    const auto& worldPosition = transform.worldPosition;
    const auto& worldRotation = transform.worldRotation;
    const auto& worldScale = transform.worldScale;
    const std::array<float, 10> pose{ worldPosition.x, worldPosition.y, worldPosition.z,
        worldRotation.x, worldRotation.y, worldRotation.z, worldRotation.w, worldScale.x, worldScale.y, worldScale.z };
    return std::bit_cast<std::array<uint32_t, 10>>(pose);
}

std::array<uint32_t, 14> StaticMeshInputs(const renderer::Mesh& mesh)
{
    const auto& boundsCenter = mesh.boundsCenter;
    const auto& boundsExtents = mesh.boundsExtents;
    return {mesh.vertexBuffer.id, mesh.vertexBuffer.gen, mesh.indexBuffer.id, mesh.indexBuffer.gen,
        mesh.vertexCount, mesh.indexCount, std::bit_cast<uint32_t>(boundsCenter.x), std::bit_cast<uint32_t>(boundsCenter.y),
        std::bit_cast<uint32_t>(boundsCenter.z), std::bit_cast<uint32_t>(mesh.boundsRadius),
        std::bit_cast<uint32_t>(boundsExtents.x), std::bit_cast<uint32_t>(boundsExtents.y), std::bit_cast<uint32_t>(boundsExtents.z),
        std::bit_cast<uint32_t>(mesh.occlusionRadius)};
}

uint64_t EntityKey(EntityID id)
{
    return (static_cast<uint64_t>(id.index) << 32) | id.generation;
}

struct RayLodSelection {
    bool visible = false;
    bool ambiguous = false;
    uint64_t group = 0;
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
math::Matrix4 AdvanceWorld(T& component, const math::Matrix4& world, uint64_t frameStamp)
{
    if (component.prevWorldFrame != frameStamp) {
        component.prevWorldMatrix = frameStamp > 0 && component.prevWorldFrame == frameStamp - 1
            ? component.worldMatrixSnapshot : world;
        component.worldMatrixSnapshot = world;
        component.prevWorldFrame = frameStamp;
    }
    return component.prevWorldMatrix;
}

template<typename T>
renderer::RenderObject ExtractObject(GameObject& owner, T& component, uint64_t frameStamp,
    const math::Matrix4& world, const math::Matrix4& inverseTranspose)
{
    renderer::RenderObject object;
    object.sourceIndex = owner.GetID().index;
    object.sourceGeneration = owner.GetID().generation;
    object.layer = static_cast<uint32_t>(owner.layer);
    object.world = world;
    object.worldInvTranspose = inverseTranspose;
    object.previousWorld = AdvanceWorld(component, object.world, frameStamp);
    object.sortPosition = owner.transform.position;
    object.castShadows = component.castShadows;
    object.lodVisible = component.lodVisible;
    object.lodDither = component.lodDither;
    if constexpr (std::is_same_v<T, MeshRenderer>) {
        if (const auto* part = owner.GetComponent<WaterfallPart>()) {
            object.waterfallFarIndexStart = part->farIndexStart;
            object.sortPosition = owner.transform.worldPosition;
        }
    }
    return object;
}

template<typename T>
renderer::RenderObject ExtractObject(GameObject& owner, T& component, uint64_t frameStamp)
{
    const auto world = owner.transform.GetWorldMatrix();
    return ExtractObject(owner, component, frameStamp, world, math::Matrix4::InverseTransposeAffine(world));
}

float WorldOccluderRadius(const math::Matrix4& world, float localRadius)
{
    /// @note TRS の最小軸長を掛けると、負や不均一なスケールでも内接球を形状内に保てる。
    const float sx = math::Vector3{world.m[0][0], world.m[1][0], world.m[2][0]}.Length();
    const float sy = math::Vector3{world.m[0][1], world.m[1][1], world.m[2][1]}.Length();
    const float sz = math::Vector3{world.m[0][2], world.m[1][2], world.m[2][2]}.Length();
    return localRadius > 0.0f ? localRadius * (std::min)({sx, sy, sz}) : 0.0f;
}


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

/// @note メッシュ解決のコールバックが姿勢を書き換え得るため、解決後に現在の姿勢を読む。
renderer::RenderMeshItem ExtractMesh(GameObject& owner, const renderer::Mesh& mesh)
{
    const math::Matrix4 world = owner.transform.GetWorldMatrix();
    return ExtractMesh(world, ComputeWorldBoundsScale(world), mesh);
}

namespace {

/// @brief 共有メッシュ入力から中間の個体配列を経由せず描画項目を追加する。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine Shared Fragments
class DenseGeometryBatch {
public:
    DenseGeometryBatch(renderer::RenderScene& output, size_t hint) : m_output(output)
    { m_output.geometry.EditObjects().reserve(hint); m_output.items.reserve(hint); m_meshes.reserve(32); }
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

/// @note 初回や連続移動時は、行列を永続キャッシュへ書き戻さない。
/// @see https://learn.microsoft.com/en-us/cpp/cpp/inline-functions-cpp 構造体を値で返す補助処理のインライン化に使う __forceinline。
__forceinline renderer::RenderObject ExtractUncachedStaticObject(GameObject& owner, MeshRenderer& component, uint64_t frameStamp,
    renderer::RenderScene& output, DenseGeometryBatch& denseGeometry)
{
    auto object = ExtractObject(owner, component, frameStamp);
    object.firstItem = static_cast<uint32_t>(output.items.size());
    auto& item = denseGeometry.Append(*component.mesh, object.world, ComputeWorldBoundsScale(object.world));
    const float windPadding = ComputeLeafWindBoundsPadding(owner, 1);
    object.leafWindBoundsPadding = windPadding;
    if (windPadding < 0.0f) item.boundsRadius = 0.0f;
    else if (item.boundsRadius > 0.0f) item.boundsRadius += windPadding;
    if (windPadding != 0.0f) item.occluderRadius = 0.0f;
    object.boundsCenter = item.boundsCenter; object.boundsRadius = item.boundsRadius;
    object.itemCount = 1;
    item.objectIndex = static_cast<uint32_t>(output.Objects().size());
    return object;
}

/// @note 通知は同フレームの共有を失効させ、行列と境界の再計算は現在の入力値で決める。
/// @see https://github.com/godotengine/godot/blob/master/servers/rendering/renderer_scene_cull.h インスタンス依存の変更通知。
renderer::RenderObject ExtractStaticObject(const ComponentArray<MeshRenderer>& staticMeshes, StaticSceneGeometry& cache,
    GameObject& owner, MeshRenderer& component, uint64_t frameStamp, renderer::RenderScene& output, DenseGeometryBatch& denseGeometry,
    bool& boundsChanged)
{
    boundsChanged = false;
    const auto entity = owner.GetID();
    /// @note Component の格納位置は有効状態や Scene の表示順で変わらない。
    const size_t denseComponentIndex = staticMeshes.GetDenseIndex(entity);
    if (denseComponentIndex >= cache.entries.size()) cache.entries.resize(denseComponentIndex + 1);
    auto& cachedGeometry = cache.entries[denseComponentIndex];
    const auto componentLifetime = staticMeshes.GetLifetime(entity);
    const auto poseInputs = StaticPoseInputs(owner.transform);
    const bool sameMeshComponent = cachedGeometry.entity == entity && cachedGeometry.lifetime == componentLifetime && cachedGeometry.mesh == component.mesh;
    const bool poseChanged = cachedGeometry.pose != poseInputs;
    const bool firstExtractionThisFrame = cachedGeometry.lastExtractFrame != frameStamp;
    cachedGeometry.lastExtractFrame = frameStamp;
    if (sameMeshComponent && poseChanged && firstExtractionThisFrame && frameStamp > 0 && cachedGeometry.lastPoseChangeFrame == frameStamp - 1) {
        boundsChanged = true;
        cachedGeometry.pose = poseInputs; cachedGeometry.lastPoseChangeFrame = frameStamp; cachedGeometry.valid = false;
        denseGeometry.MarkExistingChange();
        denseGeometry.MarkMovingGeometry();
        return ExtractUncachedStaticObject(owner, component, frameStamp, output, denseGeometry);
    }
    if (!sameMeshComponent) cachedGeometry.lastPoseChangeFrame = UINT64_MAX;
    else if (poseChanged) cachedGeometry.lastPoseChangeFrame = frameStamp;
    const auto meshInputs = StaticMeshInputs(*component.mesh);
    const float windPadding = ComputeLeafWindBoundsPadding(owner, 1);
    if (!cachedGeometry.valid || cachedGeometry.entity != entity || cachedGeometry.lifetime != componentLifetime || cachedGeometry.mesh != component.mesh
        || cachedGeometry.pose != poseInputs || cachedGeometry.meshInputs != meshInputs || cachedGeometry.windPadding != windPadding) {
        boundsChanged = true;
        if (cachedGeometry.entity.IsValid()) denseGeometry.MarkExistingChange();
        std::optional<profiler::ProfileScope> rebuildProfile;
        if (profiler::Profiler::IsEnabled()) rebuildProfile.emplace(
            profiler::ProfilerMarker("RenderSceneExtractor::StaticGeometryRebuild"));
        cachedGeometry.entity = entity; cachedGeometry.lifetime = componentLifetime; cachedGeometry.mesh = component.mesh;
        cachedGeometry.pose = poseInputs; cachedGeometry.meshInputs = meshInputs; cachedGeometry.windPadding = windPadding;
        cachedGeometry.world = owner.transform.GetWorldMatrix();
        cachedGeometry.inverseTranspose = math::Matrix4::InverseTransposeAffine(cachedGeometry.world);
        const auto bounds = ComputeWorldBounds(cachedGeometry.world, ComputeWorldBoundsScale(cachedGeometry.world), *component.mesh);
        cachedGeometry.center = bounds.center;
        cachedGeometry.radius = component.mesh->boundsRadius > 0.0f && windPadding >= 0.0f ? bounds.radius + windPadding : 0.0f;
        cachedGeometry.occluderRadius = windPadding == 0.0f ? WorldOccluderRadius(cachedGeometry.world, component.mesh->occlusionRadius) : 0.0f;
        cachedGeometry.reliable = IsReliableOccluder(*component.mesh);
        cachedGeometry.valid = true;
    }
    auto object = ExtractObject(owner, component, frameStamp, cachedGeometry.world, cachedGeometry.inverseTranspose);
    object.firstItem = static_cast<uint32_t>(output.items.size()); object.itemCount = 1;
    object.boundsCenter = cachedGeometry.center; object.boundsRadius = cachedGeometry.radius;
    object.leafWindBoundsPadding = cachedGeometry.windPadding;
    auto& item = output.items.emplace_back();
    item.objectIndex = static_cast<uint32_t>(output.Objects().size());
    item.vertexBuffer = item.skinningVertexBuffer = component.mesh->vertexBuffer;
    item.indexBuffer = component.mesh->indexBuffer;
    item.vertexCount = component.mesh->vertexCount; item.indexCount = component.mesh->indexCount;
    item.clusterLod = component.mesh->ClusterLod();
    item.vertexStride = sizeof(renderer::Vertex);
    item.boundsCenter = cachedGeometry.center; item.boundsRadius = cachedGeometry.radius; item.reliableOccluder = cachedGeometry.reliable;
    item.occluderRadius = cachedGeometry.occluderRadius;
    return object;
}

}

renderer::RenderScene BuildRenderSceneGeometry(Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette, const GeometryBuildOptions& options,
    uint64_t* staticGeometryRevision, renderer::RenderScene* reusableStorage, SkinnedObjectRange* skinnedObjects)
{
    FBZZ_PROFILE_SCOPE("RenderSceneExtractor::Geometry");
    if (skinnedObjects) *skinnedObjects = {};
    const bool fuseColorEligibility = options.fuseColorEligibility && !options.extractItemMaterialState;
    const bool rayTracingInputs = options.rayTracingInputs;
    const auto* resources = options.resources;
    if (fuseColorEligibility && profiler::Profiler::IsEnabled())
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::FusedColorEligibility");
    renderer::RenderScene output;
    if (reusableStorage) {
        output.items.swap(reusableStorage->items);
        output.geometry.EditObjects().swap(reusableStorage->geometry.EditObjects());
    }
    output.snapshotSerial = NextRenderSnapshotSerial();
    output.sceneGeneration = scene.GetRenderSceneGeneration();
    output.frameStamp = frameStamp;
    const auto rayLodRenderers = rayTracingInputs ? CollectRayLodRenderers(scene, output.rayLodDiagnostics)
        : std::unordered_map<uint64_t, RayLodSelection>{};
    std::optional<std::unordered_map<uint64_t, RayLodSelection>> rasterShadowBakeLodRenderers;
    const auto objectsProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::GeometryObjects", "Rendering"));
    auto& candidates = s_renderGeometryCandidates;
    scene.CopyRendererEntities(candidates);
    DenseGeometryBatch denseGeometry(output, candidates.size());
    auto& objects = output.geometry.EditObjects();
    auto& staticGeometry = *FindStaticGeometry(scene, true);
    SpatialUpdateBuilder spatialUpdates(staticGeometry);
    const bool useDenseFallback = staticGeometry.primed && staticGeometry.denseBuildsRemaining > 0;
    if (useDenseFallback) {
        --staticGeometry.denseBuildsRemaining;
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::DenseGeometryFallback");
    } else if (!staticGeometry.primed) {
        FBZZ_PROFILE_MARKER("RenderSceneExtractor::ColdGeometry");
    }
    const bool reuseStaticGeometry = staticGeometry.primed && !useDenseFallback;
    const auto& staticMeshes = detail::ComponentStorageAccess<MeshRenderer>::Get(scene);
    if (reuseStaticGeometry) {
        const size_t meshCount = staticMeshes.Count();
        if (staticGeometry.entries.size() > meshCount) staticGeometry.entries.resize(meshCount);
        if (staticGeometry.entries.capacity() < meshCount) staticGeometry.entries.reserve(meshCount);
    }
    size_t staticObjectCount = 0;
    for (EntityID id : candidates) {
        auto* source = scene.GetGameObject(id);
        if (!source) continue;
        auto& owner = *source;
        if (!owner.activeInHierarchy()) continue;
        std::optional<bool> colorEligibility;
        const auto applyColorEligibility = [&](renderer::RenderObject& object) {
            if (!fuseColorEligibility) return;
            if (!colorEligibility.has_value()) {
                const auto* material = owner.GetComponent<MaterialComponent>();
                colorEligibility = !material || material->enabled;
            }
            object.colorEligible = *colorEligibility;
        };
        if (auto* meshRenderer = owner.GetComponent<MeshRenderer>(); meshRenderer && meshRenderer->enabled && meshRenderer->mesh && !meshRenderer->mesh->isSkinned) {
            bool boundsChanged = true;
            auto object = reuseStaticGeometry
                ? ExtractStaticObject(staticMeshes, staticGeometry, owner, *meshRenderer, frameStamp, output, denseGeometry, boundsChanged)
                : ExtractUncachedStaticObject(owner, *meshRenderer, frameStamp, output, denseGeometry);
            ++staticObjectCount;
            if (const auto lod = rayLodRenderers.find(EntityKey(owner.GetID())); lod != rayLodRenderers.end()) {
                object.rayVisible = lod->second.visible;
                object.rayLodSelectionRequired = lod->second.ambiguous;
            }
            if (meshRenderer->bakeStaticShadow) {
                if (!rayTracingInputs && !rasterShadowBakeLodRenderers) {
                    std::vector<renderer::RenderRayLodDiagnostic> unusedDiagnostics;
                    rasterShadowBakeLodRenderers = CollectRayLodRenderers(scene, unusedDiagnostics);
                }
                const auto& shadowBakeLodRenderers = rayTracingInputs ? rayLodRenderers : *rasterShadowBakeLodRenderers;
                object.shadowBakeStatic = !shadowBakeLodRenderers.contains(EntityKey(owner.GetID()));
                if (object.shadowBakeStatic)
                    output.shadowBakeMeshObjects.push_back(static_cast<uint32_t>(objects.size()));
            }
            if (spatialUpdates.Enabled())
                spatialUpdates.Append(id, staticMeshes.GetLifetime(id), false, boundsChanged);
            applyColorEligibility(object);
            objects.push_back(object);
        } else if (reuseStaticGeometry && meshRenderer && meshRenderer->enabled) {
            /// @note メッシュの解除やスキン付きへの切り替えを観測したら、静的形状の再利用を打ち切る。
            const size_t slot = staticMeshes.GetDenseIndex(id);
            if (slot < staticGeometry.entries.size()) staticGeometry.entries[slot].entity = EntityID::INVALID;
        }
        auto* skinnedRenderer = owner.GetComponent<SkinnedMeshRenderer>();
        if (!skinnedRenderer || !skinnedRenderer->enabled || !skinnedRenderer->model) continue;
        auto object = ExtractObject(owner, *skinnedRenderer, frameStamp);
        if (const auto lod = rayLodRenderers.find(EntityKey(owner.GetID())); lod != rayLodRenderers.end()) {
            object.rayVisible = lod->second.visible;
            object.rayLodSelectionRequired = lod->second.ambiguous;
        }
        object.skinned = true;
        object.leafWindBoundsPadding = ComputeLeafWindBoundsPadding(owner, skinnedRenderer->SubmeshCount());
        object.firstItem = static_cast<uint32_t>(output.items.size());
        /// @note この走査ではコールバックを呼ばないため、全サブメッシュが同じ姿勢を使える。
        const float maximumScale = ComputeWorldBoundsScale(object.world);
        ExtractSkinnedPose(owner, *skinnedRenderer, identityPalette, resources, object);
        for (size_t slot = 0; slot < skinnedRenderer->SubmeshCount(); ++slot) {
            const auto* mesh = skinnedRenderer->SubmeshMesh(slot);
            if (!mesh) continue;
            auto& item = denseGeometry.Append(*mesh, object.world, maximumScale);
            item.objectIndex = static_cast<uint32_t>(objects.size());
            item.materialSlot = static_cast<uint32_t>(slot);
            item.sourceSubmesh = skinnedRenderer->SubmeshAt(slot);
            item.skinningVertexBuffer = skinnedRenderer->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer);
            /// @note 古いフレームやモーフ対象に残った GPU 出力を再利用しない。
            if (skinnedRenderer->gpuSkinningFrame == frameStamp && skinnedRenderer->skinnedBufferModel == skinnedRenderer->model)
                item.deformedVertexBuffer = skinnedRenderer->ResolveSlotSkinnedVertexBuffer(slot);
        }
        object.itemCount = static_cast<uint32_t>(output.items.size()) - object.firstItem;
        if (spatialUpdates.Enabled())
            spatialUpdates.Append(id, scene.GetComponentLifetime<SkinnedMeshRenderer>(id), true, true);
        applyColorEligibility(object);
        if (skinnedObjects) {
            if (skinnedObjects->begin == skinnedObjects->end) skinnedObjects->begin = objects.size();
            skinnedObjects->end = objects.size() + 1;
        }
        objects.push_back(object);
    }
    if (reuseStaticGeometry) {
        /// @note 単発のテレポートを除外するため、変更が多い抽出が連続したときだけ一時的に追跡を省く。
        const bool highChange = staticObjectCount >= 64 && denseGeometry.ExistingChanges() >= staticObjectCount - staticObjectCount / 4;
        if (highChange && staticGeometry.highChangeLastBuild) {
            staticGeometry.denseBuildsRemaining = 16;
            staticGeometry.highChangeLastBuild = false;
        } else staticGeometry.highChangeLastBuild = highChange;
    }
    staticGeometry.primed = true;
    auto update = spatialUpdates.Finish();
    if (staticGeometryRevision) *staticGeometryRevision = staticGeometry.revision;
    candidates.clear();
    profiler::Profiler::EndScope(objectsProfile);
    const auto materialCapabilitiesProfile = profiler::Profiler::BeginScope(
        profiler::ProfilerMarker("RenderSceneExtractor::GeometryMaterialCapabilities", "Rendering"));
    if (!fuseColorEligibility) ExtractGeometryMaterialState(scene, output, options.extractItemMaterialState);
    output.geometry.SetSpatialUpdate(std::move(update));
    profiler::Profiler::EndScope(materialCapabilitiesProfile);
    return output;
}

}

renderer::RenderScene ExtractRenderSceneGeometry(Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette, bool rayTracingInputs)
{
    return render_extraction_detail::BuildRenderSceneGeometry(scene, frameStamp, identityPalette,
        { .rayTracingInputs = rayTracingInputs });
}

}
