/// @file    ProfilerHistory.hpp
/// @brief   所有 snapshot の履歴、画面選択とスクリプト集計。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Core/Memory/MemoryTracker.hpp>
#include <deque>
#include <memory>
#include <string>
#include <vector>
namespace fbzz::editor {
struct EditorContext;
struct PerformanceCapture : profiler::PerformanceSnapshot {
    renderer::RenderDebugOverlay::Snapshot rendering;
};
template<class Snapshot> struct ProfilerCaptureHistory {
    std::deque<std::shared_ptr<const Snapshot>> frames;
    std::shared_ptr<const Snapshot> selected;
    std::shared_ptr<const Snapshot> spike;
    uint64_t lastSession = 0;
    uint64_t lastSerial = 0;
    uint64_t historyEvictedFrameCount = 0;
    bool frozen = false;
    bool budgetBlocked = false;
};
using PerformanceHistory = ProfilerCaptureHistory<PerformanceCapture>;
using ScriptHistory = ProfilerCaptureHistory<scene::ScriptProfileSnapshot>;
struct MemoryProfileTag {
    std::string name;
    core::MemoryStats stats;
};
struct MemoryProfileSnapshot : profiler::ProfileSnapshot {
    std::vector<MemoryProfileTag> tags;
    core::MemoryStats frameAllocator;
};
using MemoryHistory = ProfilerCaptureHistory<MemoryProfileSnapshot>;
enum class ScriptProfileGroup { TYPE, CALLBACK_KIND, INSTANCE };
struct ScriptProfileFilter {
    std::string name;
    std::string type;
    std::string instance;
    std::string callback;
    std::string mode;
    uint64_t sceneGeneration = 0;
};
struct ScriptProfileRow {
    scene::ScriptProfileDescriptor descriptor;
    scene::ScriptProfileContext context;
    uint64_t calls = 0;
    uint64_t faults = 0;
    uint64_t completedCalls = 0;
    uint64_t validFrames = 0;
    uint64_t excludedFrames = 0;
    double inclusiveMs = 0.0;
    double selfMs = 0.0;
    double maxCallMs = 0.0;
    double peakFrameMs = 0.0;
    double faultElapsedMs = 0.0;
    double avgPerCallMs = 0.0;
    double avgPerFrameMs = 0.0;
    double share = 0.0;
    bool selfAvailable = true;
    bool shareAvailable = true;
};
PerformanceHistory& GetPerformanceHistory();
ScriptHistory& GetScriptHistory();
MemoryHistory& GetMemoryHistory();
bool IsMemoryHistoryRecording();
void SetMemoryHistoryRecording(bool recording);
void ClearMemoryHistory();
/// @note 各 session/serial を一度だけ採る。選択中の値と DLL 内文字列を共有しない。
void TickProfilerHistory(EditorContext& ctx);
void ResetProfilerHistory();
void ClearPerformanceHistory();
void ClearScriptHistory();
std::vector<ScriptProfileRow> AggregateScriptProfiles(
    const std::vector<const scene::ScriptProfileSnapshot*>& frames,
    ScriptProfileGroup group, const ScriptProfileFilter& filter = {});
bool MatchesScriptProfile(const scene::ScriptProfileDescriptor& descriptor,
                          const scene::ScriptProfileContext& context,
                          const ScriptProfileFilter& filter);
const scene::ScriptProfileDescriptor* FindScriptDescriptor(const scene::ScriptProfileSnapshot& snapshot, uint64_t key);
const scene::ScriptProfileContext* FindScriptContext(const scene::ScriptProfileSnapshot& snapshot, uint64_t key);
}
