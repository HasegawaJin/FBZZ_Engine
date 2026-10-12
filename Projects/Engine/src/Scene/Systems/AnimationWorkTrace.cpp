/// @file    AnimationWorkTrace.cpp
/// @brief   Fixed-size independent animation trace, serialized after the capture window.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include "AnimationWorkTrace.hpp"
#include <Core/Util/FileSystem.hpp>
#include <Core/Logger.hpp>
#include <Graphics/Renderer/GpuProfiler.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
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
    text << "resourceFrame,sceneGeneration,resourceEpoch,totalNs,previousCbNs,currentCbNs,stateMachineNs,clipSelectionNs,boneHierarchyNs,legacyFkNs,boneApplicationNs,propagationRebuildNs,bakedFkNs,sideEffectsBoundsNs,bakePreparationNs,batchSubmitNs,animators,boneMatrices,cbUpdates,cbApiBytes,legacyPoses,bakedCpuPoses,bakedGpuPoses,poseCacheHits,poseCacheMisses,cbBulkPackets,wallBeginIntervalNs\n";
    for (uint32_t index = 0; index < capture.cpuCount; ++index) {
        const auto& row = capture.cpu[index];
        text << row.resourceFrame << ',' << row.sceneGeneration << ',' << row.resourceEpoch;
        for (uint64_t elapsed : row.nanoseconds) text << ',' << elapsed;
        text << ',' << row.animators << ',' << row.boneMatrices << ',' << row.cbUpdates << ',' << row.cbApiBytes
             << ',' << row.legacyPoses << ',' << row.bakedCpuPoses << ',' << row.bakedGpuPoses
             << ',' << row.poseCacheHits << ',' << row.poseCacheMisses << ',' << row.cbBulkPackets << ',' << row.wallBeginIntervalNs << '\n';
    }
    if (!util::FileSystem::WriteTextAtomic(capture.path + ".cpu.csv", text.str()))
        FBZZ_LOG_WARN("AnimationWorkTrace: unable to save completed CPU capture");
    std::ostringstream metadata;
    metadata << "samples=" << capture.cpuCount << "\nwarmed=" << capture.warmed
             << "\nduplicateCpuFrames=" << capture.duplicateCpuFrames << "\nposeShareHz=" << capture.shareRate
             << "\nclock=steady_clock_nanoseconds\ncbBytes=ResourceManager_Update_arguments_not_GPU_submitted_bytes\n";
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
AnimationWorkRow* CurrentAnimationWorkRow() { return s_animationWorkRow; }
uint32_t AnimationPoseShareRate() { return Capture().shareRate; }
bool AnimationCompatibilityBatchRequested() { return Capture().batchCompatibility; }
bool AnimationGpuTraceRequested() { return !Capture().path.empty(); }
void CaptureAnimationSkinningCpuWork(const AnimationSkinningCpuRow& row)
{
    auto& capture=Capture();if(capture.path.empty())return;
    std::lock_guard lock(capture.mutex);
    bool found=false;
    for(uint32_t index=0;index<capture.cpuCount;++index) {
        const auto& cpu=capture.cpu[index];
        if(cpu.resourceFrame==row.resourceFrame && cpu.resourceEpoch==row.resourceEpoch && cpu.sceneGeneration==row.sceneGeneration){found=true;break;}
    }
    if(!found || capture.skinCount>=capture.skin.size())return;
    capture.skin[capture.skinCount++]=row;
    if(capture.cpuCount!=capture.samples || row.resourceFrame!=capture.cpu[capture.cpuCount-1].resourceFrame)return;
    std::ostringstream text;
    text<<"resourceFrame,sceneGeneration,resourceEpoch,totalNs,profilerBeginViewSyncNs,boneReplayCpuSubmitNs,vertexSkinningCpuSubmitNs\n";
    for(uint32_t index=0;index<capture.skinCount;++index){const auto& entry=capture.skin[index];
        text<<entry.resourceFrame<<','<<entry.sceneGeneration<<','<<entry.resourceEpoch<<','<<entry.totalNs<<','<<entry.profilerSyncNs<<','<<entry.replaySubmitNs<<','<<entry.vertexSubmitNs<<'\n';}
    if(!util::FileSystem::WriteTextAtomic(capture.path+".skin-cpu.csv",text.str()))FBZZ_LOG_WARN("AnimationWorkTrace: unable to save skinning CPU capture");
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
    m_row.resourceFrame = frame; m_row.sceneGeneration = generation; m_row.resourceEpoch = epoch;
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
    if (m_active) { m_row.resourceFrame = frame; m_row.sceneGeneration = generation; m_row.resourceEpoch = epoch; }
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
            if (source.applicationFrameSerial == cpu.resourceFrame && source.resourceEpoch == cpu.resourceEpoch
                && source.sceneGeneration == cpu.sceneGeneration) { cpuFrameFound = true; break; }
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
