/// @file    ProfilerHistory.cpp
/// @brief   非表示時の有界採取と文脈別の Inclusive/Self 集計。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Core/Memory/MemorySystem.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>
namespace fbzz::editor {
namespace {
constexpr size_t MAX_HISTORY_FRAMES = 240;
/// @note 既存表の文字列と集計索引へ 16 MiB を予約し、所有 snapshot と合計 64 MiB に収める。
constexpr size_t MAX_HISTORY_BYTES = 48 * 1024 * 1024;
PerformanceHistory s_performance;
ScriptHistory s_script;
MemoryHistory s_memory;
bool s_memoryRecording = true;
uint64_t s_memorySession = 1;
size_t s_historyBytes = 0;
size_t SnapshotBytes(const profiler::PerformanceSnapshot& snapshot)
{
    size_t bytes = sizeof(snapshot) + snapshot.samples.capacity() * sizeof(profiler::ProfileSample);
    bytes += snapshot.descriptors.capacity() * sizeof(profiler::PerformanceDescriptor);
    for (const auto& descriptor : snapshot.descriptors) bytes += descriptor.name.capacity() + descriptor.category.capacity();
    return bytes;
}
size_t SnapshotBytes(const PerformanceCapture& snapshot)
{
    size_t bytes = SnapshotBytes(static_cast<const profiler::PerformanceSnapshot&>(snapshot));
    bytes += sizeof(snapshot.rendering) + snapshot.rendering.planDescription.capacity();
    for (const auto& [name, ms] : snapshot.rendering.gpuPassTimings) { (void)ms; bytes += sizeof(name) + sizeof(ms) + name.capacity(); }
    bytes += snapshot.rendering.passTimings.capacity() * sizeof(std::pair<std::string, double>);
    for (const auto& [name, ms] : snapshot.rendering.passTimings) { (void)ms; bytes += name.capacity(); }
    bytes += snapshot.rendering.gpuProfiler.passes.capacity() * sizeof(renderer::GpuPassProfile);
    for (const auto& pass : snapshot.rendering.gpuProfiler.passes) bytes += pass.name.capacity();
    return bytes;
}
size_t SnapshotBytes(const scene::ScriptProfileSnapshot& snapshot)
{
    size_t bytes = sizeof(snapshot) + snapshot.samples.capacity() * sizeof(profiler::ProfileSample);
    bytes += snapshot.contexts.capacity() * sizeof(scene::ScriptProfileContext);
    bytes += snapshot.descriptors.capacity() * sizeof(scene::ScriptProfileDescriptor);
    for (const auto& descriptor : snapshot.descriptors)
        bytes += descriptor.inspectionId.capacity() + descriptor.instanceId.capacity() + descriptor.objectName.capacity() + descriptor.typeName.capacity() + descriptor.callbackLabel.capacity();
    return bytes;
}
size_t SnapshotBytes(const MemoryProfileSnapshot& snapshot)
{
    size_t bytes = sizeof(snapshot) + snapshot.tags.capacity() * sizeof(MemoryProfileTag);
    for (const auto& tag : snapshot.tags) bytes += tag.name.capacity();
    return bytes;
}
template<class Snapshot> bool EvictOne(ProfilerCaptureHistory<Snapshot>& history)
{
    const auto it = std::find_if(history.frames.begin(), history.frames.end(), [&](const auto& frame) {
        return (!history.frozen || frame != history.selected) && frame != history.spike;
    });
    if (it == history.frames.end()) return false;
    s_historyBytes -= SnapshotBytes(**it);
    if (*it == history.selected) history.selected.reset();
    history.frames.erase(it);
    ++history.historyEvictedFrameCount;
    return true;
}
template<class Snapshot> uint64_t OldestEvictableSerial(const ProfilerCaptureHistory<Snapshot>& history)
{
    for (const auto& frame : history.frames)
        if ((!history.frozen || frame != history.selected) && frame != history.spike) return frame->applicationFrameSerial;
    return (std::numeric_limits<uint64_t>::max)();
}
bool EvictOldest()
{
    const uint64_t performance = OldestEvictableSerial(s_performance);
    const uint64_t script = OldestEvictableSerial(s_script);
    const uint64_t memory = OldestEvictableSerial(s_memory);
    if (performance == (std::numeric_limits<uint64_t>::max)() && script == performance && memory == performance) return false;
    if (performance <= script && performance <= memory) return EvictOne(s_performance);
    return script <= memory ? EvictOne(s_script) : EvictOne(s_memory);
}
template<class Snapshot> void Capture(ProfilerCaptureHistory<Snapshot>& history, Snapshot snapshot)
{
    if (!snapshot.available || !snapshot.recording ||
        (history.lastSession == snapshot.captureSessionId && history.lastSerial == snapshot.applicationFrameSerial)) return;
    history.lastSession = snapshot.captureSessionId;
    history.lastSerial = snapshot.applicationFrameSerial;
    while (history.frames.size() >= MAX_HISTORY_FRAMES && EvictOne(history)) {}
    const size_t bytes = SnapshotBytes(snapshot);
    while (s_historyBytes + bytes > MAX_HISTORY_BYTES) {
        if (!EvictOldest()) break;
    }
    history.budgetBlocked = s_historyBytes + bytes > MAX_HISTORY_BYTES;
    if (history.budgetBlocked) return;
    auto owned = std::make_shared<const Snapshot>(std::move(snapshot));
    s_historyBytes += SnapshotBytes(*owned);
    history.frames.push_back(owned);
    if (!history.frozen) history.selected = std::move(owned);
}
template<class Snapshot> void Clear(ProfilerCaptureHistory<Snapshot>& history)
{
    for (const auto& frame : history.frames) s_historyBytes -= SnapshotBytes(*frame);
    history = {};
}
std::string RowKey(const scene::ScriptProfileDescriptor& descriptor, const scene::ScriptProfileContext& context,
                   ScriptProfileGroup group, uint64_t session)
{
    std::string key = std::to_string(session) + "/" + std::to_string(context.executionContextId) + "/" + std::to_string(descriptor.typeId);
    if (group != ScriptProfileGroup::TYPE) {
        key += "/" + std::to_string(static_cast<int>(descriptor.callbackKind));
        if (descriptor.callbackKind == scene::ScriptCallbackKind::UNKNOWN || descriptor.callbackKind == scene::ScriptCallbackKind::EVENT_HANDLER)
            key += "/" + descriptor.callbackLabel;
    }
    if (group == ScriptProfileGroup::INSTANCE) key += "/" + descriptor.inspectionId;
    return key;
}
}
PerformanceHistory& GetPerformanceHistory() { return s_performance; }
ScriptHistory& GetScriptHistory() { return s_script; }
MemoryHistory& GetMemoryHistory() { return s_memory; }
bool IsMemoryHistoryRecording() { return s_memoryRecording; }
void SetMemoryHistoryRecording(bool recording)
{
    if (recording && !s_memoryRecording) ++s_memorySession;
    s_memoryRecording = recording;
}
void ClearMemoryHistory() { Clear(s_memory); ++s_memorySession; }
void TickProfilerHistory(EditorContext& ctx)
{
    const auto& performance = profiler::Profiler::GetSnapshot();
    if (performance.available && performance.recording &&
        (performance.captureSessionId != s_performance.lastSession || performance.applicationFrameSerial != s_performance.lastSerial)) {
        PerformanceCapture capture;
        static_cast<profiler::PerformanceSnapshot&>(capture) = performance;
        capture.rendering = renderer::RenderDebugOverlay::GetLastSnapshot();
        Capture(s_performance, std::move(capture));
    }
    const auto& script = scene::ScriptProfiler::GetSnapshot();
    if (script.available && script.recording &&
        (script.captureSessionId != s_script.lastSession || script.applicationFrameSerial != s_script.lastSerial)) Capture(s_script, script);
    if (s_memoryRecording && ctx.memorySystem != nullptr && ctx.memorySystem->IsInitialized()) {
        MemoryProfileSnapshot memory;
        memory.recording = true; memory.available = true; memory.captureSessionId = s_memorySession;
        memory.applicationFrameSerial = ctx.resources != nullptr ? ctx.resources->FrameStamp() : (std::max)(performance.applicationFrameSerial, script.applicationFrameSerial);
        const auto& tracker = ctx.memorySystem->GetTracker();
        memory.frameAllocator = ctx.memorySystem->GetFrameAllocator().GetStats();
        for (size_t i = 0; i < static_cast<size_t>(core::MemoryTag::COUNT); ++i) {
            const auto tag = static_cast<core::MemoryTag>(i);
            memory.tags.push_back({tracker.GetTagName(tag), tracker.GetStats(tag)});
        }
        if (ctx.resources != nullptr) {
            std::vector<core::AllocationInfo> live;
            ctx.resources->CollectLiveDebugResources(live);
            for (const auto& allocation : live) {
                const size_t index = static_cast<size_t>(allocation.tag);
                if (index >= memory.tags.size()) continue;
                auto& stats = memory.tags[index].stats;
                stats.used += allocation.size; stats.peakUsed += allocation.size;
                ++stats.activeCount; ++stats.allocationCount;
            }
        }
        Capture(s_memory, std::move(memory));
    }
}
void ResetProfilerHistory() { Clear(s_performance); Clear(s_script); Clear(s_memory); ++s_memorySession; }
void ClearPerformanceHistory() { Clear(s_performance); profiler::Profiler::RequestClear(); }
void ClearScriptHistory() { Clear(s_script); scene::ScriptProfiler::RequestClear(); }
const scene::ScriptProfileDescriptor* FindScriptDescriptor(const scene::ScriptProfileSnapshot& snapshot, uint64_t key)
{
    for (const auto& value : snapshot.descriptors) if (value.sampleKey == key) return &value;
    return nullptr;
}
const scene::ScriptProfileContext* FindScriptContext(const scene::ScriptProfileSnapshot& snapshot, uint64_t key)
{
    for (const auto& value : snapshot.contexts) if (value.executionContextId == key) return &value;
    return nullptr;
}
bool MatchesScriptProfile(const scene::ScriptProfileDescriptor& descriptor, const scene::ScriptProfileContext& context, const ScriptProfileFilter& filter)
{
    return (filter.sceneGeneration == 0 || context.sceneGeneration == filter.sceneGeneration) &&
        (filter.mode.empty() || filter.mode == scene::ScriptProfiler::GetModeName(context.mode)) &&
        (filter.type.empty() || filter.type == descriptor.typeName) &&
        (filter.instance.empty() || filter.instance == descriptor.inspectionId || filter.instance == descriptor.instanceId) &&
        (filter.callback.empty() || filter.callback == scene::ScriptProfiler::GetCallbackName(descriptor.callbackKind)) &&
        (filter.name.empty() || descriptor.objectName.find(filter.name) != std::string::npos || descriptor.typeName.find(filter.name) != std::string::npos || descriptor.callbackLabel.find(filter.name) != std::string::npos);
}
std::vector<ScriptProfileRow> AggregateScriptProfiles(const std::vector<const scene::ScriptProfileSnapshot*>& frames, ScriptProfileGroup group, const ScriptProfileFilter& filter)
{
    std::vector<ScriptProfileRow> rows;
    std::unordered_map<std::string, size_t> indices;
    std::vector<uint64_t> sessions;
    std::unordered_map<std::string, double> contextSelf;
    std::unordered_map<std::string, bool> contextComplete;
    for (const auto* frame : frames) {
        if (frame == nullptr) continue;
        const bool normalFrame = frame->recording && frame->available && frame->complete && !frame->transitionFrame;
        for (const auto& context : frame->contexts) {
            const std::string key = std::to_string(frame->captureSessionId) + "/" + std::to_string(context.executionContextId);
            auto [it, inserted] = contextComplete.emplace(key, normalFrame);
            if (!inserted) it->second = it->second && normalFrame;
        }
        std::unordered_map<size_t, double> frameTotals;
        for (const auto& sample : frame->samples) {
            const auto* descriptor = FindScriptDescriptor(*frame, sample.sampleKey);
            const auto* context = descriptor != nullptr ? FindScriptContext(*frame, descriptor->executionContextId) : nullptr;
            if (context == nullptr) continue;
            const std::string contextKey = std::to_string(frame->captureSessionId) + "/" + std::to_string(context->executionContextId);
            contextComplete[contextKey] = contextComplete[contextKey] && sample.selfAvailable && sample.status == profiler::ProfileSampleStatus::COMPLETE;
            if (normalFrame && sample.selfAvailable && sample.status == profiler::ProfileSampleStatus::COMPLETE) contextSelf[contextKey] += sample.selfMs;
            if (!MatchesScriptProfile(*descriptor, *context, filter)) continue;
            const std::string key = RowKey(*descriptor, *context, group, frame->captureSessionId);
            auto [it, inserted] = indices.emplace(key, rows.size());
            if (inserted) { ScriptProfileRow row; row.descriptor = *descriptor; row.context = *context; rows.push_back(std::move(row)); sessions.push_back(frame->captureSessionId); }
            const size_t index = it->second;
            auto& row = rows[index];
            ++row.calls;
            if (sample.status == profiler::ProfileSampleStatus::FAULTED) ++row.faults;
            if (sample.status != profiler::ProfileSampleStatus::COMPLETE) { row.faultElapsedMs += sample.inclusiveMs; row.selfAvailable = false; continue; }
            if (!normalFrame) { row.selfAvailable = false; continue; }
            ++row.completedCalls;
            row.inclusiveMs += sample.inclusiveMs;
            row.maxCallMs = (std::max)(row.maxCallMs, sample.inclusiveMs);
            row.selfAvailable = row.selfAvailable && sample.selfAvailable;
            if (sample.selfAvailable) row.selfMs += sample.selfMs;
            frameTotals[index] += sample.inclusiveMs;
        }
        for (const auto& [index, total] : frameTotals) rows[index].peakFrameMs = (std::max)(rows[index].peakFrameMs, total);
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        auto& row = rows[i];
        for (const auto* frame : frames) {
            if (frame == nullptr || frame->captureSessionId != sessions[i]) continue;
            if (FindScriptContext(*frame, row.context.executionContextId) == nullptr) continue;
            if (frame->recording && frame->available && frame->complete && !frame->transitionFrame) ++row.validFrames;
            else ++row.excludedFrames;
        }
        row.avgPerCallMs = row.completedCalls != 0 ? row.inclusiveMs / static_cast<double>(row.completedCalls) : 0.0;
        row.avgPerFrameMs = row.validFrames != 0 ? row.inclusiveMs / static_cast<double>(row.validFrames) : 0.0;
        const std::string contextKey = std::to_string(sessions[i]) + "/" + std::to_string(row.context.executionContextId);
        row.shareAvailable = contextComplete[contextKey] && row.selfAvailable;
        const double totalSelf = contextSelf[contextKey];
        if (row.shareAvailable && totalSelf > 0.0) row.share = row.selfMs / totalSelf;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.selfMs > b.selfMs; });
    return rows;
}
}
