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
}
void RegisterProfilerOperators(OperatorRegistry& registry)
{
    for (const bool script : {false, true}) {
        EditorOperator op; op.id = script ? "profiler.script.set_recording" : "profiler.performance.set_recording";
        op.label = script ? "Record Script Profiler" : "Record Performance Profiler"; op.category = "Profiler"; op.kind = OpKind::Action;
        OpParam recording; recording.name = "recording"; recording.type = OpParamType::Bool; op.params.push_back(recording);
        op.exec = [script](OpContext&, const OpArgs& args) { if (script) scene::ScriptProfiler::RequestRecording(args.GetBool("recording")); else profiler::Profiler::RequestRecording(args.GetBool("recording")); return OpResult::Ok(); };
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
}
}
