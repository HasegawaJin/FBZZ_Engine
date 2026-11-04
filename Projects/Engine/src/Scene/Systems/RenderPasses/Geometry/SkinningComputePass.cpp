/// @file    SkinningComputePass.cpp
/// @brief   コンピュートスキニング — ボーン変形を 1 フレームに 1 回だけ計算して共有する。
/// @author  Hasegawa Jin
/// @date    2026-08-14

/// @note スキニングは各マテリアルの VS 内で毎回計算されていた。ここで 1 回だけ変形し、静的メッシュと
/// @note 同じ頂点レイアウト (`renderer::Vertex`) へ書き出すことで後続パスは「ただの静的メッシュ」として
/// @note 扱える。次の場合は従来の VS スキニングへフォールバックする: ポーズまたは入力未解決、
/// @note バックエンドが GPU 書き込み頂点バッファ非対応。
#include "GeometryPasses.hpp"
#include "BoneBakeSkinning.hpp"
#include "SkinningSignatureScratch.hpp"
#include "SkinningOutputDemand.hpp"
#include <Graphics/Renderer/SkinningPolicy.hpp>
#include <Graphics/Renderer/BufferReadScope.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/BoneBakeRuntime.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Graphics/Renderer/RenderVisibility.hpp>
#include <Graphics/Effects/PreparedSkinning.hpp>
#include <Graphics/Passes/Geometry/ComputedSkinningDemand.hpp>
#include <Core/ExternalCallbackBoundary.hpp>
#include <cmath>
#include "../../AnimationWorkTrace.hpp"
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/IBuffer.hpp"
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include "Engine/Renderer/DynamicBufferPool.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Core/Profiler/Profiler.hpp>
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace skinning_demand_detail {
namespace {
thread_local const RenderPassContext* s_demandContext=nullptr;
thread_local uint32_t s_demandDepth=0;
bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool Finite(const math::Matrix4& value)
{
    for(const auto& row:value.m)for(float element:row)if(!std::isfinite(element))return false;
    return true;
}
bool SafeWorldTransform(const Transform& transform)
{
    if(!Finite(transform.worldPosition) || !Finite(transform.worldScale))return false;
    const auto& rotation=transform.worldRotation;
    if(!std::isfinite(rotation.x) || !std::isfinite(rotation.y)
        || !std::isfinite(rotation.z) || !std::isfinite(rotation.w))return false;
    const float squared=rotation.x*rotation.x+rotation.y*rotation.y+rotation.z*rotation.z+rotation.w*rotation.w;
    /// @note Matrix4::TRS normalizes this quaternion. Reject before that call can report its zero-length contract.
    return std::isfinite(squared) && squared>math::EPSILON*math::EPSILON;
}
bool StoredPoseBounds(GameObject& object,const SkinnedMeshRenderer& skin,WorldBounds& bounds)
{
    const AnimatorComponent* animator=nullptr;
    GameObject* owner=nullptr;
    for(GameObject* current=&object;current;current=current->GetParent()) {
        if(const auto* candidate=current->GetComponent<AnimatorComponent>()) { animator=candidate;owner=current;break; }
    }
    /// @note Bind-pose fallback bounds are not evidence of a current CPU pose, even in this provisional diagnostic.
    if(!animator || !owner || !Finite(animator->skinnedBoundsCenter)
        || !std::isfinite(animator->skinnedBoundsRadius) || animator->skinnedBoundsRadius<=0
        || !SafeWorldTransform(owner->transform))return false;
    for(size_t slot=0;slot<skin.SubmeshCount();++slot) {
        const auto* mesh=skin.SubmeshMesh(slot);
        if(!mesh || !std::isfinite(mesh->boundsRadius))return false;
    }
    if(!Finite(owner->transform.GetWorldMatrix()))return false;
    return ComputeSkinnedWorldBounds(object,skin,bounds) && Finite(bounds.center)
        && std::isfinite(bounds.radius) && bounds.radius>0;
}
uint32_t ObserveMorphContainersAndSelection(const SkinnedMeshRenderer& skin)
{
    uint32_t flags=0;
    if(!skin.morphWeights.empty())flags|=InputBit(InputFlag::WEIGHT_MAP_PRESENT);
    if(!skin.appliedMorphWeights.empty())flags|=InputBit(InputFlag::APPLIED_MAP_PRESENT);
    if(!skin.morphVertexBuffers.empty())flags|=InputBit(InputFlag::BUFFER_VECTOR_PRESENT);
    const auto weights=[&](const auto& map,InputFlag nonzero,InputFlag nonfinite) {
        for(const auto& entry:map) {
            if(!std::isfinite(entry.second))flags|=InputBit(nonfinite);
            else if(entry.second!=0.0f)flags|=InputBit(nonzero);
        }
    };
    weights(skin.morphWeights,InputFlag::WEIGHT_FINITE_NONZERO,InputFlag::WEIGHT_NONFINITE);
    weights(skin.appliedMorphWeights,InputFlag::APPLIED_WEIGHT_FINITE_NONZERO,InputFlag::APPLIED_WEIGHT_NONFINITE);
    if(skin.morphWeights!=skin.appliedMorphWeights)flags|=InputBit(InputFlag::REQUESTED_APPLIED_DIFFER);
    bool bitwiseDiffer=skin.morphWeights.size()!=skin.appliedMorphWeights.size();
    if(!bitwiseDiffer)for(const auto& entry:skin.morphWeights) {
        const auto found=skin.appliedMorphWeights.find(entry.first);
        if(found==skin.appliedMorphWeights.end() || std::bit_cast<uint32_t>(entry.second)!=std::bit_cast<uint32_t>(found->second)) {
            bitwiseDiffer=true;break;
        }
    }
    if(bitwiseDiffer)flags|=InputBit(InputFlag::REQUESTED_APPLIED_BITWISE_DIFFER);
    for(const auto& handle:skin.morphVertexBuffers)
        if(handle.IsValid()) {flags|=InputBit(InputFlag::LOGICAL_BUFFER_HANDLE_PRESENT);break;}
    for(size_t slot=0;slot<skin.SubmeshCount();++slot) {
        const auto* mesh=skin.SubmeshMesh(slot);
        if(!mesh)continue;
        const uint32_t index=skin.SubmeshAt(slot);
        if(index<skin.morphVertexBuffers.size() && skin.morphVertexBuffers[index].IsValid()) {
            flags|=InputBit(InputFlag::SELECTED_MORPH_PATH);
            if(skin.morphVertexBuffers[index]!=mesh->vertexBuffer)flags|=InputBit(InputFlag::SELECTED_SOURCE_DIFFERS_FROM_BASE);
        }
        if(!mesh->morphTargets.empty())flags|=InputBit(InputFlag::SELECTED_TARGET_PRESENT);
        for(const auto& target:mesh->morphTargets) {
            const auto classify=[&](const auto& map,InputFlag nonzero,InputFlag nonfinite) {
                const auto found=map.find(target.name);
                if(found==map.end())return;
                if(!std::isfinite(found->second))flags|=InputBit(nonfinite);
                else if(found->second!=0.0f)flags|=InputBit(nonzero);
            };
            classify(skin.morphWeights,InputFlag::SELECTED_TARGET_REQUESTED_NONZERO,InputFlag::SELECTED_TARGET_REQUESTED_NONFINITE);
            classify(skin.appliedMorphWeights,InputFlag::SELECTED_TARGET_APPLIED_NONZERO,InputFlag::SELECTED_TARGET_APPLIED_NONFINITE);
        }
    }
    constexpr uint32_t maps=InputBit(InputFlag::WEIGHT_MAP_PRESENT)|InputBit(InputFlag::APPLIED_MAP_PRESENT);
    constexpr uint32_t nonzeroOrNonfinite=InputBit(InputFlag::WEIGHT_FINITE_NONZERO)|InputBit(InputFlag::WEIGHT_NONFINITE)
        |InputBit(InputFlag::APPLIED_WEIGHT_FINITE_NONZERO)|InputBit(InputFlag::APPLIED_WEIGHT_NONFINITE);
    if((flags&maps)!=0 && (flags&nonzeroOrNonfinite)==0)flags|=InputBit(InputFlag::MAPS_ALL_FINITE_ZERO);
    if((flags&maps)==0 && (flags&InputBit(InputFlag::BUFFER_VECTOR_PRESENT))!=0
        && (flags&InputBit(InputFlag::LOGICAL_BUFFER_HANDLE_PRESENT))==0)flags|=InputBit(InputFlag::VECTOR_PRESENCE_ONLY);
    return flags;
}

bool FiniteCullingView(const RenderPassContext& context)
{
    if(!Finite(context.camera.m_position) || !Finite(context.cullCameraForward)
        || !std::isfinite(context.cullingBoundsPadding) || !std::isfinite(context.cullMaxDistance)
        || !std::isfinite(context.cullProjScaleY) || !std::isfinite(context.smallObjectScreenHeight))return false;
    if(context.hasLayerCullDistances)
        for(float distance:context.cullLayerDistances)if(!std::isfinite(distance))return false;
    if(context.frustumCullingEnabled && context.cameraFrustum)
        for(int index=0;index<math::Frustum::PLANE_COUNT;++index) {
            const auto plane=context.cameraFrustum->GetPlane(index);
            if(!Finite(plane.normal) || !std::isfinite(plane.distance))return false;
        }
    return true;
}
}
OutputDemandScope::OutputDemandScope(const RenderPassContext& context):m_previous(s_demandContext)
{
    s_demandContext=&context;++s_demandDepth;
}
OutputDemandScope::~OutputDemandScope()
{
    s_demandContext=m_previous;--s_demandDepth;
}
bool IsOutputDemandScoped(const RenderPassContext& context)
{
    return s_demandContext==&context && s_demandDepth==1;
}
SceneDemandSnapshot ObserveSceneDemand(const RenderPassContext& context)
{
    SceneDemandSnapshot result;
    auto& scene=context.scene;
    result.sceneObjects=scene.GameObjectCount();
    result.animatorComponents=scene.GetEntities<AnimatorComponent>().size();
    result.skinnedRendererComponents=scene.GetEntities<SkinnedMeshRenderer>().size();
    result.scopeActive=IsOutputDemandScoped(context);
    if(!result.scopeActive)result.globalReasons|=Bit(Reason::UNSCOPED_OR_REENTRY);
    result.shadowRequested=context.settings.shadowEnabled
        && (context.shadowStrength>0 || context.settings.volumetricLight.enabled || context.punctualShadowViewCount>0);
    result.rayRequested=context.experimentalRayTracingEnabled
        && (context.settings.modeRequest.mode!=renderer::RenderMode::RASTER || renderer::IsRayDebugView(context.settings.viewMode));
    if(result.rayRequested)result.globalReasons|=Bit(Reason::RAY);
    if(context.objectMaskEnabled || !context.settings.objectMaskRequests.empty())result.globalReasons|=Bit(Reason::OBJECT_MASK);
    for(EntityID id:scene.GetEntities<ReflectionProbeComponent>()) {
        const auto* object=scene.GetGameObject(id);const auto* probe=scene.GetComponent<ReflectionProbeComponent>(id);
        if(object && object->activeInHierarchy() && probe && probe->enabled) {
            ++result.activeReflectionProbes;
            if(probe->captureMode==ReflectionProbeCaptureMode::DynamicScene)++result.activeReflectionSceneProbes;
        }
    }
    const bool canBake=context.handles.lightProbeProjectCS.IsValid() && context.handles.lightProbeProjectCB.IsValid()
        && context.handles.lightProbeCaptureFrameCB.IsValid() && context.handles.lightProbeCaptureAdvancedCB.IsValid();
    for(EntityID id:scene.GetEntities<LightProbeVolumeComponent>()) {
        const auto* object=scene.GetGameObject(id);const auto* volume=scene.GetComponent<LightProbeVolumeComponent>(id);
        if(object && object->activeInHierarchy() && volume && volume->enabled) {
            ++result.activeLightProbeVolumes;
            if(canBake)++result.activeLightProbeBakeConsumers;
        }
    }
    if(result.activeReflectionSceneProbes || result.activeLightProbeBakeConsumers)result.globalReasons|=Bit(Reason::PROBE_GI);
    for(EntityID id:scene.GetEntities<InstancedMeshComponent>()) {
        const auto* object=scene.GetGameObject(id);const auto* instances=scene.GetComponent<InstancedMeshComponent>(id);
        if(object && object->activeInHierarchy() && instances && instances->m_enabled)++result.activeInstancedComponents;
    }
    /// @note Scene presence is separate from demand: static instance extraction does not resolve SMR-owned deformed buffers.
    if(!scene.GetUserRenderPasses().empty())result.globalReasons|=Bit(Reason::NATIVE_SCRIPT_OR_PASS);
    for(EntityID id:scene.GetEntities<ScriptComponent>()) {
        const auto* object=scene.GetGameObject(id);const auto* scripts=scene.GetComponent<ScriptComponent>(id);
        if(!object || !object->activeInHierarchy() || !scripts)continue;
        for(const auto& entry:scripts->scripts) {
            if(!entry.script || !entry.script->enabled)continue;
            if(ManagedScriptRuntime::Instance().FindAdapter(entry.script.get()))++result.activeManagedScriptEntries;
            else ++result.activeNativeScriptEntries;
        }
    }
    if(result.activeNativeScriptEntries)result.globalReasons|=Bit(Reason::NATIVE_SCRIPT_OR_PASS);
    return result;
}
Observation ObserveRendererDemand(const RenderPassContext& context,GameObject& object,
    const SkinnedMeshRenderer& skin,const SceneDemandSnapshot& scene)
{
    Observation result;result.reasons=scene.globalReasons;
    if(scene.shadowRequested && skin.castShadows)result.reasons|=Bit(Reason::SHADOW);
    if(const auto* fiber=object.GetComponent<FiberComponent>();fiber && fiber->m_enabled)
        result.reasons|=Bit(Reason::FIBER_OR_PROCEDURAL);
    result.inputFlags=ObserveMorphContainersAndSelection(skin);
    if(result.HasInput(InputFlag::SELECTED_MORPH_PATH))result.reasons|=Bit(Reason::MORPH_OR_BAKE);
    for(GameObject* current=&object;current;current=current->GetParent()) {
        if(const auto* animator=current->GetComponent<AnimatorComponent>()) {
            if(animator->boneBake.active) {
                result.reasons|=Bit(Reason::MORPH_OR_BAKE);
                result.inputFlags|=InputBit(InputFlag::BONE_BAKE_ACTIVE);
                if(animator->boneBake.source)result.inputFlags|=InputBit(InputFlag::BONE_BAKE_WITH_SOURCE);
                if(animator->boneBakeMode>=static_cast<int>(BoneBakeMode::GPU_SHARED))result.inputFlags|=InputBit(InputFlag::BONE_BAKE_GPU_MODE);
            }
            break;
        }
    }
    constexpr uint32_t containers=InputBit(InputFlag::WEIGHT_MAP_PRESENT)|InputBit(InputFlag::APPLIED_MAP_PRESENT)|InputBit(InputFlag::BUFFER_VECTOR_PRESENT);
    if((result.inputFlags&containers)!=0 && !result.HasInput(InputFlag::SELECTED_MORPH_PATH) && !result.HasInput(InputFlag::BONE_BAKE_ACTIVE)) {
        /// @note Presence without a selected source is an unproved input state, not an actual morph consumer.
        result.inputFlags|=InputBit(InputFlag::CONTAINERS_WITHOUT_SELECTED_MORPH_OR_BAKE);
        result.reasons|=Bit(Reason::INPUT_PROOF_PENDING);
    }
    constexpr uint32_t nonColorReasons=Bit(Reason::SHADOW)|Bit(Reason::RAY)|Bit(Reason::OBJECT_MASK)|Bit(Reason::PROBE_GI)
        |Bit(Reason::NATIVE_SCRIPT_OR_PASS)|Bit(Reason::FIBER_OR_PROCEDURAL)|Bit(Reason::MORPH_OR_BAKE)|Bit(Reason::UNSCOPED_OR_REENTRY);
    result.conservativeNonColorRequired=(result.reasons&nonColorReasons)!=0;
    const uint32_t layer=static_cast<uint32_t>(object.layer);
    bool color=skin.lodVisible && layer<32 && (context.cullingMask&(uint32_t{1}<<layer))!=0;
    const auto* materials=object.GetComponent<MaterialComponent>();
    bool visibleSlot=false;
    for(size_t slot=0;slot<skin.SubmeshCount();++slot)
        if(skin.SubmeshMesh(slot) && (!materials || (materials->enabled && materials->SlotAt(slot).visible))) {
            visibleSlot=true;break;
        }
    color &= visibleSlot;
    if(color) {
        WorldBounds bounds{};
        if(!FiniteCullingView(context) || !StoredPoseBounds(object,skin,bounds))result.reasons|=Bit(Reason::UNPROVEN_BOUNDS);
        else {
            bounds.radius+=(std::max)(context.cullingBoundsPadding,0.0f);
            if(!std::isfinite(bounds.radius))result.reasons|=Bit(Reason::UNPROVEN_BOUNDS);
            else {
                const float layerDistance=context.cullLayerDistances[layer];
                const float distance=context.hasLayerCullDistances && layerDistance>0 ? layerDistance:context.cullMaxDistance;
                const renderer::RenderCullingView view{context.camera.m_position,context.cullCameraForward,
                    context.frustumCullingEnabled?context.cameraFrustum:nullptr,context.cullProjScaleY,
                    context.smallObjectScreenHeight,context.cullDistanceSpherical,context.cullOrthographic};
                color=renderer::EvaluateVisibility(view,{bounds.center,bounds.radius,distance})==renderer::VisibilityResult::VISIBLE;
            }
        }
    }
    if(color)result.reasons|=Bit(Reason::COLOR);
    else {
        result.provisionalColorExcluded=true;
        /// @note Count-only: neither current-pose nor coherent native-input proof has been performed.
        result.reasons|=Bit(Reason::UNPROVEN_BOUNDS)|Bit(Reason::INPUT_PROOF_PENDING)|Bit(Reason::MATERIAL_PROOF_PENDING);
    }
    if(skin.gpuSkinningContent.empty() || skin.skinnedBufferModel!=skin.model)
        result.reasons|=Bit(Reason::INITIAL_OR_FAILED);
    if(!scene.scopeActive)result.reasons|=Bit(Reason::UNSCOPED_OR_REENTRY);
    return result;
}
}



namespace {

/// @note CS のスレッドグループ幅。SkinningCompute.cs.hlsl の SKINNING_GROUP_SIZE と一致させること。
constexpr uint32_t kSkinningGroupSize = 64u;

/// @note StructuredBuffer は密詰め (cbuffer のような 16 バイト整列パディングが入らない) なので、
/// @note HLSL 側の struct と C++ 側でオフセットが 1 対 1 に対応する。
/// @note ここがずれると頂点が明後日の方向へ飛ぶ形で壊れるため、サイズで固定しておく。
static_assert(sizeof(renderer::SkinnedVertex) == 76,
    "SkinSrcVertex in SkinningCompute.cs.hlsl must match renderer::SkinnedVertex (76 bytes)");
static_assert(sizeof(renderer::Vertex) == 60,
    "SkinnedOutVertex in SkinningCompute.cs.hlsl must match renderer::Vertex (60 bytes)");

/// @note SkinningConstants (b0) — CS 側と一致させること。
struct SkinningCB {
    uint32_t vertexCount = 0;
    uint32_t _pad0 = 0;
    uint32_t _pad1 = 0;
    uint32_t _pad2 = 0;
};

struct EntityIDHash {
    size_t operator()(const EntityID& id) const noexcept
    {
        size_t result = std::hash<uint32_t>{}(id.index);
        result ^= std::hash<uint32_t>{}(id.generation) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        return result;
    }
};

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

/// @note 入力頂点の StructuredBuffer は Mesh 単位で共有する (ポーズに依存しない生データのため)。同じ
/// @note モデルを何体出しても入力は 1 本で足りる。Mesh の破棄・再生成でアドレスが再利用されても古い
/// @note 頂点バッファを掴まないよう、CPU 配列と元 GPU バッファの世代を値としてキーにする。
struct SourceVertexCacheEntry {
    renderer::ResourceHandle<renderer::StructuredBufferTag> handle;
    uint32_t maxBoneIndex = 0;
};
std::unordered_map<SourceVertexCacheKey,
                   SourceVertexCacheEntry,
                   SourceVertexCacheKeyHash> g_srcVertexCache;

/// @note ボーンパレットはポーズが毎フレーム変わるので、Animator ごとに 1 本持たずフレームごとに借りる。
/// @note DX12 の読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy のため、1 本を使い回すと
/// @note GPU がまだ実行中の前フレームのスキニングが今フレームのポーズを読んでしまう (詳細は
/// @note `DynamicBufferPool.hpp`)。
renderer::DynamicStructuredBufferPool g_bonePalettePool;

/// @note このフレームに借りたパレット。Animator の下に Renderer が何本あっても転送は 1 回で済ませる。
/// @note キーは EntityID: ComponentArray::Remove は末尾要素を swap するため AnimatorComponent* は
/// @note 安定した識別子ではなく、EntityID なら generation を含むので削除後の再利用も区別できる。
std::unordered_map<EntityID,
                   renderer::ResourceHandle<renderer::StructuredBufferTag>,
                   EntityIDHash> g_framePalettes;

/// @note 使用集合はビュー単位ではなくエンジンフレーム全体で累積する。Scene View にだけ見えるメッシュを
/// @note Game View 側の判定末尾で「未使用」と誤判定すると、次フレームに immutable 入力 SRV を
/// @note 再アップロードすることになる。
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
        for (const auto& [key, entry] : g_srcVertexCache)
            resources->Release(entry.handle);
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

struct RendererWorklistStorage {
    std::vector<EntityID> entities;
    std::vector<renderer::ResourceHandle<renderer::BufferTag>> bufferHandles;
    std::vector<renderer::BufferResourceState> bufferStates;
};

struct RendererWorklistPool {
    std::vector<std::unique_ptr<RendererWorklistStorage>> entries;
    size_t depth = 0;
};

RendererWorklistPool& WorklistPool()
{
    thread_local RendererWorklistPool pool;
    return pool;
}

/// @note Component packing order differs from Scene ownership order after late registration or removal.
/// @note Nested passes borrow separate ID storage; release retains capacity without retaining Scene references.
class RendererWorklistLease {
public:
    explicit RendererWorklistLease(Scene& scene)
    {
        auto& pool = WorklistPool();
        if (pool.depth == pool.entries.size())
            pool.entries.push_back(std::make_unique<RendererWorklistStorage>());
        m_storage = pool.entries[pool.depth++].get();
        scene.CopyRendererEntities(m_storage->entities, Scene::RendererEntitySet::SKINNED);
    }

    ~RendererWorklistLease()
    {
        m_storage->entities.clear();
        m_storage->bufferHandles.clear();
        m_storage->bufferStates.clear();
        --WorklistPool().depth;
    }

    RendererWorklistLease(const RendererWorklistLease&) = delete;
    RendererWorklistLease& operator=(const RendererWorklistLease&) = delete;

    std::span<const EntityID> Entities() const { return m_storage->entities; }
    RendererWorklistStorage& Storage() const { return *m_storage; }

private:
    RendererWorklistStorage* m_storage = nullptr;
};

/// @note Each nested actor borrow owns stable numeric storage even if the pool's owner-pointer vector reallocates.
struct SignatureScratchStorage {std::vector<uint32_t> words;};
struct SignatureScratchPool {
    std::vector<std::unique_ptr<SignatureScratchStorage>> entries;
    size_t depth=0;
};
SignatureScratchPool& SignaturePool()
{
    thread_local SignatureScratchPool pool;
    return pool;
}

/// @note Sample current-thread retained capacity at pass end; row capture follows this guard and the request-counter TLS guard.
class SignatureScratchPassGauge {
public:
    explicit SignatureScratchPassGauge(AnimationSkinningCpuRow* trace):m_trace(trace){}
    ~SignatureScratchPassGauge()
    {
        if(!m_trace)return;
        const auto metrics=skinning_signature_detail::CurrentSignatureScratchMetrics();
        m_trace->signatureScratchRetainedPayloadBytes=metrics.payloadBytes;
        m_trace->signatureScratchRetainedStorageBytes=metrics.storageBytes;
        m_trace->signatureScratchRetainedPointerPayloadBytes=metrics.pointerPayloadBytes;
        m_trace->signatureScratchSlotsAtPassEnd=metrics.slots;
    }
    SignatureScratchPassGauge(const SignatureScratchPassGauge&)=delete;
    SignatureScratchPassGauge& operator=(const SignatureScratchPassGauge&)=delete;
private:
    AnimationSkinningCpuRow* m_trace;
};

/// @note Both paths preserve exact row-major float bits; tracing observes only actual reserve and push growth.
template<bool TRACE_ENABLED>
class SignatureWordWriter {
public:
    SignatureWordWriter(std::vector<uint32_t>& content,AnimationSkinningCpuRow* trace,const AnimationSkinningCpuFrame& clock)
        :m_content(content),m_trace(trace),m_clock(clock){}
    void ReserveForPose(size_t matrixCount, size_t slotCount, bool baked)
    {
        constexpr size_t MATRIX_WORDS = 16, BAKE_WORDS = 15, SLOT_WORDS = 6;
        const size_t fixedWords = 1 + (baked ? BAKE_WORDS : 0);
        assert(matrixCount <= (m_content.max_size() - fixedWords) / MATRIX_WORDS);
        const size_t poseWords = fixedWords + matrixCount * MATRIX_WORDS;
        assert(slotCount <= (m_content.max_size() - poseWords) / SLOT_WORDS);
        const size_t required = poseWords + slotCount * SLOT_WORDS;
        if (required <= m_content.capacity()) return;
        [[maybe_unused]] const size_t sizeBefore = m_content.size();
        [[maybe_unused]] const uint64_t start = TRACE_ENABLED ? m_clock.Mark() : 0;
        m_content.reserve(required);
        if constexpr (TRACE_ENABLED) {
            m_trace->signatureReserveNs += m_clock.Elapsed(start);
            ++m_trace->signatureReserveCalls;
            ++m_trace->signatureClockPairCount;
            ++m_trace->signaturePayloadAllocationCalls;
            m_trace->signaturePayloadAllocationBytes += m_content.capacity() * sizeof(uint32_t);
            m_trace->signatureGrowthRelocatedLogicalBytes += sizeBefore * sizeof(uint32_t);
        }
    }
    void AppendMatrices(std::span<const math::Matrix4> matrices)
    {
        static_assert(std::is_trivially_copyable_v<math::Matrix4>);
        static_assert(std::is_standard_layout_v<math::Matrix4>);
        static_assert(offsetof(math::Matrix4, m) == 0);
        static_assert(sizeof(float) == sizeof(uint32_t));
        static_assert(sizeof(math::Matrix4) == 16 * sizeof(uint32_t));
        if (matrices.empty()) return;
        const size_t offset = m_content.size();
        assert(matrices.size() <= (m_content.max_size() - offset) / 16);
        m_content.resize(offset + matrices.size() * 16);
        std::memcpy(m_content.data() + offset, matrices.data(), matrices.size_bytes());
    }
    void Append(uint32_t word)
    {
        if constexpr(!TRACE_ENABLED){m_content.push_back(word);}
        else{
            const size_t capacityBefore=m_content.capacity(),sizeBefore=m_content.size();
            const bool grows=sizeBefore==capacityBefore;
            const uint64_t start=grows?m_clock.Mark():0;
            m_content.push_back(word);
            if(grows){
                m_trace->signatureGrowthPushNs+=m_clock.Elapsed(start);
                ++m_trace->signatureGrowthPushCalls;
                ++m_trace->signatureClockPairCount;
            }
            if(m_content.capacity()!=capacityBefore){
                ++m_trace->signaturePayloadAllocationCalls;
                m_trace->signaturePayloadAllocationBytes+=m_content.capacity()*sizeof(uint32_t);
                m_trace->signatureGrowthRelocatedLogicalBytes+=sizeBefore*sizeof(uint32_t);
            }
        }
    }
private:
    std::vector<uint32_t>& m_content;
    AnimationSkinningCpuRow* m_trace;
    const AnimationSkinningCpuFrame& m_clock;
};

enum class DirectSignatureResult { UNAVAILABLE, DIFFERENT, EQUAL_UNREADY, EQUAL_READY };

/// @note Compare the same values used by the ordinary non-baked builder; no native lookup, retained state or publication is introduced.
DirectSignatureResult CompareCurrentSignature(const SkinnedMeshRenderer& skin,
    std::span<const math::Matrix4> matrices, std::span<const renderer::BufferResourceState> states, bool allowPending = false)
{
    const auto& published = skin.gpuSkinningContent;
    if ((!skin.gpuSkinnedThisFrame && !allowPending) || states.size() != skin.SubmeshCount() * 2) return DirectSignatureResult::UNAVAILABLE;
    size_t offset = 0;
    const auto word = [&](uint32_t value) {
        if (offset >= published.size() || published[offset] != value) return false;
        ++offset; return true;
    };
    if (!word(static_cast<uint32_t>(matrices.size()))) return DirectSignatureResult::DIFFERENT;
    static_assert(sizeof(math::Matrix4) == 16 * sizeof(uint32_t));
    static_assert(std::is_trivially_copyable_v<math::Matrix4>);
    if (matrices.size() > (published.size() - offset) / 16
        || (!matrices.empty() && std::memcmp(published.data() + offset, matrices.data(), matrices.size_bytes()) != 0))
        return DirectSignatureResult::DIFFERENT;
    offset += matrices.size() * 16;
    bool ready = skin.gpuSkinnedThisFrame;
    for (size_t slot = 0; slot < skin.SubmeshCount(); ++slot) {
        const auto* mesh = skin.SubmeshMesh(slot);
        if (!mesh) return DirectSignatureResult::UNAVAILABLE;
        const auto source = skin.ResolveSlotVertexBuffer(slot, mesh->vertexBuffer);
        const auto version = states[slot * 2].contentVersion;
        if (!word(skin.SubmeshAt(slot)) || !word(source.id) || !word(source.gen)
            || !word(static_cast<uint32_t>(version)) || !word(static_cast<uint32_t>(version >> 32))
            || !word(static_cast<uint32_t>(mesh->cpuSkinnedVertices.size()))) return DirectSignatureResult::DIFFERENT;
        const auto& output = states[slot * 2 + 1];
        ready &= output.live && output.stride == sizeof(renderer::Vertex)
            && output.sizeBytes >= mesh->cpuSkinnedVertices.size() * sizeof(renderer::Vertex)
            && output.contentVersion != 0 && output.bindlessUavIndex != renderer::INVALID_BINDLESS_INDEX;
    }
    if (offset != published.size()) return DirectSignatureResult::DIFFERENT;
    return ready ? DirectSignatureResult::EQUAL_READY : DirectSignatureResult::EQUAL_UNREADY;
}

/// @note BeginSkinningCacheFrame — 前フレームの全ビューで未使用だったキャッシュだけを回収する。
void BeginSkinningCacheFrame(renderer::ResourceManager& resources, uint64_t frameStamp, AnimationSkinningCpuRow* trace)
{
    if (g_cacheOwner != resources.GetInstanceId() || g_cacheResetVersion != resources.GetResetVersion()) {
        ReleaseSourceSkinningCache();
        g_cacheOwner = resources.GetInstanceId();
        g_cacheResetVersion = resources.GetResetVersion();
    }
    if (g_cacheUsageFrame == frameStamp) return;

    if (g_cacheUsageFrame != (std::numeric_limits<uint64_t>::max)()) {
        for (auto it = g_srcVertexCache.begin(); it != g_srcVertexCache.end();) {
            if (g_usedSourceKeys.contains(it->first)) {
                ++it;
                continue;
            }
            if (it->second.handle.IsValid()) {
                if(trace)++trace->sourceReleaseCalls;
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
    renderer::buffer_state_detail::BufferStateReadScope* bufferReads = nullptr, uint64_t* capturedVersion = nullptr)
{
    /// @note cpuSkinnedVertices はインポート時に保持される。空なら GPU スキニングは行えない。
    const auto* sourceBuffer = bufferReads ? bufferReads->Get(source) : resources.Get(source);
    if (mesh.cpuSkinnedVertices.empty() || mesh.cpuSkinnedVertices.size() > UINT32_MAX
        || !sourceBuffer || sourceBuffer->GetContentVersion() == 0
        || sourceBuffer->GetStride() != sizeof(renderer::SkinnedVertex)
        || sourceBuffer->GetSize() < mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex))
        return {};

    const SourceVertexCacheKey key{
        mesh.cpuSkinnedVertices.data(),
        static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        source, sourceBuffer->GetContentVersion()};
    if (capturedVersion) *capturedVersion = key.contentVersion;
    g_usedSourceKeys.insert(key);
    const auto it = g_srcVertexCache.find(key);
    if (it != g_srcVertexCache.end()) {
        if (resources.Get(it->second.handle)) {
            if(trace)++trace->sourceCacheHits;
            return it->second.maxBoneIndex < boneCount ? it->second.handle : renderer::ResourceHandle<renderer::StructuredBufferTag>{};
        }
        g_srcVertexCache.erase(it);
    }

    if(trace)++trace->sourceCacheMisses;
    /// @note 元 mesh と同じ handle でも内容更新を持つため、authoritative な版付き入力を読む。
    std::vector<renderer::SkinnedVertex> currentVertices(mesh.cpuSkinnedVertices.size());
    if (!sourceBuffer->CopyData(0, currentVertices.size() * sizeof(renderer::SkinnedVertex), currentVertices.data())) return {};
    if(trace)trace->sourceCopyBytes+=currentVertices.size()*sizeof(renderer::SkinnedVertex);
    uint32_t maxBoneIndex = 0;
    for (const auto& vertex : currentVertices)
        for (uint32_t index : vertex.boneIndices) maxBoneIndex = (std::max)(maxBoneIndex, index);
    /// @note Packed palettes must not read into the next character, including zero-weight source indices.
    if (maxBoneIndex >= boneCount) return {};
    if(trace)++trace->sourceCreateAttempts;
    const auto handle = resources.CreateGpuLocalStructuredBuffer(
        currentVertices.data(),
        static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        static_cast<uint32_t>(sizeof(renderer::SkinnedVertex)));

    if(trace && handle.IsValid())++trace->sourceCreateSuccesses;
    g_srcVertexCache[key] = {handle, maxBoneIndex};
    return handle;
}

/// @brief AcquireBonePalette — アニメーターのボーンパレットを今フレーム用に借りて転送する。
/// @note cbuffer ではなく StructuredBuffer を使う: 頂点あたり 4 回の動的インデックスアクセスが
/// @note 走り、128 要素の cbuffer 配列への動的アクセスは定数キャッシュの高速経路を外れやすい。
renderer::ResourceHandle<renderer::StructuredBufferTag> AcquireBonePalette(
    renderer::ResourceManager& resources,
    EntityID animatorEntity,
    const std::vector<math::Matrix4>& matrices, AnimationSkinningCpuRow* trace)
{
    if (matrices.empty()) return {};
    if (const auto it = g_framePalettes.find(animatorEntity); it != g_framePalettes.end())
        return it->second;

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

} /// @note namespace

namespace skinning_demand_detail {
struct GpuOutputDemandState {
    struct Publication {
        EntityID owner = EntityID::INVALID;
        uint64_t lifetime = 0, generation = 0;
        const asset::Model* model = nullptr;
        std::shared_ptr<renderer::PreparedSkinningTicket> ticket;
        uint64_t vertices = 0;
        uint32_t dispatches = 0;
        bool submitted = false;
    };
    explicit GpuOutputDemandState(RenderPassContext& value) : context(value), observer(&BeforeCallback, this) {}
    static bool BeforeCallback(void* value) { return static_cast<GpuOutputDemandState*>(value)->Complete(); }
    bool SubmitTicket(const std::shared_ptr<renderer::PreparedSkinningTicket>& ticket)
    {
        const auto writes = ticket->Writes();
        const bool previouslyRecorded = !writes.empty() && writes.front().recorded;
        const bool submitted = renderer::SubmitPreparedSkinning(ticket);
        if (submitted && !previouslyRecorded) {
            context.statsSkinningDemandSubmitted += writes.size();
            context.statsSkinningDemandSkipped -= (std::min)(context.statsSkinningDemandSkipped, static_cast<uint64_t>(writes.size()));
            uint64_t vertices = 0;
            for (const auto& write : writes) vertices += write.vertexCount;
            g_frameStats.dispatches += static_cast<uint32_t>(writes.size()); g_frameStats.vertices += vertices;
            context.statsSkinningDispatchCount += static_cast<uint32_t>(writes.size());
            context.statsSkinningVertexCount += vertices;
        }
        return submitted;
    }
    bool Complete()
    {
        if (!committed) {
            cancelled = true;
            if (publications.empty()) return true;
        }
        bool success = true;
        std::array<const renderer::PreparedSkinningTicket*, 16> submittedTickets{};
        std::array<bool, 16> submittedResults{};
        size_t submittedCount = 0;
        /// @note 全 native 結果を先に確定する。以後の所有値 COW は Logger/pump/Scene 再抽出を呼ばない。
        for (auto& publication : publications) {
            size_t index = 0;
            while (index < submittedCount && submittedTickets[index] != publication.ticket.get()) ++index;
            if (index < submittedCount) publication.submitted = submittedResults[index];
            else {
                publication.submitted = SubmitTicket(publication.ticket);
                if (submittedCount < submittedTickets.size()) {
                    submittedTickets[submittedCount] = publication.ticket.get();
                    submittedResults[submittedCount++] = publication.submitted;
                }
            }
            success = publication.submitted && success;
        }
        for (const auto& publication : publications) {
            auto* component = context.scene.GetComponent<SkinnedMeshRenderer>(publication.owner);
            if (!component || context.scene.GetRenderSceneGeneration() != publication.generation
                || context.scene.GetComponentLifetime<SkinnedMeshRenderer>(publication.owner) != publication.lifetime
                || component->model != publication.model || component->preparedSkinning != publication.ticket) continue;
            bool written = publication.submitted && publication.ticket->ResourceInstance() == context.resources.GetInstanceId()
                && publication.ticket->ResourceEpoch() == context.resources.GetResetVersion();
            const auto writes = publication.ticket->Writes();
            for (size_t slot = 0; slot < component->SubmeshCount(); ++slot) {
                const uint32_t submesh = component->SubmeshAt(slot);
                const uint32_t row = submesh < component->preparedSkinningRows.size() ? component->preparedSkinningRows[submesh] : UINT32_MAX;
                written = written && row < writes.size() && writes[row].recorded
                    && writes[row].resultingVersion > writes[row].previousVersion && publication.ticket->IsWriteCurrent(row);
            }
            component->gpuSkinnedThisFrame = written;
            component->gpuSkinningFrame = written ? Time::frameCount : (std::numeric_limits<uint64_t>::max)();
            component->gpuSkinningVertexCount = written ? publication.vertices : 0;
            component->gpuSkinningDispatchCount = written ? publication.dispatches : 0;
            if (!written) component->gpuSkinningContent.clear();
        }
        if (auto previous = CompletePreparedSkinningSnapshot(context)) retiredSnapshots.push_back(std::move(previous));
        return success;
    }
    RenderPassContext& context;
    std::vector<Publication> publications;
    std::vector<std::shared_ptr<const renderer::RenderScene>> retiredSnapshots;
    bool cancelled = false, committed = false;
    core::ExternalCallbackScope observer;
};
namespace { thread_local GpuOutputDemandState* s_gpuDemandState = nullptr; }

bool NeedsCurrentOutput(RenderPassContext& context, GameObject& object,
    const SkinnedMeshRenderer& skin, const AnimatorComponent* animator)
{
    if ((animator && animator->boneBake.active) || !skin.morphWeights.empty()
        || !skin.appliedMorphWeights.empty()
        || (object.GetComponent<FiberComponent>() && object.GetComponent<FiberComponent>()->m_enabled)
        || !FiniteCullingView(context) || context.selectionOutlineEnabled
        || !context.settings.objectMaskRequests.empty()) return true;
    for (const auto& handle : skin.morphVertexBuffers) if (handle.IsValid()) return true;
    WorldBounds bounds;
    if (!StoredPoseBounds(object, skin, bounds)) return true;
    renderer::RenderObject captured;
    captured.layer = static_cast<uint32_t>(object.layer);
    captured.lodVisible = skin.lodVisible;
    captured.castShadows = skin.castShadows;
    captured.colorEligible = true;
    captured.boundsCenter = bounds.center;
    captured.boundsRadius = bounds.radius;
    return renderer::RequiresComputedSkinning(context, captured);
}

GpuOutputDemandScope::GpuOutputDemandScope(RenderPassContext& context) : m_previous(s_gpuDemandState)
{
    s_gpuDemandState = nullptr;
    if (!renderer::GpuSkinningDemandEnabled() || m_previous || !context.scene.GetUserRenderPasses().empty()
        || !renderer::SupportsCapturedSkinning(context.renderer, context.resources)) return;
    for (EntityID id : context.scene.GetEntities<ScriptComponent>()) {
        const auto* object = context.scene.GetGameObject(id);
        const auto* scripts = context.scene.GetComponent<ScriptComponent>(id);
        if (!object || !object->activeInHierarchy() || !scripts) continue;
        for (const auto& entry : scripts->scripts)
            if (entry.script && entry.script->enabled && !ManagedScriptRuntime::Instance().FindAdapter(entry.script.get())) return;
    }
    m_state = std::make_unique<GpuOutputDemandState>(context);
    s_gpuDemandState = m_state.get();
}
GpuOutputDemandScope::~GpuOutputDemandScope()
{
    s_gpuDemandState = m_previous;
}
} /// @note namespace skinning_demand_detail

skinning_signature_detail::SignatureScratchLease::SignatureScratchLease(AnimationSkinningCpuRow* trace)
{
    auto& pool=SignaturePool();
    if(pool.depth==pool.entries.size()){
        const size_t pointerCapacityBefore=pool.entries.capacity();
        auto storage=std::make_unique<SignatureScratchStorage>();
        pool.entries.push_back(std::move(storage));
        if(trace){
            ++trace->signatureScratchStorageAllocationCalls;
            trace->signatureScratchStorageAllocationBytes+=sizeof(SignatureScratchStorage);
            if(pool.entries.capacity()!=pointerCapacityBefore){
                ++trace->signatureScratchPoolPointerAllocationCalls;
                trace->signatureScratchPoolPointerAllocationBytes+=pool.entries.capacity()*sizeof(std::unique_ptr<SignatureScratchStorage>);
            }
        }
    }
    m_words=&pool.entries[pool.depth]->words;
    m_words->clear();
    ++pool.depth;
}

skinning_signature_detail::SignatureScratchLease::~SignatureScratchLease()
{
    m_words->clear();
    --SignaturePool().depth;
}

skinning_signature_detail::ScratchMetrics skinning_signature_detail::CurrentSignatureScratchMetrics()
{
    const auto& pool=SignaturePool();
    ScratchMetrics metrics;
    metrics.slots=pool.entries.size();
    metrics.pointerCapacity=pool.entries.capacity();
    metrics.activeLeases=pool.depth;
    metrics.storageBytes=pool.entries.size()*sizeof(SignatureScratchStorage);
    metrics.pointerPayloadBytes=pool.entries.capacity()*sizeof(std::unique_ptr<SignatureScratchStorage>);
    for(const auto& entry:pool.entries)metrics.payloadBytes+=entry->words.capacity()*sizeof(uint32_t);
    return metrics;
}

void ExecuteSkinningComputePass(RenderPassContext& ctx)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::SkinningCompute");
    AnimationSkinningCpuFrame skinCpu(ctx.resources.FrameStamp(),ctx.scene.GetRenderSceneGeneration(),ctx.resources.GetResetVersion(),ctx.resources.GetInstanceId());
    auto* trace=skinCpu.TraceRow();
    skinning_demand_detail::SceneDemandSnapshot demand;
    if(trace) {
        /// @note Entry snapshot is diagnostic only; no omission, publication or resource-lifetime decision uses it.
        demand=skinning_demand_detail::ObserveSceneDemand(ctx);
        trace->demandScopeActive=demand.scopeActive;
        trace->sceneObjectCount=demand.sceneObjects;
        trace->sceneAnimatorComponentCount=demand.animatorComponents;
        trace->sceneSkinnedRendererComponentCount=demand.skinnedRendererComponents;
        trace->activeNativeScriptEntries=demand.activeNativeScriptEntries;
        trace->activeManagedScriptEntries=demand.activeManagedScriptEntries;
        trace->activeReflectionProbes=demand.activeReflectionProbes;
        trace->activeReflectionSceneProbes=demand.activeReflectionSceneProbes;
        trace->activeLightProbeVolumes=demand.activeLightProbeVolumes;
        trace->activeLightProbeBakeConsumers=demand.activeLightProbeBakeConsumers;
        trace->activeInstancedComponents=demand.activeInstancedComponents;
    }
    SignatureScratchPassGauge scratchGauge(trace);
    renderer::skinning_trace_detail::RequestCounterScope requestTrace(trace?&trace->requests:nullptr);
    if(trace){
        const uint64_t probe=skinCpu.Mark();
        trace->signatureClockProbeNs=skinCpu.Elapsed(probe);
    }
    if (AnimationGpuTraceRequested()) CaptureAnimationGpuWork(ctx.renderer.GpuProfGetSnapshot());
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;
    renderer::buffer_state_detail::BufferStateReadScope bufferReads(resources, ctx.renderer);
    auto* gpuDemand = skinning_demand_detail::s_gpuDemandState;
    if (gpuDemand && (&gpuDemand->context != &ctx || gpuDemand->cancelled || gpuDemand->committed)) gpuDemand = nullptr;
    bool directReuseMarker = false, fullPreparationMarker = false;

    const bool ownedBatchRequested = renderer::UseOwnedSkinningBatch();
    ctx.statsSkinningDemandMode = renderer::GpuSkinningDemandEnabled() ? 1 : 0;
    ctx.skinningRequests.clear();
    ctx.skinningMatrices.clear();
    std::unordered_map<EntityID, uint32_t, EntityIDHash> packedOffsets;
    const bool packed = renderer::ResolveSkinningBatchShader(ctx).IsValid();
    ctx.skinningExecuted = false;
    const auto invalidatePublication = [](SkinnedMeshRenderer& smr) {
        smr.gpuSkinnedThisFrame = false;
        smr.gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
        smr.gpuSkinningVertexCount = 0;
        smr.gpuSkinningDispatchCount = 0;
        smr.gpuSkinningContent.clear();
        smr.preparedSkinning.reset();
        smr.preparedSkinningRows.clear();
        smr.preparedSkinningFrame = (std::numeric_limits<uint64_t>::max)();
    };
    const bool skinningProgramReady = resources.Get(h.skinningComputeCS) && resources.Get(h.skinningCB);
    const bool replayAvailable = packed && skinningProgramReady && ctx.renderer.GetCapabilities().bindless;
    std::vector<EntityID> restoreCpuActors;
    for (EntityID id : ctx.scene.GetEntities<AnimatorComponent>()) {
        auto* animator = ctx.scene.GetComponent<AnimatorComponent>(id);
        if (!animator || animator->boneBakeMode != static_cast<int>(BoneBakeMode::GPU_POSE_SHARED)) continue;
        /// @note Offscreen individuals need no CPU palette; a newly visible individual is repaired before drawing if replay fails.
        animator->boneBake.gpuReady = replayAvailable;
        animator->boneBake.gpuOwner = resources.GetInstanceId(); animator->boneBake.gpuEpoch = resources.GetResetVersion();
        if (!replayAvailable && animator->boneBake.cpuDeferred) restoreCpuActors.push_back(id);
    }
    /// @note Restoration may enter the resource executor; keep identities rather than a live packed component span across it.
    for (EntityID id : restoreCpuActors)
        if (auto* animator = ctx.scene.GetComponent<AnimatorComponent>(id)) MaterializeBoneBakeCpu(*animator, resources, true);
    if (!skinningProgramReady) {
        /// @note Non-null typed handles can be stale. A missing program/CB must not expose an old pose as current deformation.
        RendererWorklistLease worklist(ctx.scene);
        for (EntityID entity : worklist.Entities())
            if (auto* smr = ctx.scene.GetComponent<SkinnedMeshRenderer>(entity)) {
                if(trace)++trace->renderersVisited;
                invalidatePublication(*smr);
            }
        return;
    }
    struct OwnedSlotWitness {
        size_t slot = 0;
        size_t submesh = 0;
        const renderer::Mesh* mesh = nullptr;
        renderer::ResourceHandle<renderer::BufferTag> source, output;
        uint32_t vertices = 0;
        uint64_t acceptedSourceVersion = 0;
        size_t requestIndex = 0;
    };
    struct OwnedBakeWitness {
        int mode = 0;
        bool active = false;
        int generation = 0;
        std::shared_ptr<const asset::BakedBoneAnimation> source, target;
        uint32_t first = 0, second = 0, alpha = 0, targetFirst = 0, targetSecond = 0, targetAlpha = 0, blend = 0;
    };
    struct PendingPublication {
        std::vector<std::pair<renderer::ResourceHandle<renderer::BufferTag>, uint64_t>> outputs;
        EntityID owner = EntityID::INVALID, animator = EntityID::INVALID;
        uint64_t componentLifetime = 0, animatorLifetime = 0, sceneGeneration = 0;
        uint64_t resourceOwner = 0, resourceEpoch = 0, resourceFrame = 0, frame = 0;
        uint64_t vertices = 0;
        uint32_t dispatches = 0;
        const asset::Model* model = nullptr;
        size_t modelMeshCount = 0, slotCount = 0;
        std::vector<math::Matrix4> pose;
        std::vector<uint32_t> content;
        std::vector<OwnedSlotWitness> slots;
        std::vector<renderer::CapturedSkinningRequest> capturedRequests;
        std::vector<size_t> capturedRequestIndices;
        bool demandCandidate = false, deferred = false;
        OwnedBakeWitness bake;
    };

    std::vector<PendingPublication> pendingPublications;
    std::vector<BoneBakeSkinRequest> bakeRequests;

    BeginSkinningCacheFrame(resources, Time::frameCount, trace);
    /// @note EntityID は Scene 内だけで一意。別 Scene の同番号の Animator とパレットを共有しない。
    /// @note 計算済み Renderer は gpuSkinningFrame で再利用するため、既存出力を再 Dispatch しない。
    g_framePalettes.clear();
    auto& usedSourceKeys = g_usedSourceKeys;
    /// @note 各 submesh の Dispatch は別出力へ書き、相互依存しない。DX12 は UAV バリアを
    /// @note Dispatch ごとに発行せず、このバッチを閉じる時点の 1 回へ集約する。

    RendererWorklistLease worklist(ctx.scene);
    for (EntityID entity : worklist.Entities()) {
        auto* object = ctx.scene.GetGameObject(entity);
        if (!object) continue;
        auto& go = *object;
        if (!go.activeInHierarchy()) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if(trace && smr)++trace->renderersVisited;
        const bool rayDemand = ctx.experimentalRayTracingEnabled
            && (ctx.settings.modeRequest.mode != renderer::RenderMode::RASTER
                || renderer::IsRayDebugView(ctx.settings.viewMode));
        if (!smr || !smr->enabled || (!smr->lodVisible && !rayDemand) || !smr->model) continue;

        if(trace) {
            ++trace->renderersEligible;++trace->legacyEnteredRenderers;
            const auto observation=skinning_demand_detail::ObserveRendererDemand(ctx,go,*smr,demand);
            ++trace->demandObservedRenderers;
            if(observation.provisionalColorExcluded)++trace->provisionalColorExcludedRenderers;
            if(observation.conservativeNonColorRequired)++trace->conservativeNonColorRequiredRenderers;
            if(observation.provisionalColorExcluded && !observation.conservativeNonColorRequired)
                ++trace->provisionalNoKnownConsumerRenderers;
            for(uint32_t reason=0;reason<skinning_demand_detail::REASON_COUNT;++reason)
                if((observation.reasons&(uint32_t{1}<<reason))!=0)++trace->outputDemandReasonRenderers[reason];
            for(uint32_t flag=0;flag<skinning_demand_detail::INPUT_FLAG_COUNT;++flag)
                if((observation.inputFlags&(uint32_t{1}<<flag))!=0)++trace->inputObservedRenderers[flag];
        }

        AnimatorComponent* anim = nullptr;
        EntityID animatorEntity = EntityID::INVALID;
        for (GameObject* current = &go; current; current = current->GetParent()) {
            if (auto* candidate = current->GetComponent<AnimatorComponent>()) {
                anim = candidate;
                animatorEntity = current->GetID();
                break;
            }
        }
        const auto* matrices = anim && !anim->boneMatrices.empty() ? &anim->boneMatrices
            : smr->model->skeleton && !smr->model->skeleton->referencePose.empty()
                ? &smr->model->skeleton->referencePose : nullptr;
        if (!matrices) {
            /// @note ポーズ未解決時に、レイ用として単位行列や未変形頂点を公開しない。
            smr->gpuSkinnedThisFrame = false;
            smr->gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
            smr->gpuSkinningVertexCount = 0;
            smr->gpuSkinningDispatchCount = 0;
            smr->gpuSkinningContent.clear();
            smr->preparedSkinning.reset();
            smr->preparedSkinningRows.clear();
            smr->preparedSkinningFrame = (std::numeric_limits<uint64_t>::max)();
            continue;
        }

        /// @note モデルが差し替わったら、旧モデルの頂点数で確保したバッファを捨てる。
        if (smr->skinnedBufferModel != smr->model) {
            for (auto& handle : smr->skinnedVertexBuffers)
                if (handle.IsValid()) {
                    if(trace)++trace->outputReleaseCalls;
                    resources.Release(handle);
                }
            smr->skinnedVertexBuffers.clear();
            smr->skinnedBufferModel = smr->model;
            smr->gpuSkinnedThisFrame = false;
            smr->gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
            smr->gpuSkinningVertexCount = 0;
            smr->gpuSkinningDispatchCount = 0;
            smr->gpuSkinningContent.clear();
        }

        /// @note 出力バッファ配列は model->meshes と同じ添字で確保する。ローカルスロット数まで
        /// @note 詰めないのは、ShadowPass / ForwardPass も同じ添字で引くため。多少の空きスロット
        /// @note より、添字が 1 つに定まっていることを優先する。
        const size_t meshCount = smr->model->meshes.size();
        smr->skinnedVertexBuffers.resize(meshCount);

        auto& bufferHandles = worklist.Storage().bufferHandles;
        auto& bufferStates = worklist.Storage().bufferStates;
        const auto readSceneGeneration = ctx.scene.GetRenderSceneGeneration();
        const auto readComponentLifetime = ctx.scene.GetComponentLifetime<SkinnedMeshRenderer>(entity);
        const auto readResourceOwner = resources.GetInstanceId(), readResourceEpoch = resources.GetResetVersion();
        const auto readResourceFrame = resources.FrameStamp();
        const auto* readModel = smr->model;
        const size_t readSlotCount = smr->SubmeshCount();
        bufferHandles.clear();
        for (size_t slot = 0; slot < readSlotCount; ++slot) {
            const auto* mesh = smr->SubmeshMesh(slot);
            bufferHandles.push_back(mesh ? smr->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer)
                                         : renderer::ResourceHandle<renderer::BufferTag>{});
            bufferHandles.push_back(mesh ? smr->ResolveSlotSkinnedVertexBuffer(slot)
                                         : renderer::ResourceHandle<renderer::BufferTag>{});
        }
        bufferStates.resize(bufferHandles.size());
        bufferReads.Read(bufferHandles, bufferStates);
        /// @note The drain may pump Scene edits; reacquire packed components and reject identities captured before the wait.
        auto* resumedObject = ctx.scene.GetGameObject(entity);
        auto* resumedRenderer = ctx.scene.GetComponent<SkinnedMeshRenderer>(entity);
        if (ctx.scene.GetRenderSceneGeneration() != readSceneGeneration || resumedObject != object || !resumedRenderer
            || ctx.scene.GetComponentLifetime<SkinnedMeshRenderer>(entity) != readComponentLifetime
            || resources.GetInstanceId() != readResourceOwner || resources.GetResetVersion() != readResourceEpoch
            || resources.FrameStamp() != readResourceFrame) continue;
        smr = resumedRenderer;
        if (smr->model != readModel) {
            if (smr->skinnedBufferModel != smr->model) invalidatePublication(*smr);
            continue;
        }
        if (!go.activeInHierarchy() || !smr->enabled || (!smr->lodVisible && !rayDemand) || !smr->model) continue;
        if (smr->model->meshes.size() != meshCount) { invalidatePublication(*smr); continue; }
        /// @note The same component can lose output slots during the pump without changing its lifetime or model.
        if (smr->skinnedVertexBuffers.size() < meshCount) smr->skinnedVertexBuffers.resize(meshCount);
        bool useBufferStates = smr->model == readModel && smr->SubmeshCount() == readSlotCount;
        for (size_t slot = 0; useBufferStates && slot < readSlotCount; ++slot) {
            const auto* mesh = smr->SubmeshMesh(slot);
            useBufferStates = (mesh ? smr->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer)
                                   : renderer::ResourceHandle<renderer::BufferTag>{}) == bufferHandles[slot * 2]
                && (mesh ? smr->ResolveSlotSkinnedVertexBuffer(slot)
                         : renderer::ResourceHandle<renderer::BufferTag>{}) == bufferHandles[slot * 2 + 1];
        }
        /// @note Pose vectors can move during the same pump; resolve their current owner only after the native read has returned.
        anim = nullptr;
        animatorEntity = EntityID::INVALID;
        for (GameObject* current = &go; current; current = current->GetParent()) {
            if (auto* candidate = current->GetComponent<AnimatorComponent>()) {
                anim = candidate;
                animatorEntity = current->GetID();
                break;
            }
        }
        if (packed && anim && anim->boneBake.active && anim->boneBakeMode >= static_cast<int>(BoneBakeMode::GPU_SHARED))
            bakeRequests.push_back({UINT32_MAX, &anim->boneBake, smr->model, animatorEntity, ctx.scene.GetComponentLifetime<AnimatorComponent>(animatorEntity)});
        matrices = anim && !anim->boneMatrices.empty() ? &anim->boneMatrices
            : smr->model->skeleton && !smr->model->skeleton->referencePose.empty()
                ? &smr->model->skeleton->referencePose : nullptr;
        if (!matrices) { invalidatePublication(*smr); continue; }

        const uint64_t frameStamp = Time::frameCount;
        const bool demandRequired = !gpuDemand || gpuDemand->cancelled
            || skinning_demand_detail::NeedsCurrentOutput(ctx, go, *smr, anim);
        if (ctx.statsSkinningDemandMode != 0) {
            if (demandRequired) ++ctx.statsSkinningDemandRequired;
            else ++ctx.statsSkinningDemandCandidates;
        }
        if ((!gpuDemand || gpuDemand->cancelled) && smr->preparedSkinning) {
            const bool recorded = smr->preparedSkinning->ResourceInstance() == resources.GetInstanceId()
                && smr->preparedSkinning->ResourceEpoch() == resources.GetResetVersion()
                && renderer::SubmitPreparedSkinning(smr->preparedSkinning);
            smr->gpuSkinnedThisFrame = recorded;
            smr->gpuSkinningFrame = recorded ? frameStamp : (std::numeric_limits<uint64_t>::max)();
        }
        const auto reuseCurrentOutput = [&] {
            if (smr->preparedSkinning) smr->preparedSkinningFrame = frameStamp;
            if (trace) ++trace->alreadyCurrentReuse;
            const size_t reusedSlotCount = smr->SubmeshCount();
            for (size_t slot = 0; slot < reusedSlotCount; ++slot) {
                const uint32_t submeshIndex = smr->SubmeshAt(slot);
                if (submeshIndex == UINT32_MAX || submeshIndex >= meshCount) continue;
                const auto& meshPtr = smr->model->meshes[submeshIndex];
                if (!meshPtr) continue;
                const auto source = smr->ResolveSlotVertexBuffer(slot, meshPtr->vertexBuffer);
                uint64_t version = 0;
                if (useBufferStates) version = bufferStates[slot * 2].contentVersion;
                else if (const auto* buffer = resources.Get(source)) version = buffer->GetContentVersion();
                usedSourceKeys.insert(SourceVertexCacheKey{meshPtr->cpuSkinnedVertices.data(),
                    static_cast<uint32_t>(meshPtr->cpuSkinnedVertices.size()), source, version});
            }
            if (smr->gpuSkinningFrame != frameStamp) {
                smr->gpuSkinningVertexCount = 0;
                smr->gpuSkinningDispatchCount = 0;
                smr->gpuSkinningFrame = frameStamp;
            }
        };
        DirectSignatureResult directSignature = DirectSignatureResult::UNAVAILABLE;
        if (useBufferStates && (smr->gpuSkinnedThisFrame || (gpuDemand && smr->preparedSkinning)) && !(anim && anim->boneBake.active)) {
            const uint64_t compareStart = trace ? skinCpu.Mark() : 0;
            directSignature = CompareCurrentSignature(*smr, *matrices, bufferStates, gpuDemand && smr->preparedSkinning != nullptr);
            if (trace) {
                trace->signatureCompareNs += skinCpu.Elapsed(compareStart);
                ++trace->signatureCompareCalls;
                ++trace->signatureClockPairCount;
            }
        }
        if (gpuDemand && !gpuDemand->cancelled && smr->preparedSkinning && bufferReads.HasCurrentReadBoundary()
            && (directSignature == DirectSignatureResult::EQUAL_READY || directSignature == DirectSignatureResult::EQUAL_UNREADY)
            && smr->preparedSkinning->ResourceInstance() == resources.GetInstanceId()
            && smr->preparedSkinning->ResourceEpoch() == resources.GetResetVersion()) {
            auto ticket = smr->preparedSkinning;
            smr->preparedSkinningFrame = frameStamp;
            uint64_t vertices = 0;
            uint32_t dispatches = 0;
            for (size_t slot = 0; slot < smr->SubmeshCount(); ++slot)
                if (const auto* mesh = smr->SubmeshMesh(slot)) { vertices += mesh->cpuSkinnedVertices.size(); ++dispatches; }
            gpuDemand->publications.push_back({entity, readComponentLifetime, readSceneGeneration, smr->model, ticket, vertices, dispatches});
            if (!ticket->Writes().empty() && !ticket->Writes().front().recorded) {
                ctx.statsSkinningDemandPrepared += dispatches;
                ctx.statsSkinningDemandSkipped += dispatches;
            }
            if (demandRequired) {
                /// @note 元 ticket は旧 snapshot と同じ入力を所有し、可視な別 view でも再準備しない。
                bool recorded = true, needsSubmit = false;
                const auto writes = ticket->Writes();
                for (size_t slot = 0; slot < smr->SubmeshCount(); ++slot) {
                    const uint32_t submesh = smr->SubmeshAt(slot);
                    const uint32_t row = submesh < smr->preparedSkinningRows.size() ? smr->preparedSkinningRows[submesh] : UINT32_MAX;
                    if (row >= writes.size()) { recorded = false; break; }
                    if (!writes[row].recorded) needsSubmit = true;
                    else recorded = ticket->IsWriteCurrent(row) && recorded;
                }
                if (recorded && needsSubmit) recorded = gpuDemand->SubmitTicket(ticket);
                smr->gpuSkinnedThisFrame = recorded;
                smr->gpuSkinningFrame = recorded ? frameStamp : (std::numeric_limits<uint64_t>::max)();
            }
            if (smr->gpuSkinnedThisFrame) reuseCurrentOutput();
            continue;
        }
        if (directSignature == DirectSignatureResult::EQUAL_READY && bufferReads.HasCurrentReadBoundary()) {
            if (!directReuseMarker) {
                profiler::Profiler::PushMarker({"RenderSystem::SkinningPreparedReuse", "Rendering"});
                directReuseMarker = true;
            }
            reuseCurrentOutput();
            continue;
        }
        if (!fullPreparationMarker) {
            profiler::Profiler::PushMarker({"RenderSystem::SkinningFullPreparation", "Rendering"});
            fullPreparationMarker = true;
        }
        const uint64_t signatureAcquireStart=trace?skinCpu.Mark():0;
        skinning_signature_detail::SignatureScratchLease signatureScratch(trace);
        auto& content=signatureScratch.Words();
        if(trace){
            trace->signatureScratchAcquireNs+=skinCpu.Elapsed(signatureAcquireStart);
            ++trace->signatureScratchAcquireCalls;
            ++trace->signatureClockPairCount;
        }
        bool outputsReady=false;
        const auto buildSignature=[&]<bool TRACE_ENABLED>(){
            [[maybe_unused]] const uint64_t signatureBuildStart=TRACE_ENABLED?skinCpu.Mark():0;
            [[maybe_unused]] const uint64_t payloadAllocationsBefore=TRACE_ENABLED?trace->signaturePayloadAllocationCalls:0;
            SignatureWordWriter<TRACE_ENABLED> signatureWords(content,trace,skinCpu);
            if constexpr(TRACE_ENABLED)++trace->signatureVectors;
            signatureWords.ReserveForPose(matrices->size(), smr->SubmeshCount(), anim && anim->boneBake.active && anim->boneBake.source);
            signatureWords.Append(static_cast<uint32_t>(matrices->size()));
            signatureWords.AppendMatrices(*matrices);
            if (anim && anim->boneBake.active && anim->boneBake.source) {
                const auto& bake = anim->boneBake;
                const auto appendHash = [&signatureWords](uint64_t value) {
                    signatureWords.Append(static_cast<uint32_t>(value)); signatureWords.Append(static_cast<uint32_t>(value >> 32));
                };
                appendHash(bake.source->skeletonFingerprint); appendHash(bake.source->clipFingerprint);
                appendHash(bake.target ? bake.target->clipFingerprint : 0);
                signatureWords.Append(static_cast<uint32_t>(bake.generation));
                signatureWords.Append(bake.frame.first); signatureWords.Append(bake.frame.second); signatureWords.Append(std::bit_cast<uint32_t>(bake.frame.alpha));
                signatureWords.Append(bake.targetFrame.first); signatureWords.Append(bake.targetFrame.second);
                signatureWords.Append(std::bit_cast<uint32_t>(bake.targetFrame.alpha)); signatureWords.Append(std::bit_cast<uint32_t>(bake.blendWeight));
                signatureWords.Append(static_cast<uint32_t>(anim->boneBakeMode));
            }
            outputsReady = smr->gpuSkinnedThisFrame;
            for (size_t slot = 0; slot < smr->SubmeshCount(); ++slot) {
                const auto* mesh = smr->SubmeshMesh(slot);
                if (!mesh) { outputsReady = false; continue; }
                const auto source = smr->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer);
                [[maybe_unused]] bool observedMorphPath=false,observedDifferentSource=false;
                if constexpr(TRACE_ENABLED) {
                    /// @note Preserve pre-lookup choice as values; do not dereference earlier scene/model inputs after this lookup's possible wait.
                    const uint32_t index=smr->SubmeshAt(slot);
                    observedMorphPath=index<smr->morphVertexBuffers.size() && smr->morphVertexBuffers[index].IsValid();
                    observedDifferentSource=source!=mesh->vertexBuffer;
                }
                renderer::BufferResourceState sourceState;
                if (useBufferStates) sourceState = bufferStates[slot * 2];
                else if (const auto* buffer = resources.Get(source)) {
                    sourceState.contentVersion = buffer->GetContentVersion();
                    sourceState.live = true;
                }
                const uint64_t version = sourceState.contentVersion;
                if constexpr(TRACE_ENABLED) {
                    ++trace->sourceObservedSlots;
                    if(observedMorphPath)++trace->sourceMorphPathSlots;else ++trace->sourceBasePathSlots;
                    if(observedDifferentSource)++trace->sourceDiffersFromBaseSlots;
                    if(!sourceState.live)++trace->sourceLookupNullSlots;
                    else if(observedMorphPath)++trace->sourceLookupNonNullMorphPathSlots;
                    else ++trace->sourceLookupNonNullBasePathSlots;
                    if(version==0)++trace->sourceNativeVersionZeroSlots;
                }
                signatureWords.Append(smr->SubmeshAt(slot));
                signatureWords.Append(source.id); signatureWords.Append(source.gen);
                signatureWords.Append(static_cast<uint32_t>(version)); signatureWords.Append(static_cast<uint32_t>(version >> 32));
                signatureWords.Append(static_cast<uint32_t>(mesh->cpuSkinnedVertices.size()));
                renderer::BufferResourceState outputState;
                if (useBufferStates) outputState = bufferStates[slot * 2 + 1];
                else if (const auto* output = resources.Get(smr->ResolveSlotSkinnedVertexBuffer(slot)))
                    outputState = {output->GetSize(), output->GetStride(), output->GetContentVersion(), output->GetBindlessUavIndex(), true};
                outputsReady &= outputState.live && outputState.stride == sizeof(renderer::Vertex)
                    && outputState.sizeBytes >= mesh->cpuSkinnedVertices.size() * sizeof(renderer::Vertex)
                    && outputState.contentVersion != 0
                    && outputState.bindlessUavIndex != renderer::INVALID_BINDLESS_INDEX;
            }
            if constexpr(TRACE_ENABLED){
                trace->signatureBuildNs+=skinCpu.Elapsed(signatureBuildStart);
                ++trace->signatureBuildCalls;
                ++trace->signatureClockPairCount;
                trace->signatureBytes+=content.size()*sizeof(uint32_t);
                trace->signatureCapacityBytes+=content.capacity()*sizeof(uint32_t);
                if(!content.empty() && trace->signaturePayloadAllocationCalls>payloadAllocationsBefore)++trace->signatureAllocationLowerBound;
            }
        };
        if(trace)buildSignature.template operator()<true>();
        else buildSignature.template operator()<false>();
        const auto signatureMatches=[&]{
            if(!trace)return smr->gpuSkinningContent==content;
            const uint64_t start=skinCpu.Mark();
            const bool matches=smr->gpuSkinningContent==content;
            trace->signatureCompareNs+=skinCpu.Elapsed(start);
            ++trace->signatureCompareCalls;
            ++trace->signatureClockPairCount;
            return matches;
        };
        if (outputsReady && directSignature != DirectSignatureResult::DIFFERENT && signatureMatches()) {
            reuseCurrentOutput();
            continue;
        }

        renderer::ResourceHandle<renderer::StructuredBufferTag> bonePalette;
        uint32_t boneOffset = 0;
        const EntityID paletteEntity = anim ? animatorEntity : go.GetID();
        if (packed) {
            if (matrices->size() > UINT32_MAX || ctx.skinningMatrices.size() > UINT32_MAX - matrices->size()) {
                invalidatePublication(*smr);
                continue;
            }
            const auto [it, inserted] = packedOffsets.try_emplace(paletteEntity,
                static_cast<uint32_t>(ctx.skinningMatrices.size()));
            boneOffset = it->second;
            if (inserted) ctx.skinningMatrices.insert(ctx.skinningMatrices.end(), matrices->begin(), matrices->end());
        } else {
            const bool needsUpload = !g_framePalettes.contains(paletteEntity);
            bonePalette = AcquireBonePalette(resources, paletteEntity, *matrices, trace);
            const auto* palette = resources.Get(bonePalette);
            if (!palette || palette->GetStride() != sizeof(math::Matrix4) || palette->GetElementCount() == 0
                || palette->GetSize() / sizeof(math::Matrix4) < palette->GetElementCount()) {
                invalidatePublication(*smr);
                continue;
            }
            if (needsUpload) {
                ++ctx.statsSkinningPaletteUploads;
                ctx.statsSkinningPaletteUploadBytes += matrices->size() * sizeof(math::Matrix4);
            }
        }

        bool allSkinned = smr->SubmeshCount() != 0;
        PendingPublication publication;
        publication.owner = go.GetID();
        publication.componentLifetime = ctx.scene.GetComponentLifetime<SkinnedMeshRenderer>(publication.owner);
        publication.sceneGeneration = ctx.scene.GetRenderSceneGeneration();
        publication.resourceOwner = resources.GetInstanceId(); publication.resourceEpoch = resources.GetResetVersion();
        publication.resourceFrame = resources.FrameStamp();
        publication.frame = frameStamp; publication.model = smr->model;
        publication.demandCandidate = gpuDemand && !gpuDemand->cancelled && !ownedBatchRequested && packed
            && !demandRequired;
        /// @note 次の dirty 姿勢は旧 ticket 出力へ書かない。旧 snapshot がその lease を引き続き保持する。
        smr->preparedSkinning.reset();
        smr->preparedSkinningRows.clear();
        smr->preparedSkinningFrame = (std::numeric_limits<uint64_t>::max)();
        if (ownedBatchRequested) {
            publication.modelMeshCount = meshCount; publication.slotCount = smr->SubmeshCount();
            publication.animator = animatorEntity;
            publication.animatorLifetime = anim ? ctx.scene.GetComponentLifetime<AnimatorComponent>(animatorEntity) : 0;
            publication.pose = *matrices; publication.content.assign(content.begin(), content.end());
            if (anim) {
                const auto& bake = anim->boneBake;
                publication.bake = {anim->boneBakeMode, bake.active && bake.source != nullptr, bake.generation, bake.source, bake.target,
                    bake.frame.first, bake.frame.second, std::bit_cast<uint32_t>(bake.frame.alpha),
                    bake.targetFrame.first, bake.targetFrame.second, std::bit_cast<uint32_t>(bake.targetFrame.alpha),
                    std::bit_cast<uint32_t>(bake.blendWeight)};
            }
        }
        uint64_t skinnedVertexCount = 0;
        uint32_t skinningDispatchCount = 0;
        /// @note この Renderer が実際に描く submesh だけをスキニングする。ノードごとに子 GameObject
        /// @note へ分けた構成では同じモデルを参照する Renderer が複数並ぶため、全員がモデル全体を
        /// @note 変形すると子の数だけ同じ計算を繰り返し、負荷が submesh 数倍になる。
        const size_t slotCount = smr->SubmeshCount();
        for (size_t slot = 0; slot < slotCount; ++slot) {
            const uint32_t submeshIndex = smr->SubmeshAt(slot);
            if (submeshIndex == UINT32_MAX || submeshIndex >= meshCount) { allSkinned = false; continue; }
            /// @note 以降の mi は model->meshes / 各バッファ配列と同じ添字。
            const size_t mi = static_cast<size_t>(submeshIndex);
            const auto& meshPtr = smr->model->meshes[mi];
            if (!meshPtr) { allSkinned = false; continue; }
            const auto source = smr->ResolveSlotVertexBuffer(slot, meshPtr->vertexBuffer);
            uint64_t capturedSourceVersion = 0;
            const auto srcVertices = EnsureSourceVertexBuffer(resources, *meshPtr, source, matrices->size(), trace,
                useBufferStates ? &bufferReads : nullptr, publication.demandCandidate ? &capturedSourceVersion : nullptr);
            const uint32_t vertexCount = static_cast<uint32_t>(meshPtr->cpuSkinnedVertices.size());
            const auto* sourceVertices = resources.Get(srcVertices);
            if (vertexCount == 0 || !sourceVertices || sourceVertices->GetStride() != sizeof(renderer::SkinnedVertex)
                || sourceVertices->GetElementCount() < vertexCount
                || sourceVertices->GetSize() / sizeof(renderer::SkinnedVertex) < vertexCount) {
                allSkinned = false;
                continue;
            }

            /// @note ポーズが異なるため出力は renderer 別に所有し、現在の頂点数を収める容量だけを再利用する。
            auto& outBuffer = smr->skinnedVertexBuffers[mi];
            const size_t outputBytes = static_cast<size_t>(vertexCount) * sizeof(renderer::Vertex);
            const auto* output = useBufferStates ? bufferReads.Get(outBuffer) : resources.Get(outBuffer);
            /// @note 同じ model/source handle の topology growth でも旧容量へ書かない。解放は ResourceManager の GPU fence へ委ねる。
            if (output && (output->GetStride() != sizeof(renderer::Vertex) || output->GetSize() < outputBytes
                || output->GetBindlessUavIndex() == renderer::INVALID_BINDLESS_INDEX)) {
                if(trace){++trace->outputReleaseCalls;if(output->GetSize()<outputBytes)++trace->outputCapacityRejects;}
                resources.Release(outBuffer);
                outBuffer = {};
            } else if (!output) outBuffer = {};
            if (!outBuffer.IsValid()) {
                if(trace)++trace->outputCreateAttempts;
                outBuffer = resources.CreateGpuWritableVertexBuffer(
                    outputBytes,
                    static_cast<uint32_t>(sizeof(renderer::Vertex)));
                /// @note バックエンド未対応ならここで無効ハンドルが返る。VS 経路へ落とす。
                if (!outBuffer.IsValid()) { allSkinned = false; break; }
                if(trace)++trace->outputCreateSuccesses;
            }
            output = useBufferStates ? bufferReads.Get(outBuffer) : resources.Get(outBuffer);
            if (!output || output->GetStride() != sizeof(renderer::Vertex)
                || output->GetSize() / sizeof(renderer::Vertex) < vertexCount
                || output->GetBindlessUavIndex() == renderer::INVALID_BINDLESS_INDEX) {
                allSkinned = false;
                continue;
            }

            if (ownedBatchRequested) {
                renderer::BufferResourceMetadata metadata;
                const bool live = resources.GetBufferMetadata(source, metadata);
                publication.slots.push_back({slot, mi, meshPtr.get(), source, outBuffer, vertexCount,
                    live ? metadata.contentVersion : 0, ctx.skinningRequests.size()});
            }
            ctx.skinningRequests.push_back({srcVertices, bonePalette, outBuffer, vertexCount, boneOffset,
                packed ? static_cast<uint32_t>(matrices->size()) : 0u});
            if (publication.demandCandidate) {
                publication.capturedRequestIndices.push_back(ctx.skinningRequests.size() - 1u);
                publication.capturedRequests.push_back({source, {}, capturedSourceVersion, vertexCount,
                    boneOffset, static_cast<uint32_t>(matrices->size())});
                publication.slots.push_back({slot, mi, meshPtr.get(), source, outBuffer, vertexCount, 0, 0});
            }
            if(trace){++trace->requestedDispatches;trace->requestedVertexCount+=vertexCount;}
            if (packed && anim && anim->boneBake.active && anim->boneBakeMode >= static_cast<int>(BoneBakeMode::GPU_SHARED)) {
                bakeRequests.push_back({static_cast<uint32_t>(ctx.skinningRequests.size() - 1u), &anim->boneBake, smr->model, animatorEntity, ctx.scene.GetComponentLifetime<AnimatorComponent>(animatorEntity)});
                if(trace)++trace->boneBakeQueuedReplayRows;
            }
            publication.outputs.emplace_back(outBuffer, output->GetContentVersion());

            skinnedVertexCount += vertexCount;
            ++skinningDispatchCount;
        }

        if(trace && skinningDispatchCount!=0)++trace->renderersWithOutputRequests;
        /// @note Owned work publishes only after native writes and the current owner witness are confirmed.
        smr->gpuSkinnedThisFrame = !ownedBatchRequested && allSkinned;
        if(allSkinned){
            if(!trace)smr->gpuSkinningContent.assign(content.begin(),content.end());
            else{
                const size_t capacityBefore=smr->gpuSkinningContent.capacity();
                const uint64_t start=skinCpu.Mark();
                smr->gpuSkinningContent.assign(content.begin(),content.end());
                trace->signatureAssignNs+=skinCpu.Elapsed(start);
                ++trace->signatureAssignCalls;
                ++trace->signatureClockPairCount;
                trace->signatureAssignCopyLogicalBytes+=content.size()*sizeof(uint32_t);
                if(smr->gpuSkinningContent.capacity()!=capacityBefore){
                    ++trace->signatureDestinationPayloadAllocationCalls;
                    trace->signatureDestinationPayloadAllocationBytes+=smr->gpuSkinningContent.capacity()*sizeof(uint32_t);
                }
            }
        }else{
            const uint64_t start=trace?skinCpu.Mark():0;
            smr->gpuSkinningContent.clear();
            if(trace){
                trace->signatureClearNs+=skinCpu.Elapsed(start);
                ++trace->signatureClearCalls;
                ++trace->signatureClockPairCount;
            }
        }
        if (ownedBatchRequested || publication.demandCandidate) {
            publication.vertices = skinnedVertexCount; publication.dispatches = skinningDispatchCount;
        }
        if (allSkinned) pendingPublications.push_back(std::move(publication));
        smr->gpuSkinningFrame = ownedBatchRequested ? (std::numeric_limits<uint64_t>::max)() : frameStamp;
        smr->gpuSkinningVertexCount = ownedBatchRequested ? 0 : skinnedVertexCount;
        smr->gpuSkinningDispatchCount = ownedBatchRequested ? 0 : skinningDispatchCount;


    }

    /// @note Prepare は元 dispatch 線形化点より前。途中の外部 callback は全省略を取り消す。
    if (gpuDemand && !gpuDemand->cancelled) {
        std::vector<renderer::CapturedSkinningRequest> captured;
        std::vector<math::Matrix4> capturedMatrices;
        std::unordered_map<uint64_t, uint32_t> capturedOffsets;
        for (const auto& publication : pendingPublications)
            if (publication.demandCandidate) for (auto request : publication.capturedRequests) {
                if (static_cast<uint64_t>(request.boneOffset) + request.boneCount > ctx.skinningMatrices.size()) {
                    gpuDemand->cancelled = true;
                    break;
                }
                const uint64_t key = (static_cast<uint64_t>(request.boneOffset) << 32) | request.boneCount;
                const auto [offset, inserted] = capturedOffsets.try_emplace(key, static_cast<uint32_t>(capturedMatrices.size()));
                if (inserted) capturedMatrices.insert(capturedMatrices.end(), ctx.skinningMatrices.begin() + request.boneOffset,
                    ctx.skinningMatrices.begin() + request.boneOffset + request.boneCount);
                request.boneOffset = offset->second;
                captured.push_back(request);
            }
        if (!captured.empty() && !gpuDemand->cancelled) {
            auto ticket = renderer::PrepareCapturedSkinning(ctx.renderer, resources, h.skinningComputeCS, captured, capturedMatrices);
            bool current = ticket && ticket->Writes().size() == captured.size() && !gpuDemand->cancelled;
            for (const auto& publication : pendingPublications) {
                if (!publication.demandCandidate) continue;
                const auto* component = ctx.scene.GetComponent<SkinnedMeshRenderer>(publication.owner);
                current = current && component && ctx.scene.GetRenderSceneGeneration() == publication.sceneGeneration
                    && ctx.scene.GetComponentLifetime<SkinnedMeshRenderer>(publication.owner) == publication.componentLifetime
                    && component->model == publication.model && resources.GetInstanceId() == publication.resourceOwner
                    && resources.GetResetVersion() == publication.resourceEpoch && resources.FrameStamp() == publication.resourceFrame;
            }
            if (current) {
                ctx.statsSkinningDemandPrepared += captured.size();
                ctx.statsSkinningDemandSkipped += captured.size();
                std::vector<bool> deferredRequests(ctx.skinningRequests.size(), false);
                uint32_t row = 0;
                for (auto& publication : pendingPublications) {
                    if (!publication.demandCandidate) continue;
                    auto* component = ctx.scene.GetComponent<SkinnedMeshRenderer>(publication.owner);
                    component->preparedSkinning = ticket;
                    component->preparedSkinningFrame = publication.frame;
                    component->preparedSkinningRows.assign(component->model->meshes.size(), UINT32_MAX);
                    for (size_t slot = 0; slot < publication.slots.size(); ++slot) {
                        component->preparedSkinningRows[publication.slots[slot].submesh] = row++;
                        deferredRequests[publication.capturedRequestIndices[slot]] = true;
                    }
                    component->gpuSkinnedThisFrame = false;
                    component->gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
                    component->gpuSkinningVertexCount = 0; component->gpuSkinningDispatchCount = 0;
                    gpuDemand->publications.push_back({publication.owner, publication.componentLifetime, publication.sceneGeneration,
                        publication.model, ticket, publication.vertices, publication.dispatches});
                    publication.deferred = true;
                }
                size_t destination = 0;
                for (size_t index = 0; index < ctx.skinningRequests.size(); ++index) {
                    if (deferredRequests[index]) continue;
                    for (auto& bake : bakeRequests)
                        if (bake.requestIndex == index) bake.requestIndex = static_cast<uint32_t>(destination);
                    ctx.skinningRequests[destination++] = ctx.skinningRequests[index];
                }
                ctx.skinningRequests.resize(destination);
            }
        }
        gpuDemand->committed = true;
    }

    {
        FBZZ_PROFILE_SCOPE("RenderSystem::SkinningDispatch");
        /// @note Skinning precedes RenderGraph; a separate preparation view records its actual GPU commands without claiming whole-frame GPU time.
        renderer::GpuProfilerViewMetadata skinView;
        skinView.applicationFrameSerial = resources.FrameStamp();
        skinView.sceneGeneration = ctx.scene.GetRenderSceneGeneration();
        skinView.resourceEpoch = resources.GetResetVersion();
        skinView.width = ctx.renderer.GetWidth(); skinView.height = ctx.renderer.GetHeight();
        const uint64_t profilerSyncStart = skinCpu.Mark();
        const bool skinProfile = ctx.renderer.GpuProfRecordView(skinView);
        skinCpu.row.profilerSyncNs += skinCpu.Elapsed(profilerSyncStart);
        const bool splitTrace = skinProfile && AnimationGpuTraceRequested();
        if (skinProfile && !splitTrace) ctx.renderer.GpuProfBeginPass("AnimationSkinningCompute");
        if (splitTrace && !bakeRequests.empty()) ctx.renderer.GpuProfBeginPass("BakedBoneReplay");
        const uint64_t replayCpuStart = skinCpu.Mark();
        PrepareBakedSkinningRequests(ctx, bakeRequests);
        skinCpu.row.replaySubmitNs += skinCpu.Elapsed(replayCpuStart);
        if (splitTrace && !bakeRequests.empty()) ctx.renderer.GpuProfEndPass("BakedBoneReplay");
        if (splitTrace) ctx.renderer.GpuProfBeginPass("VertexSkinningCompute");
        const uint64_t vertexCpuStart = skinCpu.Mark();
        renderer::ExecuteSkinningRequests(ctx);
        skinCpu.row.vertexSubmitNs += skinCpu.Elapsed(vertexCpuStart);
        if (splitTrace) ctx.renderer.GpuProfEndPass("VertexSkinningCompute");
        if (skinProfile) {
            if (!splitTrace) ctx.renderer.GpuProfEndPass("AnimationSkinningCompute");
            ctx.renderer.GpuProfEndRecordedView();
        }
    }
    g_frameStats.dispatches += ctx.statsSkinningDispatchCount;
    g_frameStats.vertices += ctx.statsSkinningVertexCount;
    g_frameStats.uploads += ctx.statsSkinningPaletteUploads;
    g_frameStats.bytes += ctx.statsSkinningPaletteUploadBytes;
    /// @note Every view reports actual preparation accumulated in this engine frame, including earlier reused views.
    ctx.statsSkinningDispatchCount = g_frameStats.dispatches;
    ctx.statsSkinningVertexCount = g_frameStats.vertices;
    ctx.statsSkinningPaletteUploads = g_frameStats.uploads;
    ctx.statsSkinningPaletteUploadBytes = g_frameStats.bytes;
    /// @note Dispatch is a void API; only the backend's successful DIRECT write publication advances this version.
    if (ownedBatchRequested) {
        /// @note Scene/ResourceManager remain caller-owned for this synchronous pass; re-resolve every actor after Win32 message reentry before dereferencing a saved component.
        for (const auto& publication : pendingPublications) {
            if (publication.sceneGeneration != ctx.scene.GetRenderSceneGeneration() || publication.frame != Time::frameCount
                || publication.resourceOwner != resources.GetInstanceId() || publication.resourceEpoch != resources.GetResetVersion()
                || publication.resourceFrame != resources.FrameStamp()) continue;
            auto* component = ctx.scene.GetComponent<SkinnedMeshRenderer>(publication.owner);
            auto* object = ctx.scene.GetGameObject(publication.owner);
            if (!component || !object || ctx.scene.GetComponentLifetime<SkinnedMeshRenderer>(publication.owner) != publication.componentLifetime) continue;
            /// @note A nested pass may have published a newer pose; the outer call never invalidates that different publication.
            if (component->gpuSkinningContent != publication.content) continue;
            bool current = component->enabled && object->activeInHierarchy() && component->model == publication.model
                && component->model && component->model->meshes.size() == publication.modelMeshCount
                && component->SubmeshCount() == publication.slotCount;
            AnimatorComponent* animator = nullptr;
            EntityID animatorOwner = EntityID::INVALID;
            for (auto* ancestor = object; ancestor; ancestor = ancestor->GetParent())
                if ((animator = ancestor->GetComponent<AnimatorComponent>())) { animatorOwner = ancestor->GetID(); break; }
            current &= animatorOwner == publication.animator
                && (!animator || ctx.scene.GetComponentLifetime<AnimatorComponent>(animatorOwner) == publication.animatorLifetime);
            const auto* currentPose = animator && !animator->boneMatrices.empty() ? &animator->boneMatrices
                : component->model && component->model->skeleton && !component->model->skeleton->referencePose.empty()
                    ? &component->model->skeleton->referencePose : nullptr;
            current &= currentPose && currentPose->size() == publication.pose.size();
            if (current) current = std::memcmp(currentPose->data(), publication.pose.data(), publication.pose.size() * sizeof(math::Matrix4)) == 0;
            if (animator) {
                const auto& bake = animator->boneBake;
                const auto& witness = publication.bake;
                const bool bakedSignature = bake.active && bake.source != nullptr;
                current &= bakedSignature == witness.active;
                if (bakedSignature) current &= animator->boneBakeMode == witness.mode && bake.generation == witness.generation
                    && bake.source == witness.source && bake.target == witness.target
                    && bake.frame.first == witness.first && bake.frame.second == witness.second && std::bit_cast<uint32_t>(bake.frame.alpha) == witness.alpha
                    && bake.targetFrame.first == witness.targetFirst && bake.targetFrame.second == witness.targetSecond
                    && std::bit_cast<uint32_t>(bake.targetFrame.alpha) == witness.targetAlpha && std::bit_cast<uint32_t>(bake.blendWeight) == witness.blend;
            }
            const auto& receipt = ctx.skinningBatchReceipt;
            current &= receipt.ownedPath && receipt.resourceInstance == publication.resourceOwner && receipt.resourceEpoch == publication.resourceEpoch
                && receipt.resourceFrame == publication.resourceFrame;
            for (const auto& slot : publication.slots) {
                if (!current) break;
                const auto* mesh = component->SubmeshMesh(slot.slot);
                renderer::BufferResourceMetadata metadata;
                /// @note Accepted CPU metadata is only a change witness; successful native writes are proven exclusively by the worker receipt.
                current = component->SubmeshAt(slot.slot) == slot.submesh && mesh == slot.mesh && mesh
                    && mesh->cpuSkinnedVertices.size() == slot.vertices
                    && component->ResolveSlotVertexBuffer(slot.slot, mesh->vertexBuffer) == slot.source
                    && slot.submesh < component->skinnedVertexBuffers.size() && component->skinnedVertexBuffers[slot.submesh] == slot.output
                    && resources.GetBufferMetadata(slot.source, metadata) && metadata.contentVersion == slot.acceptedSourceVersion
                    && slot.acceptedSourceVersion != 0 && slot.requestIndex < receipt.writes.size();
                if (current) {
                    const auto& write = receipt.writes[slot.requestIndex];
                    renderer::BufferResourceMetadata outputMetadata;
                    current = write.output == slot.output && write.recorded && write.resultingVersion > write.previousVersion
                        && resources.GetBufferMetadata(slot.output, outputMetadata) && outputMetadata.gpuWritable
                        && outputMetadata.stride == sizeof(renderer::Vertex) && outputMetadata.sizeBytes / sizeof(renderer::Vertex) >= slot.vertices;
                }
            }
            if (!current) invalidatePublication(*component);
            else {
                component->gpuSkinnedThisFrame = true;
                component->gpuSkinningFrame = publication.frame;
                component->gpuSkinningVertexCount = publication.vertices;
                component->gpuSkinningDispatchCount = publication.dispatches;
                if (trace) ++trace->nativePublishedRenderers;
            }
        }
    }

    for (const auto& publication : pendingPublications) {
        if (ownedBatchRequested) continue;
        if (publication.deferred) continue;
        bool allWritten = true;
        for (const auto& [handle, previousVersion] : publication.outputs) {
            const auto* output = resources.Get(handle);
            if (!output || output->GetContentVersion() <= previousVersion) {
                allWritten = false;
                break;
            }
        }
        /// @note Buffer completion may pump edits made while another actor was prepared; never dereference a previous packed component address.
        auto* component = ctx.scene.GetComponent<SkinnedMeshRenderer>(publication.owner);
        if (!component || ctx.scene.GetRenderSceneGeneration() != publication.sceneGeneration
            || ctx.scene.GetComponentLifetime<SkinnedMeshRenderer>(publication.owner) != publication.componentLifetime
            || component->model != publication.model) continue;
        allWritten &= publication.resourceOwner == resources.GetInstanceId() && publication.resourceEpoch == resources.GetResetVersion()
            && publication.resourceFrame == resources.FrameStamp() && publication.frame == Time::frameCount;
        if (!allWritten) invalidatePublication(*component);
        if(trace && component->gpuSkinnedThisFrame)++trace->nativePublishedRenderers;
    }
}

/// @brief ReleaseSkinningComputeCaches — シーン切り替え / リソースリセット時にキャッシュを捨てる。
/// @note キャッシュの GPU ハンドルはデバイス世代に属するため、デバイスリセット後に古い世代の
/// @note ハンドルを再利用しない。通常のシーン上の破棄は Execute 側で回収する。
void ReleaseSkinningComputeCaches()
{
    ReleaseBoneBakeSkinningCaches();
    ReleaseSourceSkinningCache();
}


} /// @note namespace fbzz::scene
