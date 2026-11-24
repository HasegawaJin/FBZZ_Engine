/// @file    SceneSkinningComputePass.cpp
/// @brief   コンピュートスキニング — ボーン変形を 1 フレームに 1 回だけ計算して共有する。
/// @author  Hasegawa Jin
/// @date    2026-08-14

/// @note 静的頂点と同じレイアウトを共有し、入力や GPU 書き込みが未対応なら頂点シェーダーへ戻す。
#include "GeometryPasses.hpp"
#include "BoneBakeSkinning.hpp"
#include "SkinningSignatureScratch.hpp"
#include "SkinningOutputDemand.hpp"
#include "SkinningPreparedOutput.hpp"
#include "SkinningComputeCache.hpp"
#include <Graphics/Renderer/Geometry/SkinningPolicy.hpp>
#include <Graphics/Renderer/Resources/Buffers/BufferReadScope.hpp>
#include <Engine/Scene/Animation/BoneBakeRuntime.hpp>
#include <Graphics/Effects/Animation/PreparedSkinning.hpp>
#include "../../../Scene/Systems/Animation/AnimationWorkTrace.hpp"
#include <Engine/Rendering/Extraction/RenderSceneExtractor.hpp>
#include "Engine/Core/Timing/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Hierarchy/Transform.hpp"
#include "Graphics/Renderer/Geometry/Mesh.hpp"
#include "Graphics/Renderer/Commands/ComputeCall.hpp"
#include "Graphics/Renderer/Resources/Buffers/IBuffer.hpp"
#include <Graphics/Renderer/Resources/Buffers/IStructuredBuffer.hpp>
#include "Graphics/Renderer/Resources/ResourceManager.hpp"
#include <Graphics/RayTracing/RayTracingPipeline.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Core/Profiler/Profiler.hpp>
#include "Engine/Scene/Components/Geometry/MaterialComponent.hpp"
#include "Engine/Scene/Components/Animation/AnimatorComponent.hpp"
#include "Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp"
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <cstdint>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

/// @note CS のスレッドグループ幅。SkinningCompute.cs.hlsl の SKINNING_GROUP_SIZE と一致させること。
constexpr uint32_t kSkinningGroupSize = 64u;

/// @note StructuredBuffer は定数バッファのように整列しないため、HLSL の頂点レイアウトとサイズを合わせる。
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

/// @note 後から登録・削除したコンポーネントは、Scene の所有順と格納順が異なることがある。
/// @note 入れ子のパスは別々の ID バッファを借り、返却後は容量だけを保持する。
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

/// @note パス終了時に現在のスレッドの保持容量を計測し、容量と要求件数の計測を締めてから行データを公開する。
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

/// @note 両経路は行優先の浮動小数点ビット列を保持し、計測には予約または追加による実際の容量増加だけを含める。
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

/// @note 通常の非ベイク経路と同じ入力値を照合し、GPU 資源の照会・状態の保持・結果の公開は行わない。
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

}

void ExecuteSkinningComputePass(RenderPassContext& ctx)
{
    FBZZ_PROFILE_SCOPE("RenderSystem::SkinningCompute");
    AnimationSkinningCpuFrame skinCpu(ctx.resources.FrameStamp(),ctx.scene.GetRenderSceneGeneration(),ctx.resources.GetResetVersion(),ctx.resources.GetInstanceId());
    auto* trace=skinCpu.TraceRow();
    skinning_demand_detail::SceneDemandSnapshot demand;
    if(trace) {
        /// @note 開始時のスナップショットは診断専用であり、省略・公開・資源寿命の判定には使わない。
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
    auto* gpuDemand = skinning_demand_detail::FindGpuOutputDemand(ctx);
    bool directReuseMarker = false, fullPreparationMarker = false;

    const bool ownedBatchRequested = renderer::UseOwnedSkinningBatch();
    ctx.statsSkinningDemandMode = renderer::GpuSkinningDemandEnabled() ? 1 : 0;
    ctx.skinningRequests.clear();
    ctx.skinningMatrices.clear();
    std::unordered_map<EntityID, uint32_t, skinning_cache_detail::EntityIDHash> packedOffsets;
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
        /// @note 画面外の個体は CPU パレットを省き、再生に失敗した直後に可視になった個体は描画前に復旧する。
        animator->boneBake.gpuReady = replayAvailable;
        animator->boneBake.gpuOwner = resources.GetInstanceId(); animator->boneBake.gpuEpoch = resources.GetResetVersion();
        if (!replayAvailable && animator->boneBake.cpuDeferred) restoreCpuActors.push_back(id);
    }
    /// @note 復元で描画実行器へ入る場合に備え、コンポーネント配列の参照をまたがせず ID を保持する。
    for (EntityID id : restoreCpuActors)
        if (auto* animator = ctx.scene.GetComponent<AnimatorComponent>(id)) MaterializeBoneBakeCpu(*animator, resources, true);
    if (!skinningProgramReady) {
        /// @note 型付きハンドルが非 null でも失効し得る。プログラムや CB がなければ、古い姿勢を現在の変形として公開しない。
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

    skinning_cache_detail::BeginSkinningCacheFrame(resources, Time::frameCount, trace);
    /// @note EntityID は Scene 内だけで一意。別 Scene の同番号の Animator とパレットを共有しない。
    /// @note 計算済み Renderer は gpuSkinningFrame で再利用するため、既存出力を再 Dispatch しない。
    skinning_cache_detail::ResetSkinningPalettes();
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
        /// @note 待機中にシーン編集が発生し得るため、コンポーネント配列を再取得して待機前の識別情報を検証する。
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
        /// @note コンポーネントやモデルの寿命が変わらなくても、待機中に出力スロットが削除される場合がある。
        if (smr->skinnedVertexBuffers.size() < meshCount) smr->skinnedVertexBuffers.resize(meshCount);
        bool useBufferStates = smr->model == readModel && smr->SubmeshCount() == readSlotCount;
        for (size_t slot = 0; useBufferStates && slot < readSlotCount; ++slot) {
            const auto* mesh = smr->SubmeshMesh(slot);
            useBufferStates = (mesh ? smr->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer)
                                   : renderer::ResourceHandle<renderer::BufferTag>{}) == bufferHandles[slot * 2]
                && (mesh ? smr->ResolveSlotSkinnedVertexBuffer(slot)
                         : renderer::ResourceHandle<renderer::BufferTag>{}) == bufferHandles[slot * 2 + 1];
        }
        /// @note 待機でポーズ配列の位置が変わり得るため、GPU 資源の取得後に現在の所有者を解決する。
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
                skinning_cache_detail::RetainSkinningSource(*meshPtr, source, version);
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
                    /// @note GPU 資源の取得が待機に入る場合に備え、取得前の選択は値で保持し、以前のシーン・モデル参照を再利用しない。
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
            const bool needsUpload = !skinning_cache_detail::HasSkinningPalette(paletteEntity);
            bonePalette = skinning_cache_detail::AcquireBonePalette(resources, paletteEntity, *matrices, trace);
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
            const auto srcVertices = skinning_cache_detail::EnsureSourceVertexBuffer(resources, *meshPtr, source, matrices->size(), trace,
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
        /// @note GPU 書き込みと現在の所有者の一致を確認してから、所有済みの作業結果を公開する。
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
        /// @note グラフに先行するスキニングには独立した計測ビューを使い、発行した GPU 命令の時間だけを記録する。
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
    skinning_cache_detail::AccumulateSkinningFrameStats(ctx);
    /// @note 戻り値のない Dispatch では成否を判断せず、バックエンドが DIRECT キューの書き込みを公開したときだけ版を進める。
    if (ownedBatchRequested) {
        /// @note 同期パス中もシーンと資源管理者は呼び出し側が所有する。Win32 メッセージによる再入後にコンポーネントを解決し直す。
        for (const auto& publication : pendingPublications) {
            if (publication.sceneGeneration != ctx.scene.GetRenderSceneGeneration() || publication.frame != Time::frameCount
                || publication.resourceOwner != resources.GetInstanceId() || publication.resourceEpoch != resources.GetResetVersion()
                || publication.resourceFrame != resources.FrameStamp()) continue;
            auto* component = ctx.scene.GetComponent<SkinnedMeshRenderer>(publication.owner);
            auto* object = ctx.scene.GetGameObject(publication.owner);
            if (!component || !object || ctx.scene.GetComponentLifetime<SkinnedMeshRenderer>(publication.owner) != publication.componentLifetime) continue;
            /// @note 入れ子のパスが新しいポーズを公開していたら、外側のパスはその公開状態を失効させない。
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
                /// @note 受理した CPU メタデータは変化の手掛かりに限り、GPU 書き込みの成功は実行側の受領記録で確認する。
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
        /// @note 他のアクターの準備中の編集がバッファ待機で実行され得るため、以前のコンポーネントのアドレスを使わない。
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

}
