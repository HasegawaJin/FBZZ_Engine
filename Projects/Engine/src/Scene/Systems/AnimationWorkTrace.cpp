/// @file    AnimationWorkTrace.cpp
/// @brief   Fixed-size independent animation trace, serialized after the capture window.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include "AnimationWorkTrace.hpp"
#include <Core/Util/FileSystem.hpp>
#include <Core/Logger.hpp>
#include <Graphics/Renderer/GpuProfiler.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <atomic>
#include <utility>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <Math/MathUtils.hpp>
#include <new>
#include <mutex>
#include <sstream>
#include <string>

namespace fbzz::scene {
namespace {
thread_local AnimationWorkRow* s_animationWorkRow = nullptr;
std::string ReadAnimationTraceEnv(const char* name)
{
#ifdef _MSC_VER
    char* raw = nullptr; size_t length = 0;
    if (_dupenv_s(&raw, &length, name) != 0 || !raw) return {};
    const std::unique_ptr<char, decltype(&std::free)> value(raw, &std::free);
    return value.get();
#else
    const char* value = std::getenv(name); return value ? value : "";
#endif
}
uint32_t ReadAnimationTraceCount(const char* name, uint32_t fallback, uint32_t maximum)
{
    const auto text = ReadAnimationTraceEnv(name);
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return fallback;
    if (text.size() > 9) return fallback;
    const auto value = std::strtoul(text.c_str(), nullptr, 10);
    return value <= maximum ? static_cast<uint32_t>(value) : fallback;
}
struct AnimationGpuWorkRow {
    uint64_t resourceFrame = 0, physicalFrame = 0, deviceEpoch = 0, sourceScene = 0;
    uint64_t sourceResourceEpoch = 0, viewId = 0;
    double milliseconds = 0.0;
    bool complete = false;
    uint32_t dropped = 0;
    uint32_t part = 0;
};
struct AnimationWorkCapture {
    static constexpr uint32_t CAPACITY = 3600;
    std::mutex mutex;
    std::string path = ReadAnimationTraceEnv("FBZZ_ANIMATION_TRACE_OUTPUT");
    uint32_t warmup = ReadAnimationTraceCount("FBZZ_ANIMATION_TRACE_WARMUP", 210, 100000);
    uint32_t samples = ReadAnimationTraceCount("FBZZ_ANIMATION_TRACE_SAMPLES", 600, CAPACITY);
    uint32_t shareRate = ReadAnimationTraceCount("FBZZ_BONE_POSE_SHARE_HZ", 0, 1000);
    bool batchCompatibility = ReadAnimationTraceCount("FBZZ_ANIMATION_BATCH_CB", 0, 1) != 0;
    std::array<AnimationWorkRow, CAPACITY> cpu{};
    std::array<AnimationGpuWorkRow, CAPACITY * 3> gpu{};
    std::array<AnimationSkinningCpuRow,CAPACITY * 2> skin{};
    uint32_t skinCount=0;
    struct SkinningPassOrdinal {
        uint64_t resourceInstanceId=0,resourceEpoch=0,resourceFrame=UINT64_MAX,ordinal=0;
    };
    std::array<SkinningPassOrdinal,64> skinningOrdinals{};
    uint32_t warmed = 0, cpuCount = 0, gpuCount = 0, duplicateCpuFrames = 0;
    uint64_t lastStartedFrame = UINT64_MAX;
    uint64_t lastBeginNs = 0, lastBeginGeneration = 0, lastBeginEpoch = 0;
    bool cpuWritten = false, gpuWritten = false;
    uint32_t vertexGpuFrames = 0;
};
AnimationWorkCapture& Capture()
{
    static AnimationWorkCapture capture;
    return capture;
}
void WriteAnimationCpuCapture(AnimationWorkCapture& capture)
{
    std::ostringstream text;
    text << "resourceFrame,sceneGeneration,resourceEpoch,totalNs,previousCbNs,currentCbNs,stateMachineNs,clipSelectionNs,boneHierarchyNs,legacyFkNs,boneApplicationNs,propagationRebuildNs,bakedFkNs,sideEffectsBoundsNs,bakePreparationNs,batchSubmitNs,animators,boneMatrices,cbUpdates,cbApiBytes,legacyPoses,bakedCpuPoses,bakedGpuPoses,poseCacheHits,poseCacheMisses,cbBulkPackets,wallBeginIntervalNs,poseObservationSchema,poseCountersAvailable,legacyBoneWritesAvailable,paletteApiCountersAvailable,sourceIdentityFinalized,sourceIdentityComplete,beginResourceFrame,beginSceneGeneration,beginResourceEpoch,endResourceFrame,endSceneGeneration,endResourceEpoch,posePathActors,poseLodGatePassed,poseReducedIntervalActors,poseRefreshEntered,poseHeld,poseLegacyFkCalls,poseBakedSuccesses,poseBindFallbackActors,poseOtherRefreshActors,legacyEvaluatedBoneWrites,previousPaletteUpdateCalls,previousPaletteApiBytes,currentPaletteUpdateCalls,currentPaletteApiBytes,bindPaletteUpdateCalls,bindPaletteApiBytes,heldCurrentPaletteUpdates,actualGpuPaletteBytesAvailable,actualGpuPaletteBytes,bakedEvaluatedBonesAvailable,bakedEvaluatedBones\n";
    for (uint32_t index = 0; index < capture.cpuCount; ++index) {
        const auto& row = capture.cpu[index];
        text << row.resourceFrame << ',' << row.sceneGeneration << ',' << row.resourceEpoch;
        for (uint64_t elapsed : row.nanoseconds) text << ',' << elapsed;
        text << ',' << row.animators << ',' << row.boneMatrices << ',' << row.cbUpdates << ',' << row.cbApiBytes
             << ',' << row.legacyPoses << ',' << row.bakedCpuPoses << ',' << row.bakedGpuPoses
             << ',' << row.poseCacheHits << ',' << row.poseCacheMisses << ',' << row.cbBulkPackets << ',' << row.wallBeginIntervalNs;
        text << ',' << AnimationWorkRow::POSE_OBSERVATION_SCHEMA << ',' << row.poseCountersAvailable
             << ',' << row.legacyBoneWritesAvailable << ',' << row.paletteApiCountersAvailable
             << ',' << row.sourceIdentityFinalized << ',' << row.HasCompleteSourceIdentity()
             << ',' << row.beginResourceFrame << ',' << row.beginSceneGeneration << ',' << row.beginResourceEpoch;
        for (const uint64_t value : {row.endResourceFrame, row.endSceneGeneration, row.endResourceEpoch}) {
            text << ','; if (row.sourceIdentityFinalized) text << value;
        }
        for (const uint64_t value : {row.posePathActors, row.poseLodGatePassed, row.poseReducedIntervalActors,
                row.poseRefreshEntered, row.poseHeld, row.poseLegacyFkCalls, row.poseBakedSuccesses,
                row.poseBindFallbackActors, row.poseOtherRefreshActors}) {
            text << ','; if (row.poseCountersAvailable) text << value;
        }
        text << ','; if (row.legacyBoneWritesAvailable) text << row.legacyEvaluatedBoneWrites;
        for (const uint64_t value : {row.previousPaletteUpdateCalls, row.previousPaletteApiBytes,
                row.currentPaletteUpdateCalls, row.currentPaletteApiBytes, row.bindPaletteUpdateCalls, row.bindPaletteApiBytes}) {
            text << ','; if (row.paletteApiCountersAvailable) text << value;
        }
        text << ','; if (row.poseCountersAvailable) text << row.heldCurrentPaletteUpdates;
        /// @note This CPU observation does not observe GPU transfer bytes or bones evaluated by baked/cache replay.
        text << ",0,,0,\n";
    }
    if (!util::FileSystem::WriteTextAtomic(capture.path + ".cpu.csv", text.str()))
        FBZZ_LOG_WARN("AnimationWorkTrace: unable to save completed CPU capture");
    std::ostringstream metadata;
    metadata << "samples=" << capture.cpuCount << "\nwarmed=" << capture.warmed
             << "\nduplicateCpuFrames=" << capture.duplicateCpuFrames << "\nposeShareHz=" << capture.shareRate
             << "\nclock=steady_clock_nanoseconds\ncbBytes=ResourceManager_Update_arguments_not_GPU_submitted_bytes\n";
    uint32_t sourceIdentityIncompleteRows = 0;
    for (uint32_t index = 0; index < capture.cpuCount; ++index)
        if (!capture.cpu[index].HasCompleteSourceIdentity()) ++sourceIdentityIncompleteRows;
    metadata << "poseObservationSchema=1\nsourceIdentityIncompleteRows=" << sourceIdentityIncompleteRows
             << "\nposeRefreshEntered=refresh_path_entry_not_successful_clip_evaluation"
                "\nlegacyEvaluatedBoneWrites=valid_direct_legacy_FK_palette_writes_not_loaded_skeleton_count"
                "\nposeBindFallbackActors=refresh_actors_with_observed_bind_palette_call"
                "\npaletteApiBytes=previous_current_bind_update_argument_bytes_not_GPU_submitted_bytes"
                "\nactualGpuPaletteBytesAvailable=0\nbakedEvaluatedBonesAvailable=0"
                "\nnearPromotionAvailable=0\nresetHistoryCountersAvailable=0\ninterpolationWorkAvailable=0"
                "\nsourceIdentityIncompleteRowsExcludedFromCpuGpuAndSkinJoins=1\n";
    metadata << AnimationPhaseDistributionConditions();
    util::FileSystem::WriteTextAtomic(capture.path + ".conditions.txt", metadata.str());
    capture.cpuWritten = true;
}
void WriteAnimationGpuCapture(AnimationWorkCapture& capture)
{
    std::ostringstream text;
    text.precision(12);
    text << "resourceFrame,physicalFrame,deviceEpoch,sceneGeneration,resourceEpoch,viewId,gpuMs,snapshotComplete,droppedPassCount,part\n";
    for (uint32_t index = 0; index < capture.gpuCount; ++index) {
        const auto& row = capture.gpu[index];
        text << row.resourceFrame << ',' << row.physicalFrame << ',' << row.deviceEpoch << ',' << row.sourceScene
             << ',' << row.sourceResourceEpoch << ',' << row.viewId << ',' << row.milliseconds
             << ',' << row.complete << ',' << row.dropped << ',' << row.part << '\n';
    }
    if (!util::FileSystem::WriteTextAtomic(capture.path + ".gpu.csv", text.str()))
        FBZZ_LOG_WARN("AnimationWorkTrace: unable to save completed GPU capture");
    capture.gpuWritten = true;
}
}

namespace animation_phase_detail {
struct Ledger::Entry {
    Key key; Source source;
    float interval=0,credit=0;
    uint64_t version=0,rejectionEpoch=0;
    bool issued=false;
};
namespace {
struct Lease {
    std::atomic_flag& flag; bool claimed;
    explicit Lease(std::atomic_flag& f):flag(f),claimed(!f.test_and_set(std::memory_order_acquire)) {}
    ~Lease(){if(claimed)flag.clear(std::memory_order_release);}
};
uint64_t Mix(uint64_t value)
{
    value^=value>>30;value*=UINT64_C(0xbf58476d1ce4e5b9);
    value^=value>>27;value*=UINT64_C(0x94d049bb133111eb);return value^(value>>31);
}
bool Next(const Source& previous,const Source& current)
{
    if(previous.resourceFrame==UINT64_MAX || previous.timeFrame==UINT64_MAX
        || previous.resourceFrame+1!=current.resourceFrame || previous.timeFrame+1!=current.timeFrame)return false;
    auto adjusted=previous;adjusted.resourceFrame=current.resourceFrame;adjusted.timeFrame=current.timeFrame;
    return adjusted==current;
}
}
uint64_t HashKey(const Key& key)
{
    return Mix(key.componentLifetime)^Mix((uint64_t{key.entityGeneration}<<32)|key.entityIndex);
}
float InitialCredit(const Key& key,float interval)
{
    if(key.componentLifetime==0 || !std::isfinite(interval) || interval<=math::EPSILON || interval>1.0f)return 0;
    const float value=interval*(static_cast<float>(HashKey(key)&UINT64_C(65535))/65536.0f);
    return (std::min)(value,std::nextafter(interval,0.0f));
}
bool CreditDue(bool normalDue,float elapsed,float interval,float credit)
{
    return !normalDue && std::isfinite(elapsed) && std::isfinite(interval) && std::isfinite(credit)
        && elapsed>=0 && interval>math::EPSILON && interval<=1.0f && credit>0 && credit<interval
        && elapsed+credit+math::EPSILON>=interval;
}
Ledger::Ledger(uint32_t capacity)
{
    if(!capacity || capacity>CAPACITY)return;
    uint32_t slots=1;while(slots<capacity*2)slots*=2;
    auto entries=std::unique_ptr<Entry[]>(new(std::nothrow)Entry[capacity]);
    auto table=std::unique_ptr<uint32_t[]>(new(std::nothrow)uint32_t[slots]{});
    if(!entries || !table)return;
    m_entries=std::move(entries);m_slots=std::move(table);m_capacity=capacity;m_slotCount=slots;
}
Ledger::~Ledger()=default;
uint32_t Ledger::Find(const Key& key) const
{
    if(!m_slotCount || !key.componentLifetime)return NO_ENTRY;
    uint32_t slot=static_cast<uint32_t>(HashKey(key))&(m_slotCount-1);
    for(uint32_t probe=0;probe<m_slotCount;++probe,slot=(slot+1)&(m_slotCount-1)) {
        const uint32_t stored=m_slots[slot];if(!stored)return NO_ENTRY;
        if(m_entries[stored-1].key==key)return stored-1;
    }
    return NO_ENTRY;
}
Ticket Ledger::Prepare(const Key& key,const Source& source,float interval,bool eligible)
{
    Ticket ticket;ticket.key=key;ticket.source=source;ticket.interval=interval;
    eligible=eligible && key.componentLifetime!=0 && std::isfinite(interval) && interval>math::EPSILON && interval<=1.0f;
    Lease lease(m_owned);if(!lease.claimed){m_rejectionEpoch.fetch_add(1,std::memory_order_acq_rel);return ticket;}
    uint32_t index=Find(key);
    if(index==NO_ENTRY) {
        if(!eligible)return ticket;
        if(m_count>=m_capacity){ticket.capacityRejected=true;return ticket;}
        index=m_count++;auto& entry=m_entries[index];entry.key=key;entry.source=source;entry.interval=interval;
        uint32_t slot=static_cast<uint32_t>(HashKey(key))&(m_slotCount-1);
        while(m_slots[slot])slot=(slot+1)&(m_slotCount-1);m_slots[slot]=index+1;
    } else {
        auto& entry=m_entries[index];
        if(!eligible || interval!=entry.interval || !Next(entry.source,source)
            || entry.rejectionEpoch!=m_rejectionEpoch.load(std::memory_order_acquire))entry.credit=0;
        entry.source=source;entry.interval=interval;
    }
    auto& entry=m_entries[index];
    if(entry.version==UINT64_MAX){entry.credit=0;entry.issued=true;return ticket;}
    ++entry.version;entry.rejectionEpoch=m_rejectionEpoch.load(std::memory_order_acquire);
    ticket.index=index;ticket.version=entry.version;ticket.rejectionEpoch=entry.rejectionEpoch;
    ticket.tracked=true;ticket.eligible=eligible;ticket.canSeed=eligible&&!entry.issued;
    ticket.credit=eligible?entry.credit:0;return ticket;
}
bool Ledger::Complete(const Ticket& ticket,const RefreshOutcome& outcome,bool coherentEligible,float& seededCredit)
{
    seededCredit=0;if(!ticket.tracked)return false;
    Lease lease(m_owned);if(!lease.claimed){m_rejectionEpoch.fetch_add(1,std::memory_order_acq_rel);return false;}
    if(ticket.index>=m_count)return false;
    auto& entry=m_entries[ticket.index];
    if(!(entry.key==ticket.key) || entry.version!=ticket.version)return false;
    if(!coherentEligible || !ticket.eligible || ticket.rejectionEpoch!=m_rejectionEpoch.load(std::memory_order_acquire)) {
        entry.credit=0;return false;
    }
    if(outcome.refreshEntered)entry.credit=0;
    if(!outcome.refreshEntered || !outcome.successfulLegacy || entry.issued)return false;
    entry.issued=true;entry.credit=seededCredit=InitialCredit(entry.key,ticket.interval);return true;
}
void Ledger::Cancel(const Key& key)
{
    Lease lease(m_owned);if(!lease.claimed){m_rejectionEpoch.fetch_add(1,std::memory_order_acq_rel);return;}
    const uint32_t index=Find(key);if(index==NO_ENTRY)return;
    auto& entry=m_entries[index];entry.credit=0;
    if(entry.version!=UINT64_MAX)++entry.version;else entry.issued=true;
}
}
namespace {
struct PhaseState {
    bool requested=ReadAnimationTraceCount("FBZZ_ANIMATION_PHASE_DISTRIBUTION",0,1)!=0;
    std::unique_ptr<animation_phase_detail::Ledger> ledger=requested
        ?std::unique_ptr<animation_phase_detail::Ledger>(new(std::nothrow)animation_phase_detail::Ledger):nullptr;
    std::atomic_flag modeLogged=ATOMIC_FLAG_INIT,seedLogged=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> seeds{0},creditDues{0},sampledCreditDues{0},capacityRejects{0};
};
PhaseState& Phase(){static PhaseState state;return state;}
}
bool BeginAnimationPhaseDistribution(uint64_t resourceFrame,uint64_t timeFrame,uint64_t sceneGeneration)
{
    auto& state=Phase();
    if(!state.modeLogged.test_and_set(std::memory_order_relaxed))
        FBZZ_LOG_INFO("AnimationPhaseDistribution mode=%u resourceFrame=%llu timeFrame=%llu sceneGeneration=%llu",
            state.requested?1u:0u,static_cast<unsigned long long>(resourceFrame),static_cast<unsigned long long>(timeFrame),static_cast<unsigned long long>(sceneGeneration));
    return state.requested && state.ledger!=nullptr;
}
bool AnimationPhaseDistributionEnabled(){auto& state=Phase();return state.requested && state.ledger!=nullptr;}
animation_phase_detail::Ticket PrepareAnimationPhaseDistribution(const animation_phase_detail::Key& key,
    const animation_phase_detail::Source& source,float interval,bool eligible)
{
    auto& state=Phase();if(!state.ledger)return {};
    auto ticket=state.ledger->Prepare(key,source,interval,eligible);
    if(ticket.capacityRejected)state.capacityRejects.fetch_add(1,std::memory_order_relaxed);return ticket;
}
void CancelAnimationPhaseDistribution(const animation_phase_detail::Key& key)
{
    auto& state=Phase();if(state.ledger)state.ledger->Cancel(key);
}
void CompleteAnimationPhaseDistribution(const animation_phase_detail::Ticket& ticket,
    const animation_phase_detail::RefreshOutcome& outcome,bool coherentEligible)
{
    auto& state=Phase();if(!state.ledger)return;float credit=0;
    if(!state.ledger->Complete(ticket,outcome,coherentEligible,credit))return;
    state.seeds.fetch_add(1,std::memory_order_relaxed);
    if(!state.seedLogged.test_and_set(std::memory_order_relaxed))
        FBZZ_LOG_INFO("AnimationPhaseDistributionSeed applied=1 resourceFrame=%llu timeFrame=%llu sceneGeneration=%llu entityIndex=%u entityGeneration=%u componentLifetime=%llu interval=%.9g credit=%.9g firstCycleOnly=1 source=successfulSafeLegacyPose",
            static_cast<unsigned long long>(ticket.source.resourceFrame),static_cast<unsigned long long>(ticket.source.timeFrame),static_cast<unsigned long long>(ticket.source.sceneGeneration),
            ticket.key.entityIndex,ticket.key.entityGeneration,static_cast<unsigned long long>(ticket.key.componentLifetime),static_cast<double>(ticket.interval),static_cast<double>(credit));
}
void CountAnimationPhaseCreditDue()
{
    auto& state=Phase();state.creditDues.fetch_add(1,std::memory_order_relaxed);
    if(CurrentAnimationWorkRow())state.sampledCreditDues.fetch_add(1,std::memory_order_relaxed);
}
std::string AnimationPhaseDistributionConditions()
{
    auto& state=Phase();std::ostringstream text;
    text<<"phaseDistributionRequested="<<state.requested<<"\nphaseSeeds="<<state.seeds.load(std::memory_order_relaxed)
        <<"\nphaseCreditDue="<<state.creditDues.load(std::memory_order_relaxed)
        <<"\nphaseCreditDueInCpuTraceSamples="<<state.sampledCreditDues.load(std::memory_order_relaxed)
        <<"\nphaseCapacityRejects="<<state.capacityRejects.load(std::memory_order_relaxed)
        <<"\nphaseLedgerCapacity=8192\nphaseSemantics=one_early_next_refresh_then_original_reset_zero\n";return text.str();
}

AnimationWorkRow* CurrentAnimationWorkRow() { return s_animationWorkRow; }
uint32_t AnimationPoseShareRate() { return Capture().shareRate; }
bool AnimationCompatibilityBatchRequested() { return Capture().batchCompatibility; }
bool AnimationGpuTraceRequested() { return !Capture().path.empty(); }
void AssignAnimationSkinningPassOrdinal(AnimationSkinningCpuRow& row)
{
    auto& capture=Capture();
    std::lock_guard lock(capture.mutex);
    /// @note Bounded diagnostic state retains only integer provenance; no scene/resource pointer or buffer lifetime is extended.
    for(auto& state:capture.skinningOrdinals) {
        if(state.resourceInstanceId!=0 && state.resourceInstanceId!=row.resourceInstanceId)continue;
        if(state.resourceInstanceId!=row.resourceInstanceId || state.resourceEpoch!=row.resourceEpoch || state.resourceFrame!=row.resourceFrame)
            state={row.resourceInstanceId,row.resourceEpoch,row.resourceFrame,0};
        row.passOrdinalWithinResourceFrame=++state.ordinal;
        return;
    }
}

void CaptureAnimationSkinningCpuWork(const AnimationSkinningCpuRow& row)
{
    auto& capture=Capture();if(capture.path.empty())return;
    std::lock_guard lock(capture.mutex);
    bool found=false;
    for(uint32_t index=0;index<capture.cpuCount;++index) {
        const auto& cpu=capture.cpu[index];
        if(cpu.MatchesCompleteSource(row.resourceFrame,row.sceneGeneration,row.resourceEpoch)){found=true;break;}
    }
    if(!found || capture.skinCount>=capture.skin.size())return;
    capture.skin[capture.skinCount++]=row;
    if(capture.cpuCount!=capture.samples || row.resourceFrame!=capture.cpu[capture.cpuCount-1].resourceFrame)return;
    std::ostringstream text;
    text<<"resourceFrame,sceneGeneration,resourceEpoch,totalNs,profilerBeginViewSyncNs,boneReplayCpuSubmitNs,vertexSkinningCpuSubmitNs"
          ",resourceInstanceId,passOrdinalWithinResourceFrame,renderersVisited,renderersEligible,alreadyCurrentReuse,requestedDispatches,requestedVertexCount"
          ",sourceCacheHits,sourceCacheMisses,sourceCreateAttempts,sourceCreateSuccesses,sourceReleaseCalls,sourceCopyBytes"
          ",outputCreateAttempts,outputCreateSuccesses,outputReleaseCalls,outputCapacityRejects,bonePaletteUploadCalls,bonePaletteUploadBytes"
          ",backendRowPoolCreateAttempts,backendRowPoolCreateSuccesses,backendRowPoolReuses,backendDispatchAttempts,backendDispatchSuccesses,backendDispatchedVertexCount"
          ",signatureVectors,signatureBytes,signatureCapacityBytes,signatureAllocationLowerBound"
          ",signatureBuildNs,signatureGrowthPushNs,signatureCompareNs,signaturePublicationMoveNs,signatureReserveNs"
          ",signatureBuildCalls,signatureGrowthPushCalls,signatureCompareCalls,signaturePublicationMoveCalls,signatureReserveCalls"
          ",signaturePayloadAllocationCalls,signaturePayloadAllocationBytes,signatureGrowthRelocatedLogicalBytes"
          ",signatureClockPairCount,signatureClockProbeNs,unattributedSkinningNs"
          ",signatureAssignNs,signatureAssignCalls,signatureAssignCopyLogicalBytes,signatureClearNs,signatureClearCalls"
          ",signatureDestinationPayloadAllocationCalls,signatureDestinationPayloadAllocationBytes"
          ",signatureScratchAcquireNs,signatureScratchAcquireCalls"
          ",signatureScratchStorageAllocationCalls,signatureScratchStorageAllocationBytes"
          ",signatureScratchPoolPointerAllocationCalls,signatureScratchPoolPointerAllocationBytes"
          ",signatureScratchRetainedPayloadBytes,signatureScratchRetainedStorageBytes,signatureScratchRetainedPointerPayloadBytes,signatureScratchSlotsAtPassEnd"
          ",demandScopeActive,sceneObjectCount,sceneAnimatorComponentCount,sceneSkinnedRendererComponentCount"
          ",activeNativeScriptEntries,activeManagedScriptEntries,activeReflectionProbes,activeReflectionSceneProbes,activeLightProbeVolumes,activeLightProbeBakeConsumers,activeInstancedComponents"
          ",demandObservedRenderers,provisionalColorExcludedRenderers,conservativeNonColorRequiredRenderers,provisionalNoKnownConsumerRenderers"
          ",legacyEnteredRenderers,renderersWithOutputRequests,nativePublishedRenderers";
    for(const char* name:skinning_demand_detail::REASON_NAMES)text<<",demandReason_"<<name;
    text<<",demandClassifierVersion";
    for(const char* name:skinning_demand_detail::INPUT_NAMES)text<<",inputRenderers_"<<name;
    text<<",sourceObservedSlots,sourceBasePathSlots,sourceMorphPathSlots,sourceDiffersFromBaseSlots"
          ",sourceLookupNonNullBasePathSlots,sourceLookupNonNullMorphPathSlots,sourceLookupNullSlots,sourceNativeVersionZeroSlots,boneBakeQueuedReplayRows\n";
    for(uint32_t index=0;index<capture.skinCount;++index){const auto& entry=capture.skin[index];
        text<<entry.resourceFrame<<','<<entry.sceneGeneration<<','<<entry.resourceEpoch<<','<<entry.totalNs<<','<<entry.profilerSyncNs<<','<<entry.replaySubmitNs<<','<<entry.vertexSubmitNs
            <<','<<entry.resourceInstanceId<<','<<entry.passOrdinalWithinResourceFrame<<','<<entry.renderersVisited<<','<<entry.renderersEligible<<','<<entry.alreadyCurrentReuse
            <<','<<entry.requestedDispatches<<','<<entry.requestedVertexCount<<','<<entry.sourceCacheHits<<','<<entry.sourceCacheMisses<<','<<entry.sourceCreateAttempts
            <<','<<entry.sourceCreateSuccesses<<','<<entry.sourceReleaseCalls<<','<<entry.sourceCopyBytes<<','<<entry.outputCreateAttempts<<','<<entry.outputCreateSuccesses
            <<','<<entry.outputReleaseCalls<<','<<entry.outputCapacityRejects<<','<<entry.requests.bonePaletteUploadCalls<<','<<entry.requests.bonePaletteUploadBytes
            <<','<<entry.requests.backendRowPoolCreateAttempts<<','<<entry.requests.backendRowPoolCreateSuccesses<<','<<entry.requests.backendRowPoolReuses
            <<','<<entry.requests.backendDispatchAttempts<<','<<entry.requests.backendDispatchSuccesses<<','<<entry.requests.backendDispatchedVertexCount
            <<','<<entry.signatureVectors<<','<<entry.signatureBytes<<','<<entry.signatureCapacityBytes<<','<<entry.signatureAllocationLowerBound
            <<','<<entry.signatureBuildNs<<','<<entry.signatureGrowthPushNs<<','<<entry.signatureCompareNs<<','<<entry.signaturePublicationMoveNs<<','<<entry.signatureReserveNs
            <<','<<entry.signatureBuildCalls<<','<<entry.signatureGrowthPushCalls<<','<<entry.signatureCompareCalls<<','<<entry.signaturePublicationMoveCalls<<','<<entry.signatureReserveCalls
            <<','<<entry.signaturePayloadAllocationCalls<<','<<entry.signaturePayloadAllocationBytes<<','<<entry.signatureGrowthRelocatedLogicalBytes
            <<','<<entry.signatureClockPairCount<<','<<entry.signatureClockProbeNs<<','<<entry.unattributedSkinningNs
            <<','<<entry.signatureAssignNs<<','<<entry.signatureAssignCalls<<','<<entry.signatureAssignCopyLogicalBytes<<','<<entry.signatureClearNs<<','<<entry.signatureClearCalls
            <<','<<entry.signatureDestinationPayloadAllocationCalls<<','<<entry.signatureDestinationPayloadAllocationBytes
            <<','<<entry.signatureScratchAcquireNs<<','<<entry.signatureScratchAcquireCalls
            <<','<<entry.signatureScratchStorageAllocationCalls<<','<<entry.signatureScratchStorageAllocationBytes
            <<','<<entry.signatureScratchPoolPointerAllocationCalls<<','<<entry.signatureScratchPoolPointerAllocationBytes
            <<','<<entry.signatureScratchRetainedPayloadBytes<<','<<entry.signatureScratchRetainedStorageBytes<<','<<entry.signatureScratchRetainedPointerPayloadBytes<<','<<entry.signatureScratchSlotsAtPassEnd
            <<','<<entry.demandScopeActive<<','<<entry.sceneObjectCount<<','<<entry.sceneAnimatorComponentCount<<','<<entry.sceneSkinnedRendererComponentCount
            <<','<<entry.activeNativeScriptEntries<<','<<entry.activeManagedScriptEntries<<','<<entry.activeReflectionProbes<<','<<entry.activeReflectionSceneProbes<<','<<entry.activeLightProbeVolumes<<','<<entry.activeLightProbeBakeConsumers<<','<<entry.activeInstancedComponents
            <<','<<entry.demandObservedRenderers<<','<<entry.provisionalColorExcludedRenderers<<','<<entry.conservativeNonColorRequiredRenderers<<','<<entry.provisionalNoKnownConsumerRenderers
            <<','<<entry.legacyEnteredRenderers<<','<<entry.renderersWithOutputRequests<<','<<entry.nativePublishedRenderers;
        for(uint64_t count:entry.outputDemandReasonRenderers)text<<','<<count;
        text<<','<<entry.demandClassifierVersion;
        for(uint64_t count:entry.inputObservedRenderers)text<<','<<count;
        text<<','<<entry.sourceObservedSlots<<','<<entry.sourceBasePathSlots<<','<<entry.sourceMorphPathSlots<<','<<entry.sourceDiffersFromBaseSlots
            <<','<<entry.sourceLookupNonNullBasePathSlots<<','<<entry.sourceLookupNonNullMorphPathSlots<<','<<entry.sourceLookupNullSlots
            <<','<<entry.sourceNativeVersionZeroSlots<<','<<entry.boneBakeQueuedReplayRows<<'\n';}
    if(!util::FileSystem::WriteTextAtomic(capture.path+".skin-cpu.csv",text.str()))FBZZ_LOG_WARN("AnimationWorkTrace: unable to save skinning CPU capture");
}
namespace animation_actor_detail {
struct ActorFrameRow {
    uint64_t resourceFrame=0,sceneGeneration=0,resourceEpoch=0,resourceInstance=0,timeFrame=0,updateOrdinal=0;
    uint64_t endResourceFrame=0,endSceneGeneration=0,endResourceEpoch=0,endResourceInstance=0;
    uint64_t observedActors=0,pendingActors=0,storedActors=0,actorRowsDropped=0,completeActors=0,missingAfterRun=0,missingAtFrameEnd=0,duplicateActors=0;
    uint64_t animatorEntityPopulation=0;
    std::array<uint64_t,4> excludedActors{};
    uint64_t refreshActors=0,heldActors=0,dueActors=0,forcedRefreshActors=0,sameDesiredAvailable=0,sameDesiredActors=0;
    uint64_t consecutiveHeldAvailable=0,consecutiveHeldActors=0,heldSameDesiredActors=0,compareArgumentBytes=0;
    bool sourceFinalized=false,sourceComplete=false;
};
struct HistoryEntry { ActorKey key; HeldHistory history; uint64_t pendingOrdinal=0; uint32_t pendingIndex=0; };
struct ActorCapture {
    static constexpr uint32_t HASH_CAPACITY=HISTORY_CAPACITY*2;
    std::atomic_flag owned=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> rejectedOverlapFrames{0};
    std::string path;
    uint32_t warmup=0,samples=0,warmed=0,sampleCount=0,pendingCount=0,actorCount=0,historyCount=0;
    uint64_t ordinal=0,lastFrame=UINT64_MAX,lastTimeFrame=UINT64_MAX,lastScene=0,lastEpoch=0,lastInstance=0;
    uint64_t duplicateFrames=0,sourceDiscontinuities=0,frameGaps=0,pendingOverflow=0,actorOverflow=0,historyOverflow=0;
    uint64_t sampledObservedActors=0,warmObservedActors=0,incompleteActors=0,incompleteFrames=0;
    uint32_t pendingHighWater=0,historyHighWater=0,actorHighWater=0;
    bool sampled=false,written=false,frameOpen=false;
    uint64_t overlapAtBegin=0;
    ActorFrameRow current;
    std::array<ActorRow,HISTORY_CAPACITY> pending{};
    /// @note Runtime allocation preserves the fixed row cap without a giant compile-time aggregate.
    std::unique_ptr<ActorRow[]> actors=std::make_unique<ActorRow[]>(ACTOR_CAPACITY);
    std::array<ActorFrameRow,FRAME_CAPACITY> frames{};
    std::array<HistoryEntry,HISTORY_CAPACITY> histories{};
    std::array<uint32_t,HASH_CAPACITY> slots{};
    void CutHistory() { slots.fill(0); historyCount=0; }
    uint32_t FindHistory(ActorRow& row)
    {
        const auto& k=row.key;
        uint64_t hash=k.componentLifetime ^ (uint64_t{k.entityIndex}<<32) ^ k.entityGeneration;
        hash ^= k.sceneGeneration*0x9e3779b97f4a7c15ULL ^ k.resourceInstance ^ k.resourceEpoch;
        hash ^= hash>>33; hash*=0xff51afd7ed558ccdULL; hash^=hash>>33;
        uint32_t slot=static_cast<uint32_t>(hash)&(HASH_CAPACITY-1);
        for(uint32_t probe=0;probe<HASH_CAPACITY;++probe,slot=(slot+1)&(HASH_CAPACITY-1)) {
            const uint32_t entry=slots[slot];
            if(entry && SameActorLifetime(histories[entry-1].key,k))return entry-1;
            if(!entry) {
                uint32_t index=0;
                if(!ClaimBoundedSlot(historyCount,HISTORY_CAPACITY,index)) { ++historyOverflow; return NO_HISTORY; }
                histories[index]={}; histories[index].key=k; slots[slot]=index+1;
                historyHighWater=(std::max)(historyHighWater,historyCount); return index;
            }
        }
        ++historyOverflow; return NO_HISTORY;
    }
};
}
namespace {
thread_local animation_actor_detail::ActorRow* s_animationActorRow=nullptr;
struct ActorCaptureConfig {
    std::string path=ReadAnimationTraceEnv("FBZZ_ANIMATION_ACTOR_TRACE_OUTPUT");
    uint32_t warmup=0,samples=0;
    ActorCaptureConfig() {
        if(path.empty())return;
        warmup=ReadAnimationTraceCount("FBZZ_ANIMATION_ACTOR_TRACE_WARMUP",120,100000);
        samples=ReadAnimationTraceCount("FBZZ_ANIMATION_ACTOR_TRACE_SAMPLES",120,animation_actor_detail::FRAME_CAPACITY);
    }
};
/// @note Nonblocking capture ownership also rejects same-thread recursion without touching a nonrecursive mutex.
struct ActorCaptureLease {
    animation_actor_detail::ActorCapture& capture;
    bool acquired;
    explicit ActorCaptureLease(animation_actor_detail::ActorCapture& c):capture(c),acquired(animation_actor_detail::TryClaimCapture(c.owned)) {}
    ~ActorCaptureLease() { if(acquired)animation_actor_detail::ReleaseCapture(capture.owned); }
};
ActorCaptureConfig& ActorConfig() { static ActorCaptureConfig config; return config; }
animation_actor_detail::ActorCapture& ActorCaptureStorage()
{
    static auto capture=std::make_unique<animation_actor_detail::ActorCapture>();
    return *capture;
}
void WriteActorCapture(animation_actor_detail::ActorCapture& c)
{
    using namespace animation_actor_detail;
    std::ostringstream actors; actors.precision(9);
    actors << "schema,resourceFrame,sceneGeneration,resourceEpoch,resourceInstance,timeFrame,updateOrdinal,entityIndex,entityGeneration,componentLifetime,previousHandleId,previousHandleGeneration,ctxDt,timeDelta,unscaledDelta,effectiveScale,timeScale,vfxScale,localScale,clampedLocalScale,actorDt,snapshotObserved,snapshotGuardPassed,previousValidBeforeSnapshot,currentCountBeforeSnapshot,previousCountBeforeSnapshot,currentClampedCount,previousClampedCount,desiredEqualityAvailable,desiredEqual,compareArgumentBytes,lodDecisionAvailable,safeLod,effectiveInterval,elapsedBeforeAdd,elapsedAtDecision,dueArgument,dueReasons,actualDecisionAvailable,actualRefresh,forceReasonsAvailable,forceReasons,afterRunObserved,afterRunIdentityValid,elapsedAfterRun,frameEndIdentityValid,sourceComplete,duplicateActor,actorComplete,previousObservationContinuous,previousHeldAvailable,previousHeld,heldSuffixLowerBound,heldStreakExactAvailable,consecutiveHeldAtLeast2Available,consecutiveHeldAtLeast2,cpuSameDesiredPotentialAvailable,cpuSameDesiredPotential,previousHistoryInvalidatedAfterSnapshot,lodBandAvailable,selectedLodBand,nearestCameraDistanceSquared,posePathStatus,actualNativeSkipAvailable,actualNativeSkip,nativePublicationProofAvailable\n";
    for(uint32_t index=0;index<c.actorCount;++index) {
        const auto& r=c.actors[index]; const auto& k=r.key;
        actors<<1<<','<<r.resourceFrame<<','<<k.sceneGeneration<<','<<k.resourceEpoch<<','<<k.resourceInstance<<','<<r.timeFrame<<','<<r.updateOrdinal
            <<','<<k.entityIndex<<','<<k.entityGeneration<<','<<k.componentLifetime<<','<<k.previousHandleId<<','<<k.previousHandleGeneration
            <<','<<r.ctxDt<<','<<r.timeDelta<<','<<r.unscaledDelta<<','<<r.effectiveScale<<','<<r.timeScale<<','<<r.vfxScale
            <<','<<r.localScale<<','<<r.clampedLocalScale<<','<<r.actorDt<<','<<r.snapshotObserved<<','<<r.snapshotGuardPassed<<','<<r.previousValidBeforeSnapshot
            <<','<<r.currentCountBeforeSnapshot<<','<<r.previousCountBeforeSnapshot<<','<<r.currentClampedCount<<','<<r.previousClampedCount
            <<','<<r.desiredEqualityAvailable<<','<<r.desiredEqual<<','<<r.compareArgumentBytes<<','<<r.lodDecisionAvailable<<','<<r.safeLod
            <<','<<r.effectiveInterval<<','<<r.elapsedBeforeAdd<<','<<r.elapsedAtDecision<<','<<r.dueArgument<<','<<r.dueReasons
            <<','<<r.actualDecisionAvailable<<','<<r.actualRefresh<<','<<r.forceReasonsAvailable<<','<<r.forceReasons
            <<','<<r.afterRunObserved<<','<<r.afterRunIdentityValid<<','<<r.elapsedAfterRun<<','<<r.frameEndIdentityValid<<','<<r.sourceComplete
            <<','<<r.duplicateActor<<','<<r.Complete()<<','<<r.previousObservationContinuous<<','<<r.previousHeldAvailable<<','<<r.previousHeld
            <<','<<r.heldSuffixLowerBound<<','<<r.heldStreakExactAvailable<<','<<r.consecutiveHeldAtLeast2Available<<','<<r.consecutiveHeldAtLeast2
            <<','<<r.cpuSameDesiredPotentialAvailable<<','<<r.cpuSameDesiredPotential
            <<','<<r.previousHistoryInvalidatedAfterSnapshot<<','<<r.lodBandAvailable<<','<<r.selectedLodBand<<','<<r.nearestCameraDistanceSquared<<','<<r.posePathStatus<<",0,,0\n";
    }
    std::ostringstream frames;
    frames << "schema,resourceFrame,sceneGeneration,resourceEpoch,resourceInstance,timeFrame,updateOrdinal,endResourceFrame,endSceneGeneration,endResourceEpoch,endResourceInstance,sourceFinalized,sourceComplete,animatorEntityPopulation,excludedMissingObject,excludedInactiveObject,excludedMissingAnimator,excludedDisabledAnimator,observedActors,pendingActors,storedActors,actorRowsDropped,completeActors,missingAfterRun,missingAtFrameEnd,duplicateActors,refreshActors,heldActors,dueActors,forcedRefreshActors,sameDesiredAvailable,sameDesiredActors,consecutiveHeldAvailable,consecutiveHeldActors,heldSameDesiredActors,compareArgumentBytes,actualNativeSkipAvailable,actualNativeSkip,nativePublicationProofAvailable\n";
    for(uint32_t index=0;index<c.sampleCount;++index) {
        const auto& f=c.frames[index];
        frames<<1<<','<<f.resourceFrame<<','<<f.sceneGeneration<<','<<f.resourceEpoch<<','<<f.resourceInstance<<','<<f.timeFrame<<','<<f.updateOrdinal
            <<','<<f.endResourceFrame<<','<<f.endSceneGeneration<<','<<f.endResourceEpoch<<','<<f.endResourceInstance<<','<<f.sourceFinalized<<','<<f.sourceComplete
            <<','<<f.animatorEntityPopulation<<','<<f.excludedActors[0]<<','<<f.excludedActors[1]<<','<<f.excludedActors[2]<<','<<f.excludedActors[3]
            <<','<<f.observedActors<<','<<f.pendingActors<<','<<f.storedActors<<','<<f.actorRowsDropped<<','<<f.completeActors<<','<<f.missingAfterRun<<','<<f.missingAtFrameEnd<<','<<f.duplicateActors
            <<','<<f.refreshActors<<','<<f.heldActors<<','<<f.dueActors<<','<<f.forcedRefreshActors<<','<<f.sameDesiredAvailable<<','<<f.sameDesiredActors
            <<','<<f.consecutiveHeldAvailable<<','<<f.consecutiveHeldActors<<','<<f.heldSameDesiredActors<<','<<f.compareArgumentBytes<<",0,,0\n";
    }
    const bool actorsSaved=util::FileSystem::WriteTextAtomic(c.path+".actor.csv",actors.str());
    const bool framesSaved=util::FileSystem::WriteTextAtomic(c.path+".frame.csv",frames.str());
    const bool complete=c.sampleCount==c.samples && !c.pendingOverflow && !c.actorOverflow && !c.historyOverflow
        && !c.duplicateFrames && !c.rejectedOverlapFrames.load() && !c.incompleteActors && !c.incompleteFrames && actorsSaved && framesSaved;
    std::ostringstream summary;
    summary<<"{\n\"schema\":1,\"complete\":"<<(complete?"true":"false")
        <<",\"requestedWarmup\":"<<c.warmup<<",\"completedWarmup\":"<<c.warmed<<",\"requestedSamples\":"<<c.samples<<",\"completedSamples\":"<<c.sampleCount
        <<",\"sampledObservedActors\":"<<c.sampledObservedActors<<",\"warmObservedActors\":"<<c.warmObservedActors<<",\"storedActorRows\":"<<c.actorCount
        <<",\"incompleteActorRows\":"<<c.incompleteActors<<",\"incompleteFrames\":"<<c.incompleteFrames<<",\"duplicateFrames\":"<<c.duplicateFrames
        <<",\"sourceDiscontinuities\":"<<c.sourceDiscontinuities<<",\"frameGaps\":"<<c.frameGaps
        <<",\"rejectedOverlapFrames\":"<<c.rejectedOverlapFrames.load()
        <<",\"pendingOverflow\":"<<c.pendingOverflow<<",\"actorOverflow\":"<<c.actorOverflow<<",\"historyOverflow\":"<<c.historyOverflow
        <<",\"pendingCapacity\":"<<HISTORY_CAPACITY<<",\"historyCapacity\":"<<HISTORY_CAPACITY<<",\"actorCapacity\":"<<ACTOR_CAPACITY<<",\"frameCapacity\":"<<FRAME_CAPACITY
        <<",\"pendingHighWater\":"<<c.pendingHighWater<<",\"historyHighWater\":"<<c.historyHighWater<<",\"actorHighWater\":"<<c.actorHighWater
        <<",\"actorRowBytes\":"<<sizeof(ActorRow)<<",\"captureStorageBytes\":"<<(sizeof(ActorCapture)+uint64_t{ACTOR_CAPACITY}*sizeof(ActorRow))
        <<",\"actorCsvSaved\":"<<(actorsSaved?"true":"false")<<",\"frameCsvSaved\":"<<(framesSaved?"true":"false")
        <<",\"actualNativeSkipAvailable\":false,\"actualNativeSkip\":null,\"nativePublicationProofAvailable\":false"
           ",\"actualGpuBytesAvailable\":false,\"actualGpuBytes\":null,\"comparisonTimingAvailable\":false,\"actorRowCoverage\":\"active enabled animators; other loop entries have frame exclusion counts\",\"heldSuffixUnit\":\"unique contiguous animation updates\"\n}\n";
    if(!util::FileSystem::WriteTextAtomic(c.path+".summary.json",summary.str()) || !actorsSaved || !framesSaved)
        FBZZ_LOG_WARN("AnimationActorTrace: unable to save complete actor diagnostic capture");
    c.written=true;
}
}
bool AnimationActorTraceRequested() { const auto& c=ActorConfig(); return !c.path.empty() && c.samples!=0; }
animation_actor_detail::ActorRow* CurrentAnimationActorRow() { return s_animationActorRow; }
AnimationActorScope::AnimationActorScope(animation_actor_detail::ActorRow* row)
{
    if(!row && !s_animationActorRow)return;
    m_previous=s_animationActorRow; s_animationActorRow=row; m_active=true;
}
AnimationActorScope::~AnimationActorScope() { if(m_active)s_animationActorRow=m_previous; }
AnimationActorFrame::AnimationActorFrame(bool simulating,uint64_t frame,uint64_t generation,uint64_t epoch,uint64_t instance)
{
    if(!simulating || !AnimationActorTraceRequested())return;
    auto& c=ActorCaptureStorage(); ActorCaptureLease lease(c);
    if(!lease.acquired) { c.rejectedOverlapFrames.fetch_add(1); return; }
    const auto& config=ActorConfig();
    if(c.written || c.sampleCount>=config.samples)return;
    if(c.frameOpen) { ++c.duplicateFrames; c.CutHistory(); return; }
    c.path=config.path; c.warmup=config.warmup; c.samples=config.samples;
    if(c.lastFrame==frame && c.lastScene==generation && c.lastEpoch==epoch && c.lastInstance==instance) {
        ++c.duplicateFrames; c.CutHistory(); return;
    }
    if(c.ordinal && (c.lastScene!=generation || c.lastEpoch!=epoch || c.lastInstance!=instance)) { ++c.sourceDiscontinuities; c.CutHistory(); }
    else if(c.ordinal && (c.lastFrame==UINT64_MAX || c.lastTimeFrame==UINT64_MAX || frame!=c.lastFrame+1 || Time::frameCount!=c.lastTimeFrame+1)) {
        ++c.frameGaps; c.CutHistory();
    }
    c.lastFrame=frame; c.lastScene=generation; c.lastEpoch=epoch; c.lastInstance=instance; c.lastTimeFrame=Time::frameCount;
    ++c.ordinal; c.overlapAtBegin=c.rejectedOverlapFrames.load(); c.pendingCount=0; c.current={}; c.current.resourceFrame=frame; c.current.sceneGeneration=generation;
    c.current.resourceEpoch=epoch; c.current.resourceInstance=instance; c.current.timeFrame=Time::frameCount; c.current.updateOrdinal=c.ordinal;
    c.sampled=c.warmed>=c.warmup; c.frameOpen=true; m_capture=&c; lease.acquired=false;
}
void AnimationActorFrame::SetPopulation(uint64_t population) { if(m_capture)m_capture->current.animatorEntityPopulation=population; }
void AnimationActorFrame::CountExcluded(uint32_t reason) { if(m_capture && reason<4)++m_capture->current.excludedActors[reason]; }
animation_actor_detail::ActorRow* AnimationActorFrame::BeginActor(uint32_t index,uint32_t generation,uint64_t lifetime,float dt,float localScale)
{
    using namespace animation_actor_detail;
    if(!m_capture)return nullptr;
    auto& c=*m_capture; ++c.current.observedActors;
    if(c.sampled)++c.sampledObservedActors; else ++c.warmObservedActors;
    uint32_t slot=0;
    if(!ClaimBoundedSlot(c.pendingCount,HISTORY_CAPACITY,slot)) { ++c.pendingOverflow; return nullptr; }
    c.pendingHighWater=(std::max)(c.pendingHighWater,c.pendingCount);
    auto& r=c.pending[slot]; r={}; r.key={c.current.sceneGeneration,c.current.resourceEpoch,c.current.resourceInstance,lifetime,index,generation,0,0};
    r.resourceFrame=c.current.resourceFrame; r.timeFrame=c.current.timeFrame; r.updateOrdinal=c.ordinal;
    r.ctxDt=dt; r.timeDelta=Time::deltaTime; r.unscaledDelta=Time::unscaledDeltaTime; r.effectiveScale=Time::GetEffectiveTimeScale();
    r.timeScale=Time::timeScale; r.vfxScale=Time::vfxTimeScale; r.localScale=localScale;
    r.clampedLocalScale=std::clamp(localScale,0.0f,8.0f); r.actorDt=dt*r.clampedLocalScale;
    return &r;
}
void AnimationActorFrame::FinishActor(animation_actor_detail::ActorRow* row,Scene& scene,uint64_t frame,uint64_t generation,uint64_t epoch,uint64_t instance)
{
    if(!m_capture || !row)return;
    auto& r=*row; r.afterRunObserved=true;
    if(frame!=r.resourceFrame || generation!=r.key.sceneGeneration || epoch!=r.key.resourceEpoch || instance!=r.key.resourceInstance)return;
    const EntityID entity{r.key.entityIndex,r.key.entityGeneration};
    if(!r.key.componentLifetime || scene.GetComponentLifetime<AnimatorComponent>(entity)!=r.key.componentLifetime)return;
    const auto* current=scene.GetComponent<AnimatorComponent>(entity);
    if(!current)return;
    r.afterRunIdentityValid=true; r.elapsedAfterRun=current->poseUpdateElapsed;
    if(r.actualDecisionAvailable && r.actualRefresh!=current->poseEvaluatedThisFrame)r.afterRunIdentityValid=false;
}
void AnimationActorFrame::FinalizeSource(Scene& scene,uint64_t frame,uint64_t generation,uint64_t epoch,uint64_t instance)
{
    if(!m_capture)return;
    auto& c=*m_capture; auto& f=c.current;
    const bool finalizedBefore=f.sourceFinalized;
    const bool matching=f.resourceFrame==frame && f.sceneGeneration==generation && f.resourceEpoch==epoch && f.resourceInstance==instance
        && c.rejectedOverlapFrames.load()==c.overlapAtBegin;
    f.sourceComplete=matching && (!finalizedBefore || f.sourceComplete); f.sourceFinalized=true;
    f.endResourceFrame=frame; f.endSceneGeneration=generation; f.endResourceEpoch=epoch; f.endResourceInstance=instance;
    for(uint32_t index=0;index<c.pendingCount;++index) {
        auto& r=c.pending[index]; r.sourceComplete=f.sourceComplete;
        const EntityID entity{r.key.entityIndex,r.key.entityGeneration};
        const bool valid=f.sourceComplete && r.key.componentLifetime!=0 && scene.GetComponentLifetime<AnimatorComponent>(entity)==r.key.componentLifetime;
        r.frameEndIdentityValid=valid && (!finalizedBefore || r.frameEndIdentityValid);
    }
}
AnimationActorFrame::~AnimationActorFrame()
{
    using namespace animation_actor_detail;
    if(!m_capture)return;
    auto& c=*m_capture; auto& f=c.current;
    if(c.rejectedOverlapFrames.load()!=c.overlapAtBegin) {
        f.sourceComplete=false;
        for(uint32_t index=0;index<c.pendingCount;++index)c.pending[index].sourceComplete=false;
    }
    f.pendingActors=c.pendingCount;
    for(uint32_t index=0;index<c.pendingCount;++index) {
        auto& r=c.pending[index];
        r.historyIndex=c.FindHistory(r);
        if(r.historyIndex!=NO_HISTORY) {
            auto& h=c.histories[r.historyIndex];
            if(h.pendingOrdinal==c.ordinal) {
                r.duplicateActor=true; c.pending[h.pendingIndex].duplicateActor=true; h.history.valid=false;
            } else { h.pendingOrdinal=c.ordinal; h.pendingIndex=index; }
        }
    }
    for(uint32_t index=0;index<c.pendingCount;++index) {
        auto& r=c.pending[index];
        if(r.historyIndex!=NO_HISTORY)c.histories[r.historyIndex].history.Observe(r);
        if(r.Complete())++f.completeActors;
        else { ++c.incompleteActors; if(!r.afterRunObserved || !r.afterRunIdentityValid)++f.missingAfterRun; if(!r.frameEndIdentityValid)++f.missingAtFrameEnd; }
        if(r.duplicateActor)++f.duplicateActors;
        if(r.Complete() && r.actualDecisionAvailable) { if(r.actualRefresh)++f.refreshActors; else ++f.heldActors; }
        if(r.Complete() && r.lodDecisionAvailable && r.dueArgument)++f.dueActors;
        if(r.Complete() && r.actualRefresh && r.forceReasonsAvailable && r.forceReasons)++f.forcedRefreshActors;
        if(r.cpuSameDesiredPotentialAvailable) { ++f.sameDesiredAvailable; if(r.cpuSameDesiredPotential)++f.sameDesiredActors; }
        if(r.consecutiveHeldAtLeast2Available) { ++f.consecutiveHeldAvailable; if(r.consecutiveHeldAtLeast2)++f.consecutiveHeldActors; }
        if(r.consecutiveHeldAtLeast2 && r.cpuSameDesiredPotential)++f.heldSameDesiredActors;
        f.compareArgumentBytes+=r.compareArgumentBytes;
        if(c.sampled) {
            uint32_t slot=0;
            if(ClaimBoundedSlot(c.actorCount,ACTOR_CAPACITY,slot)) { c.actors[slot]=r; ++f.storedActors; }
            else { ++c.actorOverflow; ++f.actorRowsDropped; }
        }
    }
    c.actorHighWater=(std::max)(c.actorHighWater,c.actorCount);
    uint64_t accounted=f.observedActors; for(uint64_t excluded:f.excludedActors)accounted+=excluded;
    if(!f.sourceComplete || f.completeActors!=f.observedActors || accounted!=f.animatorEntityPopulation)++c.incompleteFrames;
    if(c.sampled)c.frames[c.sampleCount++]=f; else ++c.warmed;
    c.frameOpen=false;
    if(c.sampleCount==c.samples && !c.written)WriteActorCapture(c);
    ReleaseCapture(c.owned);
}

void CountAnimationCompatibilityUpdate(uint64_t bytes)
{
    if (auto* row = CurrentAnimationWorkRow()) { ++row->cbUpdates; row->cbApiBytes += bytes; }
}
AnimationWorkFrame::AnimationWorkFrame(bool simulating, uint64_t frame, uint64_t generation, uint64_t epoch)
{
    auto& capture = Capture();
    if (!simulating || capture.path.empty() || capture.samples == 0) return;
    std::lock_guard lock(capture.mutex);
    if (capture.cpuCount >= capture.samples) return;
    if (capture.lastStartedFrame == frame) { ++capture.duplicateCpuFrames; return; }
    capture.lastStartedFrame = frame;
    /// @note Independent wall interval between unique animation-update beginnings; no inclusive scope sum or lockstep delta.
    const uint64_t beginNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t intervalNs = capture.lastBeginNs && capture.lastBeginGeneration == generation && capture.lastBeginEpoch == epoch ? beginNs-capture.lastBeginNs : 0;
    capture.lastBeginNs = beginNs; capture.lastBeginGeneration = generation; capture.lastBeginEpoch = epoch;
    if (capture.warmed < capture.warmup) { ++capture.warmed; return; }
    m_row.BeginSource(frame, generation, epoch);
    m_row.poseCountersAvailable = true; m_row.legacyBoneWritesAvailable = true; m_row.paletteApiCountersAvailable = true;
    m_row.wallBeginIntervalNs = intervalNs;
    m_previous = s_animationWorkRow; s_animationWorkRow = &m_row; m_active = true;
}
AnimationWorkFrame::~AnimationWorkFrame()
{
    if (!m_active) return;
    s_animationWorkRow = m_previous;
    auto& capture = Capture(); std::lock_guard lock(capture.mutex);
    if (capture.cpuCount < capture.samples) capture.cpu[capture.cpuCount++] = m_row;
    if (capture.cpuCount == capture.samples && !capture.cpuWritten) WriteAnimationCpuCapture(capture);
}
void AnimationWorkFrame::FinalizeSource(uint64_t frame, uint64_t generation, uint64_t epoch)
{
    if (m_active) m_row.FinalizeSource(frame, generation, epoch);
}
/// @note This reads the renderer's completed raw timestamp snapshot, bypassing PerformanceProfiler and game-view filtering.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing Completed queue timestamps use the queue's timestamp frequency.
void CaptureAnimationGpuWork(const renderer::GpuProfilerSnapshot& snapshot)
{
    auto& capture = Capture();
    if (capture.path.empty() || !snapshot.supported || !snapshot.available) return;
    std::lock_guard lock(capture.mutex);
    if (capture.gpuWritten || capture.cpuCount == 0) return;
    for (const auto& pass : snapshot.passes) {
        uint32_t part = 0;
        if (pass.name == "BakedBoneReplay") part = 1;
        else if (pass.name == "VertexSkinningCompute") part = 2;
        else if (pass.name != "AnimationSkinningCompute") continue;
        if (!pass.available || !std::isfinite(pass.gpuMs) || pass.gpuMs < 0.0) continue;
        const auto& source = pass.metadata;
        bool cpuFrameFound = false;
        for (uint32_t index = 0; index < capture.cpuCount; ++index) {
            const auto& cpu = capture.cpu[index];
            if (cpu.MatchesCompleteSource(source.applicationFrameSerial, source.sceneGeneration, source.resourceEpoch)) { cpuFrameFound = true; break; }
        }
        if (!cpuFrameFound) continue;
        bool duplicate = false;
        for (uint32_t index = 0; index < capture.gpuCount; ++index) {
            const auto& row = capture.gpu[index];
            if (row.physicalFrame == pass.physicalFrameSerial && row.deviceEpoch == pass.deviceEpoch
                && row.resourceFrame == source.applicationFrameSerial && row.sourceScene == source.sceneGeneration
                && row.viewId == source.viewId && row.part == part) { duplicate = true; break; }
        }
        if (duplicate || capture.gpuCount >= capture.samples * 3) continue;
        capture.gpu[capture.gpuCount++] = {source.applicationFrameSerial, pass.physicalFrameSerial, pass.deviceEpoch,
            source.sceneGeneration, source.resourceEpoch, source.viewId, pass.gpuMs, snapshot.complete, snapshot.droppedPassCount, part};
        if (part == 2) ++capture.vertexGpuFrames;
    }
    if (capture.vertexGpuFrames == capture.samples) WriteAnimationGpuCapture(capture);
}
}
