/// @file    AnimationWorkTrace.hpp
/// @brief   Opt-in independent animation work timings and completed GPU timestamp capture.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Api.hpp>
#include <string>
#include <memory>
#include <array>
#include <Engine/Scene/Systems/SkinningDemandTypes.hpp>
#include <Graphics/Renderer/SkinningTrace.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <Math/Matrix4.hpp>
#include <chrono>
#include <cstdint>

namespace fbzz::renderer { struct GpuProfilerSnapshot; }
namespace fbzz::scene {
namespace animation_actor_detail {
inline constexpr uint32_t HISTORY_CAPACITY = 8192, ACTOR_CAPACITY = 262144, FRAME_CAPACITY = 240;
static_assert(sizeof(math::Matrix4)==64);
inline constexpr uint32_t NO_HISTORY = UINT32_MAX;
inline bool TryClaimCapture(std::atomic_flag& ownership) { return !ownership.test_and_set(std::memory_order_acquire); }
inline void ReleaseCapture(std::atomic_flag& ownership) { ownership.clear(std::memory_order_release); }
enum DueReason : uint32_t { ZERO_INTERVAL=1, SKELETON_CHANGED=2, EMPTY_PALETTE=4, ELAPSED_THRESHOLD=8 };
enum ForceReason : uint32_t { STATE_CHANGED=1, BLENDING=2, NON_LEGACY_MODE=4, BAKED_ACTIVE=8 };
/// @note Scalar identity only; no object, component, resource, vector or callback pointer is retained.
struct ActorKey {
    uint64_t sceneGeneration=0,resourceEpoch=0,resourceInstance=0,componentLifetime=0;
    uint32_t entityIndex=0,entityGeneration=0,previousHandleId=0,previousHandleGeneration=0;
    bool operator==(const ActorKey&) const = default;
};
inline bool SameActorLifetime(const ActorKey& a,const ActorKey& b)
{
    return a.sceneGeneration==b.sceneGeneration && a.resourceEpoch==b.resourceEpoch && a.resourceInstance==b.resourceInstance
        && a.componentLifetime==b.componentLifetime && a.entityIndex==b.entityIndex && a.entityGeneration==b.entityGeneration;
}
struct PaletteEquality {
    bool equal=false;
    uint64_t compareArgumentBytes=0;
    uint32_t currentClampedCount=0,previousClampedCount=0;
};
/// @note Compare the effective 128-matrix packets: raw prefix bits and identity-padded unequal-count tails.
inline PaletteEquality CompareEffectivePalettes(std::span<const math::Matrix4> current,
                                                 std::span<const math::Matrix4> previous)
{
    PaletteEquality result;
    result.currentClampedCount=static_cast<uint32_t>((std::min)(current.size(),size_t{128}));
    result.previousClampedCount=static_cast<uint32_t>((std::min)(previous.size(),size_t{128}));
    const uint32_t common=(std::min)(result.currentClampedCount,result.previousClampedCount);
    if(common) {
        const size_t bytes=common*sizeof(math::Matrix4); result.compareArgumentBytes+=bytes;
        if(std::memcmp(current.data(),previous.data(),bytes)!=0)return result;
    }
    const auto identity=math::Matrix4::Identity();
    const auto extra=result.currentClampedCount>common?current:previous;
    const uint32_t end=(std::max)(result.currentClampedCount,result.previousClampedCount);
    for(uint32_t index=common;index<end;++index) {
        result.compareArgumentBytes+=sizeof(math::Matrix4);
        if(std::memcmp(&extra[index],&identity,sizeof(identity))!=0)return result;
    }
    result.equal=true; return result;
}
struct ActorRow {
    ActorKey key;
    uint64_t resourceFrame=0,timeFrame=0,updateOrdinal=0;
    float ctxDt=0,timeDelta=0,unscaledDelta=0,effectiveScale=0,timeScale=0,vfxScale=0;
    float localScale=0,clampedLocalScale=0,actorDt=0,effectiveInterval=0;
    float elapsedBeforeAdd=0,elapsedAtDecision=0,elapsedAfterRun=0,nearestCameraDistanceSquared=0;
    uint64_t currentCountBeforeSnapshot=0,previousCountBeforeSnapshot=0,compareArgumentBytes=0;
    uint32_t currentClampedCount=0,previousClampedCount=0,dueReasons=0,forceReasons=0;
    uint32_t historyIndex=NO_HISTORY,heldSuffixLowerBound=0,selectedLodBand=0,posePathStatus=0;
    bool snapshotObserved=false,snapshotGuardPassed=false,previousValidBeforeSnapshot=false;
    bool desiredEqualityAvailable=false,desiredEqual=false,previousHistoryInvalidatedAfterSnapshot=false,lodBandAvailable=false;
    bool lodDecisionAvailable=false,safeLod=false,dueArgument=false;
    bool actualDecisionAvailable=false,actualRefresh=false,forceReasonsAvailable=false;
    bool afterRunObserved=false,afterRunIdentityValid=false,frameEndIdentityValid=false,sourceComplete=false,duplicateActor=false;
    bool previousObservationContinuous=false,previousHeldAvailable=false,previousHeld=false;
    bool heldStreakExactAvailable=false,consecutiveHeldAtLeast2Available=false,consecutiveHeldAtLeast2=false;
    bool cpuSameDesiredPotentialAvailable=false,cpuSameDesiredPotential=false;
    bool Complete() const { return key.componentLifetime!=0 && afterRunObserved && afterRunIdentityValid && frameEndIdentityValid && sourceComplete && !duplicateActor; }
};
/// @note Held suffixes become exact only after an observed refresh; gaps and invalid rows cut continuity.
struct HeldHistory {
    ActorKey lastKey;
    uint64_t lastResourceFrame=0,lastTimeFrame=0,lastOrdinal=0;
    uint32_t heldSuffix=0;
    bool valid=false,lastHeld=false,exact=false,lastSnapshot=false;
    void Observe(ActorRow& row)
    {
        const bool continuous=valid && row.Complete() && row.actualDecisionAvailable && lastKey==row.key && !row.previousHistoryInvalidatedAfterSnapshot && lastResourceFrame!=UINT64_MAX && lastTimeFrame!=UINT64_MAX && lastOrdinal!=UINT64_MAX
            && row.resourceFrame==lastResourceFrame+1 && row.timeFrame==lastTimeFrame+1 && row.updateOrdinal==lastOrdinal+1;
        row.previousObservationContinuous=continuous;
        row.previousHeldAvailable=continuous; row.previousHeld=continuous && lastHeld;
        row.consecutiveHeldAtLeast2Available=continuous && row.actualDecisionAvailable && row.Complete();
        row.consecutiveHeldAtLeast2=row.consecutiveHeldAtLeast2Available && lastHeld && !row.actualRefresh;
        row.cpuSameDesiredPotentialAvailable=continuous && lastSnapshot && row.Complete() && row.desiredEqualityAvailable && !row.previousHistoryInvalidatedAfterSnapshot;
        row.cpuSameDesiredPotential=row.cpuSameDesiredPotentialAvailable && row.desiredEqual;
        if(!row.Complete() || !row.actualDecisionAvailable) { valid=false; exact=false; heldSuffix=0; return; }
        if(row.actualRefresh) { heldSuffix=0; exact=true; }
        else { heldSuffix=continuous?heldSuffix+1:1; exact=continuous && exact; }
        row.heldSuffixLowerBound=heldSuffix; row.heldStreakExactAvailable=exact;
        lastKey=row.key; lastResourceFrame=row.resourceFrame; lastTimeFrame=row.timeFrame; lastOrdinal=row.updateOrdinal;
        lastHeld=!row.actualRefresh; lastSnapshot=row.snapshotObserved && row.snapshotGuardPassed && !row.previousHistoryInvalidatedAfterSnapshot;
        valid=true;
    }
};
/// @note Shared by the production bounded collector and deterministic capacity tests.
inline bool ClaimBoundedSlot(uint32_t& count,uint32_t capacity,uint32_t& index)
{
    if(count>=capacity)return false; index=count++; return true;
}
}
class Scene;
namespace animation_actor_detail { struct ActorCapture; }
/// @note Separate opt-in scalar collector; its OFF path reads no clock and creates no capture storage.
FBZZ_ENGINE_API bool AnimationActorTraceRequested();
FBZZ_ENGINE_API animation_actor_detail::ActorRow* CurrentAnimationActorRow();
class AnimationActorScope {
public:
    FBZZ_ENGINE_API explicit AnimationActorScope(animation_actor_detail::ActorRow* row);
    FBZZ_ENGINE_API ~AnimationActorScope();
    AnimationActorScope(const AnimationActorScope&)=delete;
    AnimationActorScope& operator=(const AnimationActorScope&)=delete;
private:
    animation_actor_detail::ActorRow* m_previous=nullptr;
    bool m_active=false;
};
class AnimationActorFrame {
public:
    FBZZ_ENGINE_API AnimationActorFrame(bool simulating,uint64_t frame,uint64_t generation,uint64_t epoch,uint64_t instance);
    FBZZ_ENGINE_API ~AnimationActorFrame();
    bool Active() const { return m_capture!=nullptr; }
    FBZZ_ENGINE_API void SetPopulation(uint64_t population);
    FBZZ_ENGINE_API void CountExcluded(uint32_t reason);
    FBZZ_ENGINE_API animation_actor_detail::ActorRow* BeginActor(uint32_t entityIndex,uint32_t entityGeneration,uint64_t lifetime,float ctxDt,float localScale);
    /// @note Reacquires the component after pose/effect callbacks; no actor reference is stored for later use.
    FBZZ_ENGINE_API void FinishActor(animation_actor_detail::ActorRow* row,Scene& scene,uint64_t frame,uint64_t generation,uint64_t epoch,uint64_t instance);
    /// @note Called after compatibilityBatch flush, before destruction; all pending component lifetimes are revalidated.
    FBZZ_ENGINE_API void FinalizeSource(Scene& scene,uint64_t frame,uint64_t generation,uint64_t epoch,uint64_t instance);
    AnimationActorFrame(const AnimationActorFrame&)=delete;
    AnimationActorFrame& operator=(const AnimationActorFrame&)=delete;
private:
    animation_actor_detail::ActorCapture* m_capture=nullptr;
};

enum class AnimationWorkPart : uint32_t {
    TOTAL, PREVIOUS_CB, CURRENT_CB, STATE_MACHINE, CLIP_SELECTION, BONE_HIERARCHY,
    LEGACY_FK, BONE_APPLICATION, PROPAGATION_REBUILD, BAKED_FK, SIDE_EFFECTS_BOUNDS,
    BAKE_PREPARATION, BATCH_SUBMIT, COUNT
};
struct AnimationWorkRow {
    uint64_t resourceFrame = 0, sceneGeneration = 0, resourceEpoch = 0;
    std::array<uint64_t, static_cast<uint32_t>(AnimationWorkPart::COUNT)> nanoseconds{};
    uint64_t animators = 0, boneMatrices = 0, cbUpdates = 0, cbApiBytes = 0;
    uint64_t legacyPoses = 0, bakedCpuPoses = 0, bakedGpuPoses = 0, poseCacheHits = 0, poseCacheMisses = 0;
    uint64_t cbBulkPackets = 0;
    uint64_t wallBeginIntervalNs = 0;
    /// @note Appended observation schema; counters are actor updates/calls/writes, never unique population or GPU bytes.
    static constexpr uint32_t POSE_OBSERVATION_SCHEMA = 1;
    bool poseCountersAvailable = false, legacyBoneWritesAvailable = false, paletteApiCountersAvailable = false;
    uint64_t beginResourceFrame = 0, beginSceneGeneration = 0, beginResourceEpoch = 0;
    uint64_t endResourceFrame = 0, endSceneGeneration = 0, endResourceEpoch = 0;
    bool sourceIdentityFinalized = false, sourceIdentityMismatch = false;
    uint64_t posePathActors = 0, poseLodGatePassed = 0, poseReducedIntervalActors = 0;
    /// @note Refresh entry includes bind/error/external-pose fallbacks; it is not successful clip evaluation.
    uint64_t poseRefreshEntered = 0, poseHeld = 0, poseLegacyFkCalls = 0, poseBakedSuccesses = 0;
    uint64_t poseBindFallbackActors = 0, poseOtherRefreshActors = 0, legacyEvaluatedBoneWrites = 0;
    uint64_t previousPaletteUpdateCalls = 0, previousPaletteApiBytes = 0;
    uint64_t currentPaletteUpdateCalls = 0, currentPaletteApiBytes = 0;
    uint64_t bindPaletteUpdateCalls = 0, bindPaletteApiBytes = 0, heldCurrentPaletteUpdates = 0;
    /// @brief Save update-begin provenance without changing legacy end-source columns.
    void BeginSource(uint64_t frame, uint64_t generation, uint64_t epoch)
    {
        resourceFrame = beginResourceFrame = frame; sceneGeneration = beginSceneGeneration = generation;
        resourceEpoch = beginResourceEpoch = epoch;
    }
    /// @brief Preserve raw end identity and retain any observed discontinuity, even across repeated finalization.
    void FinalizeSource(uint64_t frame, uint64_t generation, uint64_t epoch)
    {
        resourceFrame = endResourceFrame = frame; sceneGeneration = endSceneGeneration = generation;
        resourceEpoch = endResourceEpoch = epoch; sourceIdentityFinalized = true;
        sourceIdentityMismatch |= beginResourceFrame != frame || beginSceneGeneration != generation || beginResourceEpoch != epoch;
    }
    /// @return True only for finalized, unbroken source identity with coherent legacy/end columns.
    bool HasCompleteSourceIdentity() const
    {
        return sourceIdentityFinalized && !sourceIdentityMismatch
            && beginResourceFrame == endResourceFrame && beginSceneGeneration == endSceneGeneration && beginResourceEpoch == endResourceEpoch
            && resourceFrame == endResourceFrame && sceneGeneration == endSceneGeneration && resourceEpoch == endResourceEpoch;
    }
    /// @brief Diagnostic joins refuse incomplete rows; raw row data is still serialized.
    bool MatchesCompleteSource(uint64_t frame, uint64_t generation, uint64_t epoch) const
    {
        return HasCompleteSourceIdentity() && resourceFrame == frame && sceneGeneration == generation && resourceEpoch == epoch;
    }
    /// @brief Observe the final decision after state-machine updates; no decision is fed back into scheduling.
    void CountPoseDecision(bool lodGatePassed, bool reducedInterval, bool refreshEntered)
    {
        ++posePathActors; if (lodGatePassed) ++poseLodGatePassed; if (reducedInterval) ++poseReducedIntervalActors;
        if (refreshEntered) ++poseRefreshEntered; else { ++poseHeld; ++heldCurrentPaletteUpdates; }
    }
};
/// @note One TLS row is active only within a simulating AnimatorSystem update; shared storage serializes completed rows.

/// @note Private, default-OFF one-shot early-credit experiment. Public elapsed/count/component layout are unchanged.
namespace animation_phase_detail {
inline constexpr uint32_t CAPACITY=8192, NO_ENTRY=UINT32_MAX;
struct Key {
    uint64_t componentLifetime=0;
    uint32_t entityIndex=0,entityGeneration=0;
    bool operator==(const Key&) const = default;
};
struct Source {
    uint64_t resourceFrame=0,timeFrame=0,sceneGeneration=0,resourceInstance=0,resourceEpoch=0;
    uint64_t assetGeneration=0,rendererLifetime=0,modelIdentity=0,skeletonIdentity=0;
    uint32_t rendererIndex=0,rendererGeneration=0;
    std::array<uint32_t,6> lodSettings{};
    bool operator==(const Source&) const = default;
};
struct RefreshOutcome { bool refreshEntered=false,successfulLegacy=false,verifySuccess=false; };
struct Ticket {
    Key key; Source source;
    float interval=0,credit=0;
    uint64_t version=0,rejectionEpoch=0;
    uint32_t index=NO_ENTRY;
    bool tracked=false,eligible=false,canSeed=false,capacityRejected=false;
};
FBZZ_ENGINE_API uint64_t HashKey(const Key& key);
FBZZ_ENGINE_API float InitialCredit(const Key& key,float interval);
FBZZ_ENGINE_API bool CreditDue(bool normalDue,float elapsed,float interval,float credit);
/// @note No eviction: issued lifetimes remain tombstones on invalidation/removal, so neither gaps nor source changes re-seed.
class Ledger {
public:
    FBZZ_ENGINE_API explicit Ledger(uint32_t capacity=CAPACITY);
    FBZZ_ENGINE_API ~Ledger();
    FBZZ_ENGINE_API Ticket Prepare(const Key& key,const Source& source,float interval,bool eligible);
    FBZZ_ENGINE_API bool Complete(const Ticket& ticket,const RefreshOutcome& outcome,bool coherentEligible,float& seededCredit);
    FBZZ_ENGINE_API void Cancel(const Key& key);
    Ledger(const Ledger&)=delete; Ledger& operator=(const Ledger&)=delete;
private:
    struct Entry;
    std::unique_ptr<Entry[]> m_entries;
    std::unique_ptr<uint32_t[]> m_slots;
    uint32_t m_capacity=0,m_slotCount=0,m_count=0;
    std::atomic_flag m_owned=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> m_rejectionEpoch{0};
    FBZZ_ENGINE_API uint32_t Find(const Key& key) const;
};
}
FBZZ_ENGINE_API bool BeginAnimationPhaseDistribution(uint64_t resourceFrame,uint64_t timeFrame,uint64_t sceneGeneration);
FBZZ_ENGINE_API bool AnimationPhaseDistributionEnabled();
FBZZ_ENGINE_API animation_phase_detail::Ticket PrepareAnimationPhaseDistribution(const animation_phase_detail::Key& key,
    const animation_phase_detail::Source& source,float interval,bool eligible);
FBZZ_ENGINE_API void CancelAnimationPhaseDistribution(const animation_phase_detail::Key& key);
FBZZ_ENGINE_API void CompleteAnimationPhaseDistribution(const animation_phase_detail::Ticket& ticket,
    const animation_phase_detail::RefreshOutcome& outcome,bool coherentEligible);
FBZZ_ENGINE_API void CountAnimationPhaseCreditDue();
FBZZ_ENGINE_API std::string AnimationPhaseDistributionConditions();

FBZZ_ENGINE_API AnimationWorkRow* CurrentAnimationWorkRow();
FBZZ_ENGINE_API void CountAnimationCompatibilityUpdate(uint64_t bytes);
FBZZ_ENGINE_API void CaptureAnimationGpuWork(const renderer::GpuProfilerSnapshot& snapshot);
FBZZ_ENGINE_API bool AnimationGpuTraceRequested();
/// @note Experimental process opt-in; zero preserves continuous phase and the existing uncached pose path.
FBZZ_ENGINE_API uint32_t AnimationPoseShareRate();
FBZZ_ENGINE_API bool AnimationCompatibilityBatchRequested();
/// @note All counters describe one ExecuteSkinningComputePass call; existing frame-accumulated UI stats are not copied.
struct AnimationSkinningCpuRow {
    uint64_t resourceFrame=0,sceneGeneration=0,resourceEpoch=0;
    uint64_t totalNs=0,profilerSyncNs=0,replaySubmitNs=0,vertexSubmitNs=0;
    uint64_t resourceInstanceId=0,passOrdinalWithinResourceFrame=0;
    uint64_t renderersVisited=0,renderersEligible=0,alreadyCurrentReuse=0;
    uint64_t requestedDispatches=0,requestedVertexCount=0;
    /// @note Opt-in, count-only demand observations; overlapping reasons are not a partition or a safe-to-omit count.
    uint64_t demandScopeActive=0,sceneObjectCount=0,sceneAnimatorComponentCount=0,sceneSkinnedRendererComponentCount=0;
    uint64_t activeNativeScriptEntries=0,activeManagedScriptEntries=0,activeReflectionProbes=0,activeReflectionSceneProbes=0;
    uint64_t activeLightProbeVolumes=0,activeLightProbeBakeConsumers=0,activeInstancedComponents=0;
    uint64_t demandObservedRenderers=0,provisionalColorExcludedRenderers=0,conservativeNonColorRequiredRenderers=0;
    uint64_t provisionalNoKnownConsumerRenderers=0,legacyEnteredRenderers=0,renderersWithOutputRequests=0,nativePublishedRenderers=0;
    std::array<uint64_t,skinning_demand_detail::REASON_COUNT> outputDemandReasonRenderers{};
    /// @note Version 2 separates logical morph containers, selected source paths and native observations; no GPU omission is enabled.
    uint64_t demandClassifierVersion=2;
    std::array<uint64_t,skinning_demand_detail::INPUT_FLAG_COUNT> inputObservedRenderers{};
    /// @note Each slot observation uses the existing signature lookup; non-null does not prove coherent bytes or a complete vertex format.
    uint64_t sourceObservedSlots=0,sourceBasePathSlots=0,sourceMorphPathSlots=0,sourceDiffersFromBaseSlots=0;
    uint64_t sourceLookupNonNullBasePathSlots=0,sourceLookupNonNullMorphPathSlots=0,sourceLookupNullSlots=0,sourceNativeVersionZeroSlots=0;
    uint64_t boneBakeQueuedReplayRows=0;
    uint64_t sourceCacheHits=0,sourceCacheMisses=0,sourceCreateAttempts=0,sourceCreateSuccesses=0,sourceReleaseCalls=0,sourceCopyBytes=0;
    uint64_t outputCreateAttempts=0,outputCreateSuccesses=0,outputReleaseCalls=0,outputCapacityRejects=0;
    /// @note Vectors counts logical builds, capacity sums repeated observations; lower bound counts nonempty builds with observed successful growth.
    uint64_t signatureVectors=0,signatureBytes=0,signatureCapacityBytes=0,signatureAllocationLowerBound=0;
    renderer::skinning_trace_detail::RequestCounters requests;
    /// @note Growth-push time is inside build time and includes allocation, relocation, old-block free and the appended word; it is not pure malloc time.
    uint64_t signatureBuildNs=0,signatureGrowthPushNs=0,signatureCompareNs=0,signaturePublicationMoveNs=0,signatureReserveNs=0;
    uint64_t signatureBuildCalls=0,signatureGrowthPushCalls=0,signatureCompareCalls=0,signaturePublicationMoveCalls=0,signatureReserveCalls=0;
    /// @note Successful capacity changes count new vector payload blocks, excluding allocator bookkeeping and CLR/GPU memory.
    uint64_t signaturePayloadAllocationCalls=0,signaturePayloadAllocationBytes=0,signatureGrowthRelocatedLogicalBytes=0;
    uint64_t signatureClockPairCount=0,signatureClockProbeNs=0,unattributedSkinningNs=0;
    /// @note Assign includes the native destination copy and any destination allocation; clear and scratch acquisition are disjoint spans.
    uint64_t signatureAssignNs=0,signatureAssignCalls=0,signatureAssignCopyLogicalBytes=0;
    uint64_t signatureClearNs=0,signatureClearCalls=0;
    uint64_t signatureDestinationPayloadAllocationCalls=0,signatureDestinationPayloadAllocationBytes=0;
    uint64_t signatureScratchAcquireNs=0,signatureScratchAcquireCalls=0;
    /// @note Successful native storage objects and pointer-vector payload blocks exclude allocator metadata and word payload growth.
    uint64_t signatureScratchStorageAllocationCalls=0,signatureScratchStorageAllocationBytes=0;
    uint64_t signatureScratchPoolPointerAllocationCalls=0,signatureScratchPoolPointerAllocationBytes=0;
    /// @note Current-thread pass-end gauges are retained capacity, not allocation volume or pass-attributable physical RAM.
    uint64_t signatureScratchRetainedPayloadBytes=0,signatureScratchRetainedStorageBytes=0;
    uint64_t signatureScratchRetainedPointerPayloadBytes=0,signatureScratchSlotsAtPassEnd=0;
};
/// @note This is an invocation ordinal per resource instance/epoch/frame, not a GPU view ID; zero means ordinal tracking capacity was exhausted.
FBZZ_ENGINE_API void AssignAnimationSkinningPassOrdinal(AnimationSkinningCpuRow& row);
FBZZ_ENGINE_API void CaptureAnimationSkinningCpuWork(const AnimationSkinningCpuRow& row);
class AnimationSkinningCpuFrame {
public:
    AnimationSkinningCpuFrame(uint64_t frame,uint64_t scene,uint64_t epoch,uint64_t instance):m_enabled(AnimationGpuTraceRequested())
    {
        row.resourceFrame=frame;row.sceneGeneration=scene;row.resourceEpoch=epoch;row.resourceInstanceId=instance;
        if(m_enabled)AssignAnimationSkinningPassOrdinal(row);
        m_start=Mark();
    }
    ~AnimationSkinningCpuFrame()
    {
        if(m_enabled){
            row.totalNs=Elapsed(m_start);
            /// @note Subtract only disjoint raw spans; growth and reserve are inside build, and the clock probe is not an exact overhead correction.
            const uint64_t measured=row.profilerSyncNs+row.replaySubmitNs+row.vertexSubmitNs+row.signatureBuildNs
                +row.signatureCompareNs+row.signaturePublicationMoveNs+row.signatureClockProbeNs
                +row.signatureAssignNs+row.signatureClearNs+row.signatureScratchAcquireNs;
            row.unattributedSkinningNs=row.totalNs>measured?row.totalNs-measured:0;
            CaptureAnimationSkinningCpuWork(row);
        }
    }
    uint64_t Mark() const {return m_enabled?static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()):0;}
    uint64_t Elapsed(uint64_t start) const {return m_enabled?Mark()-start:0;}
    AnimationSkinningCpuRow* TraceRow(){return m_enabled?&row:nullptr;}
    AnimationSkinningCpuRow row;
    AnimationSkinningCpuFrame(const AnimationSkinningCpuFrame&)=delete;
    AnimationSkinningCpuFrame& operator=(const AnimationSkinningCpuFrame&)=delete;
private:
    bool m_enabled=false;uint64_t m_start=0;
};

class AnimationWorkScope {
public:
    explicit AnimationWorkScope(AnimationWorkPart part) : m_row(CurrentAnimationWorkRow()), m_part(part)
    { if (m_row) m_start = std::chrono::steady_clock::now(); }
    ~AnimationWorkScope()
    {
        if (m_row) m_row->nanoseconds[static_cast<uint32_t>(m_part)] += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count());
    }
    AnimationWorkScope(const AnimationWorkScope&) = delete;
    AnimationWorkScope& operator=(const AnimationWorkScope&) = delete;
private:
    AnimationWorkRow* m_row = nullptr;
    AnimationWorkPart m_part;
    std::chrono::steady_clock::time_point m_start{};
};
class AnimationWorkFrame {
public:
    FBZZ_ENGINE_API AnimationWorkFrame(bool simulating, uint64_t resourceFrame, uint64_t sceneGeneration, uint64_t resourceEpoch);
    FBZZ_ENGINE_API ~AnimationWorkFrame();
    FBZZ_ENGINE_API void FinalizeSource(uint64_t resourceFrame, uint64_t sceneGeneration, uint64_t resourceEpoch);
    AnimationWorkFrame(const AnimationWorkFrame&) = delete;
    AnimationWorkFrame& operator=(const AnimationWorkFrame&) = delete;
private:
    AnimationWorkRow m_row;
    AnimationWorkRow* m_previous = nullptr;
    bool m_active = false;
};
}
