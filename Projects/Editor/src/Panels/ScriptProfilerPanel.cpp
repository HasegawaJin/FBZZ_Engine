/// @file    ScriptProfilerPanel.cpp
/// @brief   所有した履歴の独立表示と世代検証済み実体への移動。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <Editor/Panels/ScriptProfilerPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>
#include <charconv>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
namespace fbzz::editor {
namespace {
void CellMs(double value, bool available = true)
{
    ImGui::TableNextColumn();
    if (available) ImGui::Text("%.3f", value);
    else ImGui::TextDisabled("unavailable");
}
void SetBuffer(char* buffer, size_t capacity, const std::string& value)
{
    std::snprintf(buffer, capacity, "%s", value.c_str());
}
const char* DisplayCallback(const scene::ScriptProfileDescriptor& descriptor)
{
    return descriptor.callbackKind == scene::ScriptCallbackKind::UNKNOWN || descriptor.callbackKind == scene::ScriptCallbackKind::EVENT_HANDLER
        ? descriptor.callbackLabel.c_str() : scene::ScriptProfiler::GetCallbackName(descriptor.callbackKind);
}
void DrawScriptCallTree(EditorContext& ctx, const scene::ScriptProfileSnapshot& snapshot, const ScriptProfileFilter& filter)
{
    if (!ImGui::BeginTable("ScriptCallTree", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, {0, -1})) return;
    for (const char* title : {"Call", "Object", "Inclusive ms", "Self ms", "Start ms", "Status"}) ImGui::TableSetupColumn(title);
    ImGui::TableHeadersRow();
    std::unordered_map<uint64_t, const profiler::ProfileSample*> index;
    std::unordered_set<uint64_t> included;
    for (const auto& sample : snapshot.samples) index.emplace(sample.sampleId, &sample);
    for (const auto& sample : snapshot.samples) {
        const auto* descriptor = FindScriptDescriptor(snapshot, sample.sampleKey);
        const auto* context = descriptor != nullptr ? FindScriptContext(snapshot, descriptor->executionContextId) : nullptr;
        if (context == nullptr || !MatchesScriptProfile(*descriptor, *context, filter)) continue;
        uint64_t id = sample.sampleId;
        while (id != 0 && included.insert(id).second) {
            const auto found = index.find(id);
            if (found == index.end()) break;
            id = found->second->parentSampleId;
        }
    }
    for (const auto& sample : snapshot.samples) {
        if (!included.contains(sample.sampleId)) continue;
        const auto* descriptor = FindScriptDescriptor(snapshot, sample.sampleKey);
        const auto* context = descriptor != nullptr ? FindScriptContext(snapshot, descriptor->executionContextId) : nullptr;
        if (context == nullptr) continue;
        ImGui::PushID(static_cast<int>(sample.sampleId));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const float indent = static_cast<float>(sample.depth) * ImGui::GetTreeNodeToLabelSpacing();
        ImGui::Indent(indent);
        ImGui::Text("%s :: %s", descriptor->typeName.c_str(), DisplayCallback(*descriptor));
        ImGui::Unindent(indent);
        ImGui::TableNextColumn();
        const bool exists = ctx.activeScene != nullptr && scene::ScriptProfiler::ResolveTarget(*ctx.activeScene, *descriptor, *context) != nullptr;
        ImGui::BeginDisabled(!exists);
        if (ImGui::Selectable(descriptor->objectName.c_str())) SelectEntity(ctx, descriptor->objectEntity);
        ImGui::EndDisabled();
        if (!exists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The target no longer exists in this runtime context.");
        CellMs(sample.inclusiveMs);
        CellMs(sample.selfMs, sample.selfAvailable);
        CellMs(sample.startOffsetMs, sample.startAvailable);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sample.status == profiler::ProfileSampleStatus::COMPLETE ? "complete" : sample.status == profiler::ProfileSampleStatus::FAULTED ? "faulted" : "aborted");
        ImGui::PopID();
    }
    ImGui::EndTable();
}
}
void ScriptProfilerPanel::OnRenderContent(EditorContext& ctx)
{
    auto& history = GetScriptHistory();
    bool recording = scene::ScriptProfiler::IsRecording();
    if (ImGui::Checkbox("Record", &recording)) {
        OpArgs args; args.Set("recording", recording);
        InvokeOperator(ctx, "profiler.script.set_recording", args);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Freeze", &history.frozen);
    ImGui::SameLine();
    if (ImGui::Button("Capture") && !history.frames.empty()) { history.selected = history.frames.back(); history.frozen = true; }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { ClearScriptHistory(); return; }
    if (history.budgetBlocked) ImGui::TextDisabled("History paused: retained snapshots reached the 64 MiB budget.");
    if (!history.selected) { ImGui::TextDisabled(recording ? "Waiting for a completed capture." : "Enable Record to collect script callbacks."); return; }
    auto selected = history.selected;
    ImGui::Text("Session %llu / Frame %llu / %u samples / %u dropped / %llu evicted",
        static_cast<unsigned long long>(selected->captureSessionId), static_cast<unsigned long long>(selected->applicationFrameSerial),
        selected->recordedSampleCount, selected->droppedSampleCount, static_cast<unsigned long long>(history.historyEvictedFrameCount));
    if (!selected->complete) ImGui::TextDisabled("Incomplete capture: totals and Self may be unavailable.");
    if (selected->transitionFrame) ImGui::TextDisabled("Runtime transition: excluded from frame averages.");
    if (!history.frames.empty()) {
        int selectedIndex = static_cast<int>(history.frames.size() - 1);
        for (size_t i = 0; i < history.frames.size(); ++i) if (history.frames[i] == selected) selectedIndex = static_cast<int>(i);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::SliderInt("Frame", &selectedIndex, 0, static_cast<int>(history.frames.size() - 1))) { history.selected = history.frames[static_cast<size_t>(selectedIndex)]; history.frozen = true; selected = history.selected; }
    }
    const char* groupNames[] = {"Type", "Callback", "Instance"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    ImGui::Combo("Group", &m_group, groupNames, 3);
    ImGui::SameLine(); ImGui::Checkbox("Call Tree", &m_callTree);
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::SliderInt("Range", &m_range, 1, 240, "%d frames");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::InputTextWithHint("##ScriptSearch", "Name...", m_name, sizeof(m_name));
    ImGui::SameLine();
    const char* modeNames[] = {"All modes", "edit", "play"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f); ImGui::Combo("Mode", &m_mode, modeNames, 3);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f); ImGui::InputText("Type", m_type, sizeof(m_type));
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f); ImGui::InputText("Callback", m_callback, sizeof(m_callback));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f); ImGui::InputText("Instance", m_instance, sizeof(m_instance));
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f); ImGui::InputText("Scene generation", m_scene, sizeof(m_scene));
    ScriptProfileFilter filter;
    filter.name = m_name; filter.type = m_type; filter.instance = m_instance; filter.callback = m_callback;
    if (m_mode != 0) filter.mode = modeNames[m_mode];
    if (m_scene[0] != '\0') {
        const auto parsed = std::from_chars(m_scene, m_scene + std::strlen(m_scene), filter.sceneGeneration);
        if (parsed.ec != std::errc{} || parsed.ptr != m_scene + std::strlen(m_scene)) { ImGui::TextDisabled("Enter a numeric scene generation."); return; }
    }
    if (m_callTree) { DrawScriptCallTree(ctx, *selected, filter); return; }
    std::vector<const scene::ScriptProfileSnapshot*> range;
    for (auto it = history.frames.rbegin(); it != history.frames.rend(); ++it) {
        if ((*it)->captureSessionId != selected->captureSessionId || (*it)->applicationFrameSerial > selected->applicationFrameSerial) continue;
        range.push_back(it->get());
        if (range.size() >= static_cast<size_t>(m_range)) break;
    }
    auto rows = AggregateScriptProfiles(range, static_cast<ScriptProfileGroup>(m_group), filter);
    std::vector<float> graph;
    for (auto it = range.rbegin(); it != range.rend(); ++it) {
        const auto& frame = **it;
        if (!frame.complete || frame.transitionFrame) continue;
        double covered = 0.0;
        for (const auto& sample : frame.samples) if (sample.parentSampleId == 0 && sample.status == profiler::ProfileSampleStatus::COMPLETE) covered += sample.inclusiveMs;
        graph.push_back(static_cast<float>(covered));
    }
    if (!graph.empty()) ImGui::PlotLines("Script coverage ms", graph.data(), static_cast<int>(graph.size()), 0, nullptr, 0, FLT_MAX, {0, ImGui::GetFontSize() * 3.0f});
    if (!ImGui::BeginTable("ScriptSummary", 12, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, {0, -1})) return;
    for (const char* title : {"Type / Callback / Instance", "Context", "Inclusive ms", "Self ms", "Calls", "Faults", "Avg/call ms", "Avg/frame ms", "Max call ms", "Peak frame ms", "Share", "Frames / excluded"}) ImGui::TableSetupColumn(title);
    ImGui::TableHeadersRow();
    const int renderedGroup = m_group;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const auto& descriptor = row.descriptor;
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        const std::string label = renderedGroup == 0 ? descriptor.typeName : renderedGroup == 1 ? descriptor.typeName + " :: " + DisplayCallback(descriptor) : descriptor.objectName + " :: " + DisplayCallback(descriptor);
        if (ImGui::Selectable(label.c_str())) {
            if (renderedGroup != 2) { SetBuffer(m_type, sizeof(m_type), descriptor.typeName); if (renderedGroup == 1) SetBuffer(m_callback, sizeof(m_callback), scene::ScriptProfiler::GetCallbackName(descriptor.callbackKind)); m_group = 2; }
            else if (ctx.activeScene != nullptr && scene::ScriptProfiler::ResolveTarget(*ctx.activeScene, descriptor, row.context)) SelectEntity(ctx, descriptor.objectEntity);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nInstance %s\nScript %s\nFault/aborted elapsed %.3f ms", descriptor.objectName.c_str(), descriptor.instanceId.c_str(), descriptor.inspectionId.c_str(), row.faultElapsedMs);
        ImGui::TableNextColumn(); ImGui::Text("%s / epoch %llu / DLL %llu / scene %llu", scene::ScriptProfiler::GetModeName(row.context.mode), static_cast<unsigned long long>(row.context.runtimeEpoch), static_cast<unsigned long long>(row.context.dllGeneration), static_cast<unsigned long long>(row.context.sceneGeneration));
        CellMs(row.inclusiveMs, row.completedCalls != 0); CellMs(row.selfMs, row.selfAvailable && row.completedCalls != 0);
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.calls));
        ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.faults));
        CellMs(row.avgPerCallMs, row.completedCalls != 0); CellMs(row.avgPerFrameMs, row.validFrames != 0);
        CellMs(row.maxCallMs, row.completedCalls != 0); CellMs(row.peakFrameMs, row.validFrames != 0);
        ImGui::TableNextColumn(); if (row.shareAvailable) ImGui::Text("%.1f%%", row.share * 100.0); else ImGui::TextDisabled("unavailable");
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", static_cast<unsigned long long>(row.validFrames), static_cast<unsigned long long>(row.excludedFrames));
        ImGui::PopID();
    }
    ImGui::EndTable();
}
void ScriptProfilerPanel::OnLoadSettings(const EditorSettings& settings)
{
    m_group = std::clamp(settings.scriptProfilerGroup, 0, 2);
    m_mode = std::clamp(settings.scriptProfilerMode, 0, 2);
    m_range = std::clamp(settings.scriptProfilerRange, 1, 240);
    SetBuffer(m_name, sizeof(m_name), settings.scriptProfilerNameFilter);
    SetBuffer(m_type, sizeof(m_type), settings.scriptProfilerTypeFilter);
    SetBuffer(m_instance, sizeof(m_instance), settings.scriptProfilerInstanceFilter);
    SetBuffer(m_callback, sizeof(m_callback), settings.scriptProfilerCallbackFilter);
    SetBuffer(m_scene, sizeof(m_scene), settings.scriptProfilerSceneFilter);
    scene::ScriptProfiler::RequestRecording(settings.scriptProfilerRecording);
}
void ScriptProfilerPanel::OnSaveSettings(EditorSettings& settings) const
{
    settings.scriptProfilerGroup = m_group; settings.scriptProfilerMode = m_mode; settings.scriptProfilerRange = m_range;
    settings.scriptProfilerNameFilter = m_name; settings.scriptProfilerTypeFilter = m_type;
    settings.scriptProfilerInstanceFilter = m_instance; settings.scriptProfilerCallbackFilter = m_callback;
    settings.scriptProfilerSceneFilter = m_scene;
    settings.scriptProfilerRecording = scene::ScriptProfiler::IsRecording();
}
}
