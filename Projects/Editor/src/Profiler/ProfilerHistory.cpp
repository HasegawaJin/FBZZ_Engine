/// @file    ProfilerHistory.cpp
/// @brief   非表示時の有界採取と文脈別の Inclusive/Self 集計。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Core/Memory/MemorySystem.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
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
PerformanceBenchmarkCapture s_benchmark;
std::optional<PerformanceBenchmarkReport> s_benchmarkReference;
bool s_benchmarkEnabledRecording = false;
bool s_memoryRecording = true;
uint64_t s_memorySession = 1;
size_t s_historyBytes = 0;
void RestoreBenchmarkRecording()
{
    if (!s_benchmarkEnabledRecording) return;
    profiler::Profiler::RequestRecording(false);
    s_benchmarkEnabledRecording = false;
}
PerformanceBenchmarkDistribution Distribution(std::vector<double>& values)
{
    PerformanceBenchmarkDistribution result;
    result.count = values.size();
    if (values.empty()) return result;
    std::sort(values.begin(), values.end());
    const auto percentile = [&](size_t percent) {
        const size_t rank = values.size() / 100 * percent + (values.size() % 100 * percent + 99) / 100;
        return values[rank - 1];
    };
    result.p50Ms = percentile(50); result.p95Ms = percentile(95); result.p99Ms = percentile(99);
    result.maxMs = values.back();
    return result;
}
PhysicsBenchmarkScope ScopeFor(const profiler::PerformanceSnapshot& snapshot, uint64_t key)
{
    const auto descriptor = std::find_if(snapshot.descriptors.begin(), snapshot.descriptors.end(),
        [key](const auto& value) { return value.sampleKey == key; });
    if (descriptor == snapshot.descriptors.end()) return PhysicsBenchmarkScope::COUNT;
    for (size_t index = 0; index < PHYSICS_BENCHMARK_SCOPE_COUNT; ++index) {
        const auto scope = static_cast<PhysicsBenchmarkScope>(index);
        if (descriptor->name == PhysicsBenchmarkScopeName(scope)) return scope;
    }
    return PhysicsBenchmarkScope::COUNT;
}
bool HasPhysicsSystemAncestor(const profiler::PerformanceSnapshot& snapshot,
    const profiler::ProfileSample& sample, bool& valid)
{
    uint64_t parentId = sample.parentSampleId;
    for (size_t depth = 0; parentId != 0 && depth < profiler::ProfileRecorder::MAX_DEPTH; ++depth) {
        const auto parent = std::find_if(snapshot.samples.begin(), snapshot.samples.end(),
            [parentId](const auto& value) { return value.sampleId == parentId; });
        if (parent == snapshot.samples.end() || parent->sampleId == sample.sampleId) {
            valid = false; return false;
        }
        if (ScopeFor(snapshot, parent->sampleKey) == PhysicsBenchmarkScope::SYSTEM) return true;
        parentId = parent->parentSampleId;
    }
    valid = parentId == 0;
    return false;
}
PhysicsBenchmarkRow PhysicsRow(const profiler::PerformanceSnapshot& snapshot)
{
    PhysicsBenchmarkRow row;
    /// @note 収集を絞った記録から未収集 Physics を 0 ms と推定しない。
    row.complete = snapshot.complete && snapshot.droppedSampleCount == 0
        && snapshot.captureFilter.category.empty() && snapshot.captureFilter.scope.empty();
    for (const auto& sample : snapshot.samples) {
        const auto scope = ScopeFor(snapshot, sample.sampleKey);
        if (scope == PhysicsBenchmarkScope::COUNT) continue;
        if (sample.status == profiler::ProfileSampleStatus::FAULTED) {
            ++row.faultedScopeCount; row.complete = false; continue;
        }
        if (sample.status == profiler::ProfileSampleStatus::ABORTED) {
            ++row.abortedScopeCount; row.complete = false; continue;
        }
        if (sample.sampleKind != profiler::ProfileSampleKind::TIMED_SCOPE
            || !std::isfinite(sample.inclusiveMs) || sample.inclusiveMs < 0.0) {
            ++row.invalidScopeCount; row.complete = false; continue;
        }
        if (scope == PhysicsBenchmarkScope::SYSTEM) {
            bool valid = true;
            /// @note scheduler と System 本体の同名 scope は最外区間だけを数える。
            if (HasPhysicsSystemAncestor(snapshot, sample, valid)) continue;
            if (!valid) { ++row.invalidScopeCount; row.complete = false; continue; }
        }
        auto& value = row.scopes[static_cast<size_t>(scope)];
        ++value.calls; value.totalMs += sample.inclusiveMs;
    }
    row.stepCount = row.scopes[static_cast<size_t>(PhysicsBenchmarkScope::WORLD_STEP)].calls;
    return row;
}
size_t SnapshotBytes(const profiler::PerformanceSnapshot& snapshot)
{
    size_t bytes = sizeof(snapshot) + snapshot.samples.capacity() * sizeof(profiler::ProfileSample);
    bytes += snapshot.captureFilter.category.capacity() + snapshot.captureFilter.scope.capacity();
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
const char* PhysicsBenchmarkScopeName(PhysicsBenchmarkScope scope)
{
    switch (scope) {
    case PhysicsBenchmarkScope::WORLD_STEP: return "physics::World::Step";
    case PhysicsBenchmarkScope::SYSTEM: return "PhysicsSystem";
    case PhysicsBenchmarkScope::SYNC_RIGID_BODIES: return "PhysicsSystem::SyncRigidBodies";
    case PhysicsBenchmarkScope::SYNC_COLLIDERS: return "PhysicsSystem::SyncColliders";
    case PhysicsBenchmarkScope::SUBMIT_SCENE_STATE: return "PhysicsSystem::SubmitSceneState";
    case PhysicsBenchmarkScope::DISPATCH_COLLISION_EVENTS: return "PhysicsSystem::DispatchCollisionEvents";
    case PhysicsBenchmarkScope::WRITE_BACK_TRANSFORMS: return "PhysicsSystem::WriteBackTransforms";
    default: return "";
    }
}
bool PerformanceBenchmarkCapture::IsActive() const
{
    return m_status.state == PerformanceBenchmarkState::WARMUP || m_status.state == PerformanceBenchmarkState::SAMPLING;
}
bool PerformanceBenchmarkCapture::Start(std::string label, size_t warmupFrames, size_t sampleFrames,
    const profiler::PerformanceSnapshot& baseline, PerformanceBenchmarkConditions conditions)
{
    if (IsActive() || label.empty() || label.size() > 256 || !sampleFrames || sampleFrames > 60000 || warmupFrames > 60000
        || conditions.scenario.size() > 256 || conditions.camera.size() > 256 || conditions.configuration.size() > 1024
        || sampleFrames > m_rows.max_size()
        || sampleFrames > m_cpuTimings.max_size() || sampleFrames > m_wallTimings.max_size()) return false;
    for (size_t index = 0; index < PHYSICS_BENCHMARK_SCOPE_COUNT; ++index) {
        if (sampleFrames > m_physicsFrameTimings[index].max_size()
            || sampleFrames > m_physicsPerStepTimings[index].max_size()) return false;
    }
    m_rows.clear(); m_rows.reserve(sampleFrames);
    m_cpuTimings.clear(); m_cpuTimings.reserve(sampleFrames);
    m_wallTimings.clear(); m_wallTimings.reserve(sampleFrames);
    for (size_t index = 0; index < PHYSICS_BENCHMARK_SCOPE_COUNT; ++index) {
        m_physicsFrameTimings[index].clear(); m_physicsFrameTimings[index].reserve(sampleFrames);
        m_physicsPerStepTimings[index].clear(); m_physicsPerStepTimings[index].reserve(sampleFrames);
    }
    m_label = std::move(label);
    m_conditions = std::move(conditions);
    m_captureFilter = baseline.captureFilter;
    m_width = 0; m_height = 0; m_sceneGeneration = 0;
    m_renderingMetadataAvailable = false; m_renderingConditionsChanged = false;
    m_status = {};
    m_status.state = warmupFrames ? PerformanceBenchmarkState::WARMUP : PerformanceBenchmarkState::SAMPLING;
    m_status.warmupFrames = warmupFrames; m_status.sampleFrames = sampleFrames;
    m_cpuSummary = {}; m_wallSummary = {};
    m_physicsSummary = {};
    m_hasBaseline = baseline.available;
    m_lastSession = baseline.captureSessionId; m_lastSerial = baseline.applicationFrameSerial;
    m_hasAcceptedFrame = false;
    return true;
}
void PerformanceBenchmarkCapture::Feed(const profiler::PerformanceSnapshot& snapshot,
    const renderer::RenderDebugOverlay::Snapshot& rendering, bool recordingRequested)
{
    if (!IsActive()) return;
    if (!recordingRequested) {
        m_status.state = PerformanceBenchmarkState::FAILED; m_status.failure = PerformanceBenchmarkFailure::RECORDING_STOPPED; return;
    }
    if (!snapshot.recording) {
        /// @note 有効化前の同じ snapshot は待つが、新しい停止 session/frame は最初の標本前でも拒否する。
        if (m_hasAcceptedFrame || snapshot.captureSessionId != m_lastSession || snapshot.applicationFrameSerial != m_lastSerial) {
            m_status.state = PerformanceBenchmarkState::FAILED; m_status.failure = PerformanceBenchmarkFailure::RECORDING_STOPPED;
        }
        return;
    }
    if (!snapshot.available) return;
    if (m_hasAcceptedFrame && (snapshot.captureSessionId != m_status.captureSessionId || snapshot.captureFilter != m_captureFilter)) {
        m_status.state = PerformanceBenchmarkState::FAILED; m_status.failure = PerformanceBenchmarkFailure::SESSION_CHANGED; return;
    }
    if ((m_hasBaseline || m_hasAcceptedFrame) && snapshot.captureSessionId == m_lastSession) {
        if (snapshot.applicationFrameSerial == m_lastSerial) return;
        if (snapshot.applicationFrameSerial < m_lastSerial) {
            m_status.state = PerformanceBenchmarkState::FAILED; m_status.failure = PerformanceBenchmarkFailure::FRAME_ORDER_CHANGED; return;
        }
        if (m_hasAcceptedFrame) m_status.missingFrameCount += snapshot.applicationFrameSerial - m_lastSerial - 1;
    }
    if (!m_hasAcceptedFrame) m_captureFilter = snapshot.captureFilter;
    m_hasAcceptedFrame = true;
    m_status.captureSessionId = snapshot.captureSessionId;
    m_lastSession = snapshot.captureSessionId; m_lastSerial = snapshot.applicationFrameSerial;
    if (m_status.warmedFrames < m_status.warmupFrames) {
        ++m_status.warmedFrames;
        if (m_status.warmedFrames == m_status.warmupFrames) m_status.state = PerformanceBenchmarkState::SAMPLING;
        return;
    }
    PerformanceBenchmarkRow row;
    row.captureSessionId = snapshot.captureSessionId; row.applicationFrameSerial = snapshot.applicationFrameSerial;
    row.complete = snapshot.complete && snapshot.droppedSampleCount == 0
        && snapshot.faultedSampleCount == 0 && snapshot.abortedSampleCount == 0;
    row.droppedSampleCount = snapshot.droppedSampleCount;
    row.filteredSampleCount = snapshot.filteredSampleCount;
    row.recordedSampleCount = snapshot.recordedSampleCount;
    row.sampleCapacity = snapshot.sampleCapacity;
    row.cpuFrameElapsedAvailable = std::isfinite(snapshot.cpuFrameElapsedMs) && snapshot.cpuFrameElapsedMs >= 0.0;
    row.wallFrameIntervalAvailable = snapshot.wallFrameIntervalAvailable
        && std::isfinite(snapshot.wallFrameIntervalMs) && snapshot.wallFrameIntervalMs >= 0.0;
    if (row.cpuFrameElapsedAvailable) { row.cpuFrameElapsedMs = snapshot.cpuFrameElapsedMs; m_cpuTimings.push_back(row.cpuFrameElapsedMs); }
    if (row.wallFrameIntervalAvailable) { row.wallFrameIntervalMs = snapshot.wallFrameIntervalMs; m_wallTimings.push_back(row.wallFrameIntervalMs); }
    row.renderingAvailable = rendering.gpuCurrentView.applicationFrameSerial == snapshot.applicationFrameSerial;
    if (row.renderingAvailable) {
        row.drawCalls = rendering.renderStats.drawCalls; row.triangleCount = rendering.renderStats.triangleCount;
        const auto& view = rendering.gpuCurrentView;
        if (m_renderingMetadataAvailable && (m_width != view.width || m_height != view.height || m_sceneGeneration != view.sceneGeneration))
            m_renderingConditionsChanged = true;
        if (!m_renderingMetadataAvailable) {
            m_width = view.width; m_height = view.height; m_sceneGeneration = view.sceneGeneration;
            m_renderingMetadataAvailable = true;
        }
    }
    row.physics = PhysicsRow(snapshot);
    m_physicsSummary.observedStepCount += row.physics.stepCount;
    m_physicsSummary.faultedScopeCount += row.physics.faultedScopeCount;
    m_physicsSummary.abortedScopeCount += row.physics.abortedScopeCount;
    m_physicsSummary.invalidScopeCount += row.physics.invalidScopeCount;
    if (row.physics.complete) {
        ++m_physicsSummary.validFrames;
        m_physicsSummary.stepCount += row.physics.stepCount;
        if (row.physics.stepCount == 0) ++m_physicsSummary.zeroStepFrames;
        else if (row.physics.stepCount > 1) ++m_physicsSummary.multiStepFrames;
        for (size_t index = 0; index < PHYSICS_BENCHMARK_SCOPE_COUNT; ++index) {
            const auto& value = row.physics.scopes[index];
            auto& summary = m_physicsSummary.scopes[index];
            summary.calls += value.calls; summary.totalMs += value.totalMs;
            m_physicsFrameTimings[index].push_back(value.totalMs);
            if (row.physics.stepCount != 0)
                m_physicsPerStepTimings[index].push_back(value.totalMs / row.physics.stepCount);
        }
    } else ++m_physicsSummary.excludedFrames;
    m_rows.push_back(row);
    ++m_status.capturedFrames;
    if (!row.complete) ++m_status.incompleteFrames;
    m_status.droppedSampleCount += row.droppedSampleCount;
    if (m_status.capturedFrames == m_status.sampleFrames) {
        m_cpuSummary = Distribution(m_cpuTimings); m_wallSummary = Distribution(m_wallTimings);
        for (size_t index = 0; index < PHYSICS_BENCHMARK_SCOPE_COUNT; ++index) {
            m_physicsSummary.scopes[index].frameMs = Distribution(m_physicsFrameTimings[index]);
            m_physicsSummary.scopes[index].msPerStep = Distribution(m_physicsPerStepTimings[index]);
        }
        m_status.state = PerformanceBenchmarkState::COMPLETE;
    }
}
PerformanceBenchmarkReport PerformanceBenchmarkCapture::Report() const
{
    return {m_label, m_conditions, m_captureFilter, m_status, m_cpuSummary, m_wallSummary,
        m_width, m_height, m_sceneGeneration, m_renderingMetadataAvailable, m_renderingConditionsChanged};
}
PerformanceBenchmarkComparison ComparePerformanceBenchmarks(const PerformanceBenchmarkReport& reference,
    const PerformanceBenchmarkReport& candidate)
{
    PerformanceBenchmarkComparison result;
    const auto quality = [&](const PerformanceBenchmarkReport& report) {
        if (report.status.state != PerformanceBenchmarkState::COMPLETE) result.warnings.push_back(report.label + ": capture is not complete.");
        if (report.status.incompleteFrames || report.status.droppedSampleCount) result.warnings.push_back(report.label + ": incomplete frames or dropped CPU samples; scope costs are partial.");
        if (report.status.missingFrameCount) result.warnings.push_back(report.label + ": application frames are missing.");
        if (report.cpu.count != report.status.sampleFrames || report.wall.count != report.status.sampleFrames)
            result.warnings.push_back(report.label + ": timing samples are unavailable.");
        if (report.conditions.scenario.empty() || report.conditions.camera.empty() || report.conditions.configuration.empty())
            result.warnings.push_back(report.label + ": declare scenario, camera and fixed configuration before comparison.");
        if (report.renderingConditionsChanged) result.warnings.push_back(report.label + ": observed scene or viewport changed while sampling.");
    };
    quality(reference);
    if (&reference != &candidate) quality(candidate);
    if (reference.conditions != candidate.conditions) result.warnings.push_back("Declared fixed conditions differ.");
    if (reference.captureFilter != candidate.captureFilter) result.warnings.push_back("CPU collection filters differ.");
    if (reference.status.warmupFrames != candidate.status.warmupFrames || reference.status.sampleFrames != candidate.status.sampleFrames)
        result.warnings.push_back("Warmup or requested sample count differs.");
    if (reference.renderingMetadataAvailable != candidate.renderingMetadataAvailable
        || (reference.renderingMetadataAvailable && (reference.width != candidate.width || reference.height != candidate.height
            || reference.sceneGeneration != candidate.sceneGeneration))) result.warnings.push_back("Observed viewport or scene provenance differs.");
    result.comparable = result.warnings.empty();
    if (result.comparable) {
        result.cpuP50DeltaMs = candidate.cpu.p50Ms - reference.cpu.p50Ms;
        result.cpuP95DeltaMs = candidate.cpu.p95Ms - reference.cpu.p95Ms;
    }
    return result;
}
bool SetPerformanceBenchmarkReference()
{
    if (s_benchmark.Status().state != PerformanceBenchmarkState::COMPLETE) return false;
    s_benchmarkReference = s_benchmark.Report();
    return true;
}
const PerformanceBenchmarkReport* GetPerformanceBenchmarkReference() { return s_benchmarkReference ? &*s_benchmarkReference : nullptr; }
bool PerformanceBenchmarkCapture::Cancel()
{
    if (!IsActive()) return false;
    m_status.state = PerformanceBenchmarkState::CANCELLED;
    return true;
}
PerformanceBenchmarkCapture& GetPerformanceBenchmarkCapture() { return s_benchmark; }
bool StartPerformanceBenchmark(std::string label, size_t warmupFrames, size_t sampleFrames, PerformanceBenchmarkConditions conditions)
{
    const bool priorRecordingRequested = profiler::Profiler::IsRecordingRequested();
    if (!s_benchmark.Start(std::move(label), warmupFrames, sampleFrames, profiler::Profiler::GetSnapshot(), std::move(conditions))) return false;
    s_benchmarkEnabledRecording = !priorRecordingRequested;
    profiler::Profiler::RequestRecording(true);
    return true;
}
bool CancelPerformanceBenchmark()
{
    const bool cancelled = s_benchmark.Cancel();
    if (!s_benchmark.IsActive()) RestoreBenchmarkRecording();
    return cancelled;
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
    const bool benchmarking = s_benchmark.IsActive();
    if (benchmarking) {
        s_benchmark.Feed(performance, renderer::RenderDebugOverlay::GetLastSnapshot(), profiler::Profiler::IsRecordingRequested());
        if (!s_benchmark.IsActive()) RestoreBenchmarkRecording();
    }
    if (!benchmarking) RestoreBenchmarkRecording();
    if (performance.available && performance.recording &&
        (performance.captureSessionId != s_performance.lastSession || performance.applicationFrameSerial != s_performance.lastSerial)) {
        PerformanceCapture capture;
        static_cast<profiler::PerformanceSnapshot&>(capture) = performance;
        capture.rendering = renderer::RenderDebugOverlay::GetLastSnapshot();
        Capture(s_performance, std::move(capture));
    }
    /// @note scalar benchmark中もCPU画面は最新の確定frameを採る。Script/Memoryの複製は従来どおり抑える。
    if (benchmarking) return;
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
            /// @note ResourcePool の追跡は RENDERER タグのみ。詳細一覧を複製せず、従来と同じ各フレームの合計を残す。
            auto& stats = memory.tags[static_cast<size_t>(core::MemoryTag::RENDERER)].stats;
            const size_t bytes = ctx.resources->GetLiveDebugResourceBytes();
            const size_t count = ctx.resources->GetLiveDebugResourceCount();
            stats.used += bytes; stats.peakUsed += bytes;
            stats.activeCount += count; stats.allocationCount += count;
        }
        Capture(s_memory, std::move(memory));
    }
}
void ResetProfilerHistory() { CancelPerformanceBenchmark(); s_benchmark = {}; s_benchmarkReference.reset(); Clear(s_performance); Clear(s_script); Clear(s_memory); ++s_memorySession; }
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
