/// @file    AnimationWorkTrace.hpp
/// @brief   Opt-in independent animation work timings and completed GPU timestamp capture.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <array>
#include <chrono>
#include <cstdint>

namespace fbzz::renderer { struct GpuProfilerSnapshot; }
namespace fbzz::scene {
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
};
/// @note One TLS row is active only within a simulating AnimatorSystem update; shared storage serializes completed rows.
AnimationWorkRow* CurrentAnimationWorkRow();
void CountAnimationCompatibilityUpdate(uint64_t bytes);
void CaptureAnimationGpuWork(const renderer::GpuProfilerSnapshot& snapshot);
bool AnimationGpuTraceRequested();
/// @note Experimental process opt-in; zero preserves continuous phase and the existing uncached pose path.
uint32_t AnimationPoseShareRate();
bool AnimationCompatibilityBatchRequested();
struct AnimationSkinningCpuRow {
    uint64_t resourceFrame=0,sceneGeneration=0,resourceEpoch=0;
    uint64_t totalNs=0,profilerSyncNs=0,replaySubmitNs=0,vertexSubmitNs=0;
};
void CaptureAnimationSkinningCpuWork(const AnimationSkinningCpuRow& row);
class AnimationSkinningCpuFrame {
public:
    AnimationSkinningCpuFrame(uint64_t frame,uint64_t scene,uint64_t epoch):m_enabled(AnimationGpuTraceRequested())
    { row.resourceFrame=frame;row.sceneGeneration=scene;row.resourceEpoch=epoch;m_start=Mark(); }
    ~AnimationSkinningCpuFrame(){if(m_enabled){row.totalNs=Elapsed(m_start);CaptureAnimationSkinningCpuWork(row);}}
    uint64_t Mark() const {return m_enabled?static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()):0;}
    uint64_t Elapsed(uint64_t start) const {return m_enabled?Mark()-start:0;}
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
    AnimationWorkFrame(bool simulating, uint64_t resourceFrame, uint64_t sceneGeneration, uint64_t resourceEpoch);
    ~AnimationWorkFrame();
    void FinalizeSource(uint64_t resourceFrame, uint64_t sceneGeneration, uint64_t resourceEpoch);
    AnimationWorkFrame(const AnimationWorkFrame&) = delete;
    AnimationWorkFrame& operator=(const AnimationWorkFrame&) = delete;
private:
    AnimationWorkRow m_row;
    AnimationWorkRow* m_previous = nullptr;
    bool m_active = false;
};
}
