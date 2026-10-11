/// @file    ProfilerOperators.cpp
/// @brief   最新確定値と明示フレームに対する Profiler 診断契約。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Editor/Profiler/ProfilerOperators.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <algorithm>
#include <charconv>
#include <unordered_map>
#include <unordered_set>
namespace fbzz::editor {
namespace {
OpData Id(uint64_t value) { return OpData(std::to_string(value)); }
bool ParseId(const std::string& value, uint64_t& result)
{
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return !value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
template<class Snapshot> const Snapshot* RequestedFrame(const ProfilerCaptureHistory<Snapshot>& history, const OpArgs& args, bool& valid)
{
    const bool hasSession = args.Has("captureSessionId"), hasSerial = args.Has("applicationFrameSerial");
    valid = hasSession == hasSerial;
    if (!hasSession || !valid) return nullptr;
    uint64_t session = 0, serial = 0;
    valid = ParseId(args.GetString("captureSessionId"), session) && ParseId(args.GetString("applicationFrameSerial"), serial);
    if (!valid) return nullptr;
    for (const auto& frame : history.frames) if (frame->captureSessionId == session && frame->applicationFrameSerial == serial) return frame.get();
    return nullptr;
}
OpData Common(const profiler::ProfileSnapshot& snapshot, uint64_t evicted)
{
    OpData result = OpData::MakeObject();
    result.Set("schemaVersion", OpData(1)); result.Set("recording", OpData(snapshot.recording));
    result.Set("available", OpData(snapshot.available)); result.Set("complete", OpData(snapshot.complete));
    result.Set("captureSessionId", Id(snapshot.captureSessionId)); result.Set("applicationFrameSerial", Id(snapshot.applicationFrameSerial));
    result.Set("frameIndex", Id(snapshot.frameIndex)); result.Set("recordedSampleCount", OpData(static_cast<int>(snapshot.recordedSampleCount)));
    result.Set("droppedSampleCount", OpData(static_cast<int>(snapshot.droppedSampleCount)));
    result.Set("filteredSampleCount", OpData(snapshot.filteredSampleCount));
    result.Set("sampleCapacity", OpData(snapshot.sampleCapacity));
    result.Set("capacityDroppedSampleCount", OpData(snapshot.capacityDroppedSampleCount));
    result.Set("depthDroppedSampleCount", OpData(snapshot.depthDroppedSampleCount));
    result.Set("descriptorDroppedSampleCount", OpData(snapshot.descriptorDroppedSampleCount));
    result.Set("faultedSampleCount", OpData(snapshot.faultedSampleCount));
    result.Set("abortedSampleCount", OpData(snapshot.abortedSampleCount));
    result.Set("warning", OpData(snapshot.available && (!snapshot.complete || snapshot.droppedSampleCount) ? "Incomplete CPU capture; scope totals and Self are partial. Narrow the collection filter before recording." : ""));
    result.Set("unframedInvocationCount", Id(snapshot.unframedInvocationCount)); result.Set("historyEvictedFrameCount", Id(evicted));
    return result;
}
OpData Sample(const profiler::ProfileSample& sample)
{
    OpData data = OpData::MakeObject();
    data.Set("sampleId", Id(sample.sampleId)); data.Set("parentSampleId", Id(sample.parentSampleId)); data.Set("sampleKey", Id(sample.sampleKey));
    data.Set("startAvailable", OpData(sample.startAvailable)); data.Set("startOffsetMs", sample.startAvailable ? OpData(sample.startOffsetMs) : OpData{});
    data.Set("inclusiveMs", OpData(sample.inclusiveMs)); data.Set("selfAvailable", OpData(sample.selfAvailable)); data.Set("selfMs", sample.selfAvailable ? OpData(sample.selfMs) : OpData{});
    data.Set("depth", OpData(static_cast<int>(sample.depth)));
    data.Set("status", OpData(sample.status == profiler::ProfileSampleStatus::COMPLETE ? "complete" : sample.status == profiler::ProfileSampleStatus::FAULTED ? "faulted" : "aborted"));
    data.Set("sampleKind", OpData(sample.sampleKind == profiler::ProfileSampleKind::TIMED_SCOPE ? "timed_scope" : sample.sampleKind == profiler::ProfileSampleKind::EXTERNAL_DURATION ? "external_duration" : "instant"));
    return data;
}
OpData GpuMetadata(const renderer::GpuProfilerViewMetadata& value)
{
    OpData data = OpData::MakeObject();
    data.Set("applicationFrameSerial", Id(value.applicationFrameSerial)); data.Set("viewId", Id(value.viewId));
    data.Set("sceneGeneration", Id(value.sceneGeneration)); data.Set("planGeneration", Id(value.planGeneration)); data.Set("resourceEpoch", Id(value.resourceEpoch));
    data.Set("width", OpData(static_cast<int>(value.width))); data.Set("height", OpData(static_cast<int>(value.height)));
    data.Set("outputId", OpData(static_cast<int>(value.outputId))); data.Set("outputGeneration", OpData(static_cast<int>(value.outputGeneration)));
    return data;
}
OpData CaptureFilter(const profiler::PerformanceCaptureFilter& filter)
{
    auto data = OpData::MakeObject();
    data.Set("category", OpData(filter.category)); data.Set("scope", OpData(filter.scope));
    data.Set("match", OpData("category_exact_scope_case_sensitive_substring"));
    return data;
}
OpData BenchmarkComparison(const PerformanceBenchmarkReport& report)
{
    const auto* reference = GetPerformanceBenchmarkReference();
    const auto comparison = ComparePerformanceBenchmarks(reference ? *reference : report, report);
    auto data = OpData::MakeObject();
    data.Set("referenceAvailable", OpData(reference != nullptr));
    data.Set("eligible", OpData(comparison.comparable));
    data.Set("comparable", OpData(reference && comparison.comparable));
    data.Set("referenceLabel", reference ? OpData(reference->label) : OpData{});
    data.Set("cpuP50DeltaMs", reference && comparison.comparable ? OpData(comparison.cpuP50DeltaMs) : OpData{});
    data.Set("cpuP95DeltaMs", reference && comparison.comparable ? OpData(comparison.cpuP95DeltaMs) : OpData{});
    auto warnings = OpData::MakeArray();
    for (const auto& warning : comparison.warnings) warnings.Push(OpData(warning));
    data.Set("warnings", std::move(warnings));
    return data;
}
OpResult PerformanceQuery(const OpArgs& args)
{
    const auto& history = GetPerformanceHistory();
    bool valid = true;
    const auto* requested = RequestedFrame(history, args, valid);
    if (!valid) return OpResult::Err("BAD_ARG", "Specify both frame identifiers as decimal strings.");
    if (args.Has("captureSessionId") && requested == nullptr) return OpResult::Err("NOT_FOUND", "The requested frame is no longer retained.");
    const auto& snapshot = requested != nullptr ? static_cast<const profiler::PerformanceSnapshot&>(*requested) : profiler::Profiler::GetSnapshot();
    const auto& rendering = requested != nullptr ? requested->rendering : renderer::RenderDebugOverlay::GetLastSnapshot();
    auto result = Common(snapshot, history.historyEvictedFrameCount);
    result.Set("retainedFrameCount", OpData(static_cast<int>(history.frames.size())));
    result.Set("captureFilter", CaptureFilter(snapshot.captureFilter));
    result.Set("wallFrameIntervalAvailable", OpData(snapshot.wallFrameIntervalAvailable));
    result.Set("wallFrameIntervalMs", snapshot.wallFrameIntervalAvailable ? OpData(snapshot.wallFrameIntervalMs) : OpData{});
    result.Set("cpuFrameElapsedMs", snapshot.available ? OpData(snapshot.cpuFrameElapsedMs) : OpData{});
    result.Set("scopeRootSumMs", snapshot.available ? OpData(snapshot.scopeRootSumMs) : OpData{});
    result.Set("timingDefinition", OpData("Monotonic elapsed intervals; scope totals may overlap parallel workers."));
    OpData descriptors = OpData::MakeArray();
    for (const auto& descriptor : snapshot.descriptors) {
        auto row = OpData::MakeObject(); row.Set("sampleKey", Id(descriptor.sampleKey)); row.Set("name", OpData(descriptor.name)); row.Set("category", OpData(descriptor.category)); descriptors.Push(std::move(row));
    }
    result.Set("descriptors", std::move(descriptors));
    OpData samples = OpData::MakeArray();
    const size_t limit = static_cast<size_t>(args.GetInt("limit", 256));
    for (size_t i = 0; i < snapshot.samples.size() && i < limit; ++i) samples.Push(Sample(snapshot.samples[i]));
    result.Set("samples", std::move(samples)); result.Set("truncated", OpData(snapshot.samples.size() > limit));
    auto gpu = OpData::MakeObject();
    gpu.Set("supported", OpData(rendering.gpuProfiler.supported)); gpu.Set("available", OpData(rendering.gpuProfiler.available)); gpu.Set("complete", OpData(rendering.gpuProfiler.complete));
    gpu.Set("physicalFrameSerial", Id(rendering.gpuProfiler.physicalFrameSerial)); gpu.Set("deviceEpoch", Id(rendering.gpuProfiler.deviceEpoch));
    gpu.Set("recordedPassCount", OpData(static_cast<int>(rendering.gpuProfiler.recordedPassCount))); gpu.Set("droppedPassCount", OpData(static_cast<int>(rendering.gpuProfiler.droppedPassCount)));
    gpu.Set("totalGpuTimeAvailable", OpData(false)); gpu.Set("totalGpuMs", OpData{}); gpu.Set("strictCpuGpuJoined", OpData(false));
    gpu.Set("currentView", GpuMetadata(rendering.gpuCurrentView));
    OpData passes = OpData::MakeArray();
    for (const auto& pass : rendering.gpuProfiler.passes) {
        auto row = OpData::MakeObject(); row.Set("name", OpData(pass.name)); row.Set("available", OpData(pass.available)); row.Set("gpuMs", pass.available ? OpData(pass.gpuMs) : OpData{});
        row.Set("metadata", GpuMetadata(pass.metadata)); row.Set("physicalFrameSerial", Id(pass.physicalFrameSerial)); row.Set("deviceEpoch", Id(pass.deviceEpoch));
        row.Set("age", snapshot.applicationFrameSerial >= pass.metadata.applicationFrameSerial ? Id(snapshot.applicationFrameSerial - pass.metadata.applicationFrameSerial) : OpData{}); passes.Push(std::move(row));
    }
    gpu.Set("passes", std::move(passes)); result.Set("gpu", std::move(gpu));
    auto stats = OpData::MakeObject();
    stats.Set("drawCalls", OpData(rendering.renderStats.drawCalls)); stats.Set("triangleCount", OpData(rendering.renderStats.triangleCount));
    stats.Set("totalObjects", OpData(rendering.renderStats.totalObjects)); stats.Set("frustumCulled", OpData(rendering.renderStats.frustumCulled));
    stats.Set("occlusionCulled", OpData(rendering.renderStats.occlusionCulled)); stats.Set("distanceCulled", OpData(rendering.renderStats.distanceCulled));
    result.Set("rendering", std::move(stats));
    return OpResult::Data(std::move(result));
}
OpResult ScriptQuery(const OpArgs& args)
{
    const auto& history = GetScriptHistory();
    bool valid = true;
    const auto* requested = RequestedFrame(history, args, valid);
    if (!valid) return OpResult::Err("BAD_ARG", "Specify both frame identifiers as decimal strings.");
    if (args.Has("captureSessionId") && requested == nullptr) return OpResult::Err("NOT_FOUND", "The requested frame is no longer retained.");
    const auto& snapshot = requested != nullptr ? *requested : scene::ScriptProfiler::GetSnapshot();
    ScriptProfileFilter filter;
    filter.name = args.GetString("name"); filter.type = args.GetString("type"); filter.instance = args.GetString("instance"); filter.callback = args.GetString("callback"); filter.mode = args.GetString("mode");
    if (args.Has("scene") && !ParseId(args.GetString("scene"), filter.sceneGeneration)) return OpResult::Err("BAD_ARG", "scene must be a decimal generation.");
    if (!filter.callback.empty()) {
        bool known = false;
        for (int i = 0; i <= static_cast<int>(scene::ScriptCallbackKind::COROUTINE_STEP); ++i) known = known || filter.callback == scene::ScriptProfiler::GetCallbackName(static_cast<scene::ScriptCallbackKind>(i));
        if (!known) return OpResult::Err("BAD_ARG", "Unknown callback.");
    }
    auto result = Common(snapshot, history.historyEvictedFrameCount);
    result.Set("retainedFrameCount", OpData(static_cast<int>(history.frames.size())));
    result.Set("transitionFrame", OpData(snapshot.transitionFrame)); result.Set("timingDefinition", OpData("Inclusive contains nested Script calls; Self retains Engine API cost."));
    OpData contexts = OpData::MakeArray();
    for (const auto& context : snapshot.contexts) {
        auto row = OpData::MakeObject(); row.Set("executionContextId", Id(context.executionContextId)); row.Set("runtimeEpoch", Id(context.runtimeEpoch));
        row.Set("sceneGeneration", Id(context.sceneGeneration)); row.Set("dllGeneration", Id(context.dllGeneration)); row.Set("mode", OpData(scene::ScriptProfiler::GetModeName(context.mode))); contexts.Push(std::move(row));
    }
    result.Set("contexts", std::move(contexts));
    OpData descriptors = OpData::MakeArray();
    for (const auto& descriptor : snapshot.descriptors) {
        auto row = OpData::MakeObject(); row.Set("sampleKey", Id(descriptor.sampleKey)); row.Set("executionContextId", Id(descriptor.executionContextId)); row.Set("typeId", Id(descriptor.typeId));
        row.Set("type", OpData(descriptor.typeName)); row.Set("callback", OpData(scene::ScriptProfiler::GetCallbackName(descriptor.callbackKind))); row.Set("label", OpData(descriptor.callbackLabel));
        row.Set("instance", OpData(descriptor.inspectionId)); row.Set("objectInstanceId", OpData(descriptor.instanceId)); row.Set("objectName", OpData(descriptor.objectName)); descriptors.Push(std::move(row));
    }
    result.Set("descriptors", std::move(descriptors));
    const auto groupBy = args.GetString("groupBy", "instance");
    const size_t limit = static_cast<size_t>(args.GetInt("limit", 256));
    OpData output = OpData::MakeArray();
    bool truncated = false;
    if (groupBy == "call_tree") {
        std::unordered_map<uint64_t, const profiler::ProfileSample*> index;
        for (const auto& sample : snapshot.samples) index[sample.sampleId] = &sample;
        std::unordered_set<uint64_t> included;
        for (const auto& sample : snapshot.samples) {
            const auto* descriptor = FindScriptDescriptor(snapshot, sample.sampleKey);
            const auto* context = descriptor != nullptr ? FindScriptContext(snapshot, descriptor->executionContextId) : nullptr;
            if (context == nullptr || !MatchesScriptProfile(*descriptor, *context, filter)) continue;
            std::vector<uint64_t> ancestry;
            uint64_t id = sample.sampleId;
            while (id != 0 && !included.contains(id)) {
                const auto found = index.find(id); if (found == index.end()) break;
                ancestry.push_back(id); id = found->second->parentSampleId;
            }
            if (included.size() + ancestry.size() > limit) { truncated = true; continue; }
            included.insert(ancestry.begin(), ancestry.end());
        }
        for (const auto& sample : snapshot.samples) if (included.contains(sample.sampleId)) output.Push(Sample(sample));
        result.Set("samples", std::move(output));
    } else {
        const auto group = groupBy == "type" ? ScriptProfileGroup::TYPE : groupBy == "callback" ? ScriptProfileGroup::CALLBACK_KIND : ScriptProfileGroup::INSTANCE;
        auto rows = AggregateScriptProfiles({&snapshot}, group, filter);
        const auto sort = args.GetString("sort", "self");
        const auto metric = [&](const ScriptProfileRow& row) { return sort == "inclusive" ? row.inclusiveMs : sort == "calls" ? static_cast<double>(row.calls) : sort == "max_call" ? row.maxCallMs : row.selfMs; };
        std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return sort == "name" ? a.descriptor.typeName < b.descriptor.typeName : metric(a) > metric(b); });
        for (size_t i = 0; i < rows.size() && i < limit; ++i) {
            const auto& row = rows[i]; auto data = OpData::MakeObject();
            data.Set("sampleKey", Id(row.descriptor.sampleKey)); data.Set("executionContextId", Id(row.context.executionContextId)); data.Set("typeId", Id(row.descriptor.typeId));
            data.Set("type", OpData(row.descriptor.typeName)); data.Set("callback", group == ScriptProfileGroup::TYPE ? OpData{} : OpData(scene::ScriptProfiler::GetCallbackName(row.descriptor.callbackKind)));
            data.Set("instance", group == ScriptProfileGroup::INSTANCE ? OpData(row.descriptor.inspectionId) : OpData{});
            const bool inclusiveAvailable = row.completedCalls != 0;
            const bool selfAvailable = inclusiveAvailable && row.selfAvailable;
            data.Set("inclusiveAvailable", OpData(inclusiveAvailable)); data.Set("inclusiveMs", inclusiveAvailable ? OpData(row.inclusiveMs) : OpData{});
            data.Set("selfAvailable", OpData(selfAvailable)); data.Set("selfMs", selfAvailable ? OpData(row.selfMs) : OpData{});
            data.Set("calls", Id(row.calls)); data.Set("faults", Id(row.faults)); data.Set("faultElapsedMs", OpData(row.faultElapsedMs));
            data.Set("avgPerCallMs", row.completedCalls ? OpData(row.avgPerCallMs) : OpData{}); data.Set("avgPerFrameMs", row.validFrames ? OpData(row.avgPerFrameMs) : OpData{});
            data.Set("maxCallMs", row.completedCalls ? OpData(row.maxCallMs) : OpData{}); data.Set("peakFrameMs", row.validFrames ? OpData(row.peakFrameMs) : OpData{});
            data.Set("validFrameCount", Id(row.validFrames)); data.Set("excludedFrameCount", Id(row.excludedFrames));
            data.Set("shareAvailable", OpData(row.shareAvailable)); data.Set("share", row.shareAvailable ? OpData(row.share) : OpData{}); output.Push(std::move(data));
        }
        truncated = rows.size() > limit; result.Set("rows", std::move(output));
    }
    result.Set("groupBy", OpData(groupBy)); result.Set("truncated", OpData(truncated));
    return OpResult::Data(std::move(result));
}
OpResult MemoryQuery(const OpArgs& args)
{
    const auto& history = GetMemoryHistory();
    bool valid = true; const auto* requested = RequestedFrame(history, args, valid);
    if (!valid) return OpResult::Err("BAD_ARG", "Specify both frame identifiers as decimal strings.");
    if (args.Has("captureSessionId") && requested == nullptr) return OpResult::Err("NOT_FOUND", "The requested memory frame is no longer retained.");
    MemoryProfileSnapshot empty;
    const auto& snapshot = requested != nullptr ? *requested : !history.frames.empty() ? *history.frames.back() : empty;
    auto result = Common(snapshot, history.historyEvictedFrameCount); result.Set("recording", OpData(IsMemoryHistoryRecording()));
    result.Set("retainedFrameCount", OpData(static_cast<int>(history.frames.size())));
    result.Set("trackingScope", OpData("Tracked allocations and GPU resources; process-wide memory is not measured."));
    auto tags = OpData::MakeArray();
    const size_t limit = static_cast<size_t>(args.GetInt("limit", 256));
    for (size_t i = 0; i < snapshot.tags.size() && i < limit; ++i) {
        const auto& tag = snapshot.tags[i];
        auto row = OpData::MakeObject(); row.Set("name", OpData(tag.name)); row.Set("usedBytes", Id(tag.stats.used)); row.Set("peakBytes", Id(tag.stats.peakUsed));
        row.Set("capacityBytes", Id(tag.stats.capacity)); row.Set("activeCount", Id(tag.stats.activeCount)); row.Set("allocationCount", Id(tag.stats.allocationCount)); row.Set("freeCount", Id(tag.stats.freeCount)); tags.Push(std::move(row));
    }
    result.Set("tags", std::move(tags)); result.Set("truncated", OpData(snapshot.tags.size() > limit)); return OpResult::Data(std::move(result));
}
OpParam StringParam(const char* name, std::vector<std::string> choices = {})
{
    OpParam param; param.name = name; param.type = OpParamType::String; param.required = false; param.enumValues = std::move(choices); return param;
}
const char* BenchmarkState(PerformanceBenchmarkState state)
{
    switch (state) {
    case PerformanceBenchmarkState::WARMUP: return "warmup";
    case PerformanceBenchmarkState::SAMPLING: return "sampling";
    case PerformanceBenchmarkState::COMPLETE: return "complete";
    case PerformanceBenchmarkState::FAILED: return "failed";
    case PerformanceBenchmarkState::CANCELLED: return "cancelled";
    default: return "idle";
    }
}
const char* BenchmarkFailure(PerformanceBenchmarkFailure failure)
{
    switch (failure) {
    case PerformanceBenchmarkFailure::SESSION_CHANGED: return "capture_session_changed";
    case PerformanceBenchmarkFailure::FRAME_ORDER_CHANGED: return "frame_serial_moved_backwards";
    case PerformanceBenchmarkFailure::RECORDING_STOPPED: return "performance_recording_stopped";
    default: return "";
    }
}
OpData BenchmarkStatus()
{
    const auto& capture = GetPerformanceBenchmarkCapture();
    const auto& status = capture.Status();
    auto data = OpData::MakeObject();
    data.Set("schemaVersion", OpData(1)); data.Set("label", OpData(capture.Label()));
    data.Set("state", OpData(BenchmarkState(status.state))); data.Set("active", OpData(capture.IsActive()));
    data.Set("failure", OpData(BenchmarkFailure(status.failure)));
    data.Set("captureSessionId", Id(status.captureSessionId));
    data.Set("warmupFrames", OpData(static_cast<int64_t>(status.warmupFrames)));
    data.Set("warmedFrames", OpData(static_cast<int64_t>(status.warmedFrames)));
    data.Set("sampleFrames", OpData(static_cast<int64_t>(status.sampleFrames)));
    data.Set("capturedFrames", OpData(static_cast<int64_t>(status.capturedFrames)));
    data.Set("incompleteFrames", OpData(static_cast<int64_t>(status.incompleteFrames)));
    data.Set("droppedSampleCount", Id(status.droppedSampleCount)); data.Set("missingFrameCount", Id(status.missingFrameCount));
    const auto report = capture.Report();
    auto conditions = OpData::MakeObject();
    conditions.Set("scenario", OpData(report.conditions.scenario)); conditions.Set("camera", OpData(report.conditions.camera));
    conditions.Set("configuration", OpData(report.conditions.configuration)); conditions.Set("declaredByCaller", OpData(true));
    conditions.Set("renderingMetadataAvailable", OpData(report.renderingMetadataAvailable));
    conditions.Set("width", OpData(report.width)); conditions.Set("height", OpData(report.height));
    conditions.Set("sceneGeneration", Id(report.sceneGeneration)); conditions.Set("changedWhileSampling", OpData(report.renderingConditionsChanged));
    data.Set("conditions", std::move(conditions)); data.Set("captureFilter", CaptureFilter(report.captureFilter));
    data.Set("comparison", BenchmarkComparison(report));
    return data;
}
OpData BenchmarkDistribution(const PerformanceBenchmarkDistribution& value)
{
    auto data = OpData::MakeObject();
    data.Set("available", OpData(value.count != 0)); data.Set("count", OpData(static_cast<int64_t>(value.count)));
    data.Set("p50Ms", value.count ? OpData(value.p50Ms) : OpData{});
    data.Set("p95Ms", value.count ? OpData(value.p95Ms) : OpData{});
    data.Set("p99Ms", value.count ? OpData(value.p99Ms) : OpData{});
    data.Set("maxMs", value.count ? OpData(value.maxMs) : OpData{});
    return data;
}
OpData BenchmarkPhysicsRow(const PhysicsBenchmarkRow& value)
{
    auto data = OpData::MakeObject();
    data.Set("complete", OpData(value.complete)); data.Set("stepCount", OpData(value.stepCount));
    data.Set("faultedScopeCount", OpData(value.faultedScopeCount));
    data.Set("abortedScopeCount", OpData(value.abortedScopeCount));
    data.Set("invalidScopeCount", OpData(value.invalidScopeCount));
    auto scopes = OpData::MakeObject();
    for (size_t index = 0; index < PHYSICS_BENCHMARK_SCOPE_COUNT; ++index) {
        const auto& sample = value.scopes[index];
        auto scope = OpData::MakeObject();
        scope.Set("calls", OpData(sample.calls)); scope.Set("totalMs", OpData(sample.totalMs));
        scopes.Set(PhysicsBenchmarkScopeName(static_cast<PhysicsBenchmarkScope>(index)), std::move(scope));
    }
    data.Set("scopes", std::move(scopes));
    return data;
}
OpData BenchmarkPhysicsSummary(const PhysicsBenchmarkSummary& value)
{
    auto data = OpData::MakeObject();
    data.Set("available", OpData(value.validFrames != 0));
    data.Set("timingDefinition", OpData("Completed timed Physics scopes in complete, undropped frames; World::Step calls count fixed steps, not internal substeps. Inclusive stage totals overlap PhysicsSystem and World::Step."));
    data.Set("perStepDistributionDefinition", OpData("Per-frame totalMs / completed World::Step calls for frames with at least one step. Zero-step frames contribute zero to frameMs only. meanMsPerStep weights all valid steps."));
    data.Set("summaryIncludesIncompleteFrames", OpData(false));
    data.Set("observedStepCount", Id(value.observedStepCount)); data.Set("stepCount", Id(value.stepCount));
    data.Set("validFrames", Id(value.validFrames)); data.Set("excludedFrames", Id(value.excludedFrames));
    data.Set("zeroStepFrames", Id(value.zeroStepFrames)); data.Set("multiStepFrames", Id(value.multiStepFrames));
    data.Set("faultedScopeCount", Id(value.faultedScopeCount));
    data.Set("abortedScopeCount", Id(value.abortedScopeCount)); data.Set("invalidScopeCount", Id(value.invalidScopeCount));
    auto scopes = OpData::MakeObject();
    for (size_t index = 0; index < PHYSICS_BENCHMARK_SCOPE_COUNT; ++index) {
        const auto& summary = value.scopes[index];
        auto scope = OpData::MakeObject();
        scope.Set("calls", Id(summary.calls)); scope.Set("totalMs", OpData(summary.totalMs));
        scope.Set("meanMsPerStepAvailable", OpData(value.stepCount != 0));
        scope.Set("meanMsPerStep", value.stepCount ? OpData(summary.totalMs / value.stepCount) : OpData{});
        scope.Set("frameMs", BenchmarkDistribution(summary.frameMs));
        scope.Set("msPerStep", BenchmarkDistribution(summary.msPerStep));
        scopes.Set(PhysicsBenchmarkScopeName(static_cast<PhysicsBenchmarkScope>(index)), std::move(scope));
    }
    data.Set("scopes", std::move(scopes));
    return data;
}
OpResult BenchmarkResult(const OpArgs& args)
{
    const auto& capture = GetPerformanceBenchmarkCapture();
    if (capture.Status().state != PerformanceBenchmarkState::COMPLETE)
        return OpResult::Err("NOT_COMPLETE", "The benchmark must complete before results are read.");
    const int offset = args.GetInt("offset", 0), count = args.GetInt("count", 256);
    if (offset < 0 || count <= 0 || static_cast<size_t>(offset) > capture.Rows().size())
        return OpResult::Err("BAD_ARG", "offset must address the result and count must be positive.");
    auto data = BenchmarkStatus();
    data.Set("timingDefinition", OpData("Completed application CPU frames; wall intervals include frame pacing. The capture window is independent of C# callbacks."));
    data.Set("percentileMethod", OpData("nearest_rank"));
    data.Set("summaryIncludesIncompleteFrames", OpData(true));
    data.Set("cpu", BenchmarkDistribution(capture.CpuSummary())); data.Set("wall", BenchmarkDistribution(capture.WallSummary()));
    data.Set("physics", BenchmarkPhysicsSummary(capture.PhysicsSummary()));
    data.Set("offset", OpData(offset));
    auto rows = OpData::MakeArray();
    const size_t first = static_cast<size_t>(offset);
    const size_t pageCount = (std::min)(static_cast<size_t>(count), capture.Rows().size() - first);
    for (size_t index = first; index < first + pageCount; ++index) {
        const auto& frame = capture.Rows()[index];
        auto row = OpData::MakeObject();
        row.Set("captureSessionId", Id(frame.captureSessionId)); row.Set("applicationFrameSerial", Id(frame.applicationFrameSerial));
        row.Set("complete", OpData(frame.complete)); row.Set("droppedSampleCount", OpData(frame.droppedSampleCount));
        row.Set("filteredSampleCount", OpData(frame.filteredSampleCount)); row.Set("recordedSampleCount", OpData(frame.recordedSampleCount));
        row.Set("sampleCapacity", OpData(frame.sampleCapacity));
        row.Set("cpuFrameElapsedAvailable", OpData(frame.cpuFrameElapsedAvailable));
        row.Set("cpuFrameElapsedMs", frame.cpuFrameElapsedAvailable ? OpData(frame.cpuFrameElapsedMs) : OpData{});
        row.Set("wallFrameIntervalAvailable", OpData(frame.wallFrameIntervalAvailable));
        row.Set("wallFrameIntervalMs", frame.wallFrameIntervalAvailable ? OpData(frame.wallFrameIntervalMs) : OpData{});
        row.Set("renderingAvailable", OpData(frame.renderingAvailable));
        row.Set("drawCalls", frame.renderingAvailable ? OpData(frame.drawCalls) : OpData{});
        row.Set("triangleCount", frame.renderingAvailable ? OpData(frame.triangleCount) : OpData{});
        row.Set("physics", BenchmarkPhysicsRow(frame.physics));
        rows.Push(std::move(row));
    }
    data.Set("rows", std::move(rows)); data.Set("count", OpData(static_cast<int64_t>(pageCount)));
    data.Set("nextOffset", OpData(static_cast<int64_t>(first + pageCount)));
    data.Set("hasMore", OpData(first + pageCount < capture.Rows().size()));
    return OpResult::Data(std::move(data));
}
}
void RegisterProfilerOperators(OperatorRegistry& registry)
{
    for (const bool script : {false, true}) {
        EditorOperator op; op.id = script ? "profiler.script.set_recording" : "profiler.performance.set_recording";
        op.label = script ? "Record Script Profiler" : "Record Performance Profiler"; op.category = "Profiler"; op.kind = OpKind::Action;
        OpParam recording; recording.name = "recording"; recording.type = OpParamType::Bool; op.params.push_back(recording);
        op.exec = [script](OpContext&, const OpArgs& args) {
            if (!script && GetPerformanceBenchmarkCapture().IsActive() && !args.GetBool("recording"))
                return OpResult::Err("BENCHMARK_ACTIVE", "Cancel the benchmark before stopping Performance recording.");
            if (script) scene::ScriptProfiler::RequestRecording(args.GetBool("recording"));
            else profiler::Profiler::RequestRecording(args.GetBool("recording"));
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "profiler.performance.set_capture_filter"; op.label = "Set CPU Collection Filter";
        op.category = "Profiler"; op.kind = OpKind::Action;
        op.params.push_back(StringParam("category")); op.params.push_back(StringParam("scope"));
        op.exec = [](OpContext&, const OpArgs& args) {
            if (GetPerformanceBenchmarkCapture().IsActive()) return OpResult::Err("BENCHMARK_ACTIVE", "Cancel the benchmark before changing its collection range.");
            if (!profiler::Profiler::RequestCaptureFilter({args.GetString("category"), args.GetString("scope")}))
                return OpResult::Err("BAD_ARG", "category is at most 128 bytes and scope at most 256 bytes.");
            return OpResult::Data(CaptureFilter(profiler::Profiler::GetRequestedCaptureFilter()));
        };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "profiler.benchmark.set_reference"; op.label = "Use Benchmark As Comparison Reference";
        op.category = "Profiler"; op.kind = OpKind::Action;
        op.exec = [](OpContext&, const OpArgs&) {
            if (!SetPerformanceBenchmarkReference()) return OpResult::Err("NOT_COMPLETE", "Complete a benchmark before retaining its comparison summary.");
            return OpResult::Data(BenchmarkStatus());
        };
        registry.Register(std::move(op));
    }
    for (const char* kind : {"performance", "script", "memory"}) {
        EditorOperator op; op.id = std::string("profiler.") + kind + ".snapshot"; op.label = std::string(kind) + " Profiler Snapshot"; op.category = "Profiler"; op.kind = OpKind::Query;
        op.params.push_back(StringParam("captureSessionId")); op.params.push_back(StringParam("applicationFrameSerial"));
        OpParam limit; limit.name = "limit"; limit.type = OpParamType::Int; limit.required = false; limit.defaultValue = 256; limit.hasRange = true; limit.minValue = 1; limit.maxValue = 8192; op.params.push_back(limit);
        const std::string domain = kind;
        if (domain == "script") {
            for (const char* filter : {"name", "type", "instance", "callback", "scene"}) op.params.push_back(StringParam(filter));
            op.params.push_back(StringParam("mode", {"edit", "play"}));
            op.params.push_back(StringParam("groupBy", {"type", "callback", "instance", "call_tree"}));
            op.params.push_back(StringParam("sort", {"self", "inclusive", "calls", "max_call", "name"}));
        }
        op.exec = [domain](OpContext&, const OpArgs& args) { return domain == "script" ? ScriptQuery(args) : domain == "performance" ? PerformanceQuery(args) : MemoryQuery(args); };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "profiler.benchmark.start"; op.label = "Start Benchmark Capture";
        op.category = "Profiler"; op.kind = OpKind::Action;
        op.desc = "Capture completed application frames after warmup; start the C# workload separately at its READY boundary.";
        auto label = StringParam("label"); label.required = true; op.params.push_back(std::move(label));
        for (const char* name : {"scenario", "camera", "configuration"}) op.params.push_back(StringParam(name));
        OpParam warmup; warmup.name = "warmupFrames"; warmup.type = OpParamType::Int; warmup.required = false; warmup.defaultValue = 0;
        warmup.desc = "Number of completed frames to skip, at least zero."; op.params.push_back(std::move(warmup));
        OpParam frames; frames.name = "sampleFrames"; frames.type = OpParamType::Int;
        frames.desc = "Number of completed frames to retain, greater than zero."; op.params.push_back(std::move(frames));
        op.exec = [](OpContext&, const OpArgs& args) {
            const int warmup = args.GetInt("warmupFrames"), frames = args.GetInt("sampleFrames");
            const std::string label = args.GetString("label");
            if (label.empty() || warmup < 0 || frames <= 0) return OpResult::Err("BAD_ARG", "label must be nonempty, warmupFrames nonnegative, and sampleFrames positive.");
            if (GetPerformanceBenchmarkCapture().IsActive()) return OpResult::Err("BENCHMARK_ACTIVE", "A benchmark capture is already active.");
            PerformanceBenchmarkConditions conditions{args.GetString("scenario"), args.GetString("camera"), args.GetString("configuration")};
            if (!StartPerformanceBenchmark(label, static_cast<size_t>(warmup), static_cast<size_t>(frames), std::move(conditions)))
                return OpResult::Err("BAD_ARG", "Warmup/sample count must be at most 60000; scenario/camera at most 256 bytes and configuration at most 1024 bytes.");
            return OpResult::Data(BenchmarkStatus());
        };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "profiler.benchmark.status"; op.label = "Benchmark Capture Status";
        op.category = "Profiler"; op.kind = OpKind::Query;
        op.exec = [](OpContext&, const OpArgs&) { return OpResult::Data(BenchmarkStatus()); };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "profiler.benchmark.result"; op.label = "Benchmark Capture Result";
        op.category = "Profiler"; op.kind = OpKind::Query;
        for (const char* name : {"offset", "count"}) {
            OpParam param; param.name = name; param.type = OpParamType::Int; param.required = false;
            param.defaultValue = std::string_view(name) == "offset" ? 0 : 256; op.params.push_back(std::move(param));
        }
        op.exec = [](OpContext&, const OpArgs& args) { return BenchmarkResult(args); };
        registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "profiler.benchmark.cancel"; op.label = "Cancel Benchmark Capture";
        op.category = "Profiler"; op.kind = OpKind::Action;
        op.exec = [](OpContext&, const OpArgs&) { CancelPerformanceBenchmark(); return OpResult::Data(BenchmarkStatus()); };
        registry.Register(std::move(op));
    }
}
}
