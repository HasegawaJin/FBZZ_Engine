/// @file    AnimationWorkTrace.hpp
/// @brief   アニメーション演算時間と完了済み GPU タイムスタンプの任意計測。
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
/// @note 識別にはスカラー値だけを使い、オブジェクト・コンポーネント・資源・配列・コールバックのポインターは残さない。
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
/// @note 先頭の元のビット列と、不足分を単位行列で埋めた末尾を含め、実効 128 行列のパケットとして比較する。
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
/// @note 更新を観測してから保持区間を確定し、欠落や無効な行があれば連続性を打ち切る。
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
/// @note 本体の上限付き収集と、容量を決定的に検証するテストが共有する。
inline bool ClaimBoundedSlot(uint32_t& count,uint32_t capacity,uint32_t& index)
{
    if(count>=capacity)return false; index=count++; return true;
}
}
class Scene;
namespace animation_actor_detail { struct ActorCapture; }
/// @note 有効化したときだけスカラー値を収集し、無効時は時刻取得もバッファ確保も行わない。
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
    /// @note 姿勢・演出のコールバック後にコンポーネントを再取得し、アクター参照を持ち越さない。
    FBZZ_ENGINE_API void FinishActor(animation_actor_detail::ActorRow* row,Scene& scene,uint64_t frame,uint64_t generation,uint64_t epoch,uint64_t instance);
    /// @note 互換バッチの送信後、破棄前に呼び出す。未処理コンポーネントの寿命を再検証する。
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
    /// @note 追記する診断項目はアクター更新・呼び出し・書き込みの回数であり、個体数や GPU バイト数を表さない。
    static constexpr uint32_t POSE_OBSERVATION_SCHEMA = 1;
    bool poseCountersAvailable = false, legacyBoneWritesAvailable = false, paletteApiCountersAvailable = false;
    uint64_t beginResourceFrame = 0, beginSceneGeneration = 0, beginResourceEpoch = 0;
    uint64_t endResourceFrame = 0, endSceneGeneration = 0, endResourceEpoch = 0;
    bool sourceIdentityFinalized = false, sourceIdentityMismatch = false;
    uint64_t posePathActors = 0, poseLodGatePassed = 0, poseReducedIntervalActors = 0;
    /// @note 更新経路に入った回数にはバインド姿勢、エラー、外部姿勢へのフォールバックも含む。
    uint64_t poseRefreshEntered = 0, poseHeld = 0, poseLegacyFkCalls = 0, poseBakedSuccesses = 0;
    uint64_t poseBindFallbackActors = 0, poseOtherRefreshActors = 0, legacyEvaluatedBoneWrites = 0;
    uint64_t previousPaletteUpdateCalls = 0, previousPaletteApiBytes = 0;
    uint64_t currentPaletteUpdateCalls = 0, currentPaletteApiBytes = 0;
    uint64_t bindPaletteUpdateCalls = 0, bindPaletteApiBytes = 0, heldCurrentPaletteUpdates = 0;
    /// @brief 既存の終了時の入力列を変更せず、更新開始時の入力情報を保存する。
    void BeginSource(uint64_t frame, uint64_t generation, uint64_t epoch)
    {
        resourceFrame = beginResourceFrame = frame; sceneGeneration = beginSceneGeneration = generation;
        resourceEpoch = beginResourceEpoch = epoch;
    }
    /// @brief 完了処理を繰り返しても、終了時の元の識別情報と観測済みの不連続を保持する。
    void FinalizeSource(uint64_t frame, uint64_t generation, uint64_t epoch)
    {
        resourceFrame = endResourceFrame = frame; sceneGeneration = endSceneGeneration = generation;
        resourceEpoch = endResourceEpoch = epoch; sourceIdentityFinalized = true;
        sourceIdentityMismatch |= beginResourceFrame != frame || beginSceneGeneration != generation || beginResourceEpoch != epoch;
    }
    /// @return 完了済みで入力元が連続し、既存列と終了時列が整合する場合に限り true。
    bool HasCompleteSourceIdentity() const
    {
        return sourceIdentityFinalized && !sourceIdentityMismatch
            && beginResourceFrame == endResourceFrame && beginSceneGeneration == endSceneGeneration && beginResourceEpoch == endResourceEpoch
            && resourceFrame == endResourceFrame && sceneGeneration == endSceneGeneration && resourceEpoch == endResourceEpoch;
    }
    /// @brief 未完了の行は診断の結合から外し、元データはシリアライズする。
    bool MatchesCompleteSource(uint64_t frame, uint64_t generation, uint64_t epoch) const
    {
        return HasCompleteSourceIdentity() && resourceFrame == frame && sceneGeneration == generation && resourceEpoch == epoch;
    }
    /// @brief 状態機械の更新後に確定した判断を記録し、スケジューリングには反映しない。
    void CountPoseDecision(bool lodGatePassed, bool reducedInterval, bool refreshEntered)
    {
        ++posePathActors; if (lodGatePassed) ++poseLodGatePassed; if (reducedInterval) ++poseReducedIntervalActors;
        if (refreshEntered) ++poseRefreshEntered; else { ++poseHeld; ++heldCurrentPaletteUpdates; }
    }
};
/// @note シミュレーション中の AnimatorSystem の更新内だけでスレッドローカルの作業行が有効となり、完了した行は共有領域で直列化する。

/// @note 初回だけ先行加算する実験は既定で無効であり、公開する時間・件数とコンポーネント構造を維持する。
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
/// @note 失効・削除後も識別子を使用済みとして保持し、欠落や入力元の変更で再採番しない。
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
/// @note プロセス単位で有効化する実験。値がゼロなら連続位相と既存の非キャッシュ姿勢経路を使う。
FBZZ_ENGINE_API uint32_t AnimationPoseShareRate();
FBZZ_ENGINE_API bool AnimationCompatibilityBatchRequested();
/// @note カウンターは ExecuteSkinningComputePass 1 回分のみで、フレーム累積の UI 統計は含めない。
struct AnimationSkinningCpuRow {
    uint64_t resourceFrame=0,sceneGeneration=0,resourceEpoch=0;
    uint64_t totalNs=0,profilerSyncNs=0,replaySubmitNs=0,vertexSubmitNs=0;
    uint64_t resourceInstanceId=0,passOrdinalWithinResourceFrame=0;
    uint64_t renderersVisited=0,renderersEligible=0,alreadyCurrentReuse=0;
    uint64_t requestedDispatches=0,requestedVertexCount=0;
    /// @note 需要の件数だけを任意収集する。理由は重複するため、合計値を内訳や省略可能件数として扱わない。
    uint64_t demandScopeActive=0,sceneObjectCount=0,sceneAnimatorComponentCount=0,sceneSkinnedRendererComponentCount=0;
    uint64_t activeNativeScriptEntries=0,activeManagedScriptEntries=0,activeReflectionProbes=0,activeReflectionSceneProbes=0;
    uint64_t activeLightProbeVolumes=0,activeLightProbeBakeConsumers=0,activeInstancedComponents=0;
    uint64_t demandObservedRenderers=0,provisionalColorExcludedRenderers=0,conservativeNonColorRequiredRenderers=0;
    uint64_t provisionalNoKnownConsumerRenderers=0,legacyEnteredRenderers=0,renderersWithOutputRequests=0,nativePublishedRenderers=0;
    std::array<uint64_t,skinning_demand_detail::REASON_COUNT> outputDemandReasonRenderers{};
    /// @note 版 2 では論理上のモーフ配列、選択した入力経路、GPU 側の観測を区別する。計測値による GPU 処理の省略は行わない。
    uint64_t demandClassifierVersion=2;
    std::array<uint64_t,skinning_demand_detail::INPUT_FLAG_COUNT> inputObservedRenderers{};
    /// @note 各スロットは既存のシグネチャ取得で観測する。取得に成功しても、内容の整合性や頂点形式の完全性は保証しない。
    uint64_t sourceObservedSlots=0,sourceBasePathSlots=0,sourceMorphPathSlots=0,sourceDiffersFromBaseSlots=0;
    uint64_t sourceLookupNonNullBasePathSlots=0,sourceLookupNonNullMorphPathSlots=0,sourceLookupNullSlots=0,sourceNativeVersionZeroSlots=0;
    uint64_t boneBakeQueuedReplayRows=0;
    uint64_t sourceCacheHits=0,sourceCacheMisses=0,sourceCreateAttempts=0,sourceCreateSuccesses=0,sourceReleaseCalls=0,sourceCopyBytes=0;
    uint64_t outputCreateAttempts=0,outputCreateSuccesses=0,outputReleaseCalls=0,outputCapacityRejects=0;
    /// @note 配列数は論理的な生成回数、容量は観測の合計、下限は容量増加を観測した非空の生成回数を表す。
    uint64_t signatureVectors=0,signatureBytes=0,signatureCapacityBytes=0,signatureAllocationLowerBound=0;
    renderer::skinning_trace_detail::RequestCounters requests;
    /// @note 拡張を伴う追加時間は、確保・移動・旧領域の解放・新要素の追加を含み、生成時間の内数となる。
    uint64_t signatureBuildNs=0,signatureGrowthPushNs=0,signatureCompareNs=0,signaturePublicationMoveNs=0,signatureReserveNs=0;
    uint64_t signatureBuildCalls=0,signatureGrowthPushCalls=0,signatureCompareCalls=0,signaturePublicationMoveCalls=0,signatureReserveCalls=0;
    /// @note 容量増分は配列本体の再確保だけを数え、アロケーターの管理情報や CLR・GPU のメモリを除く。
    uint64_t signaturePayloadAllocationCalls=0,signaturePayloadAllocationBytes=0,signatureGrowthRelocatedLogicalBytes=0;
    uint64_t signatureClockPairCount=0,signatureClockProbeNs=0,unattributedSkinningNs=0;
    /// @note 代入時間は宛先へのコピーと確保を含み、クリアおよび作業領域の取得とは重複しない。
    uint64_t signatureAssignNs=0,signatureAssignCalls=0,signatureAssignCopyLogicalBytes=0;
    uint64_t signatureClearNs=0,signatureClearCalls=0;
    uint64_t signatureDestinationPayloadAllocationCalls=0,signatureDestinationPayloadAllocationBytes=0;
    uint64_t signatureScratchAcquireNs=0,signatureScratchAcquireCalls=0;
    /// @note 生成に成功したネイティブ記憶領域とポインター配列本体を数え、管理情報やワード配列の増加は除く。
    uint64_t signatureScratchStorageAllocationCalls=0,signatureScratchStorageAllocationBytes=0;
    uint64_t signatureScratchPoolPointerAllocationCalls=0,signatureScratchPoolPointerAllocationBytes=0;
    /// @note パス終了時の現在スレッドの値は保持容量を表し、確保総量やパスに帰属する実メモリを表さない。
    uint64_t signatureScratchRetainedPayloadBytes=0,signatureScratchRetainedStorageBytes=0;
    uint64_t signatureScratchRetainedPointerPayloadBytes=0,signatureScratchSlotsAtPassEnd=0;
};
/// @note 資源管理者・世代・フレームごとの呼び出し番号。ゼロは追跡枠の使い切りを表す。
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
            /// @note 重複しない生の区間だけを差し引く。拡張と予約は生成時間に含み、計時自体の時間は概算となる。
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
