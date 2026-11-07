/// @file    WorldAuthoringPanel.cpp
/// @brief   World authoring controls share operation validation, transactions and streaming services with AI.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Editor/Panels/WorldAuthoringPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Components/WorldPcgAuthoringComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <imgui.h>
#include <algorithm>
#include <utility>

namespace fbzz::editor {
namespace {

template<std::size_t N>
void CopyText(char (&buffer)[N], const std::string& text)
{
    const auto count = std::min(text.size(), N - 1);
    std::copy_n(text.data(), count, buffer);
    buffer[count] = '\0';
}

math::Vector3 Vector(const float (&value)[3]) { return {value[0], value[1], value[2]}; }

const char* String(const OpData& data, const char* key)
{
    const auto* value = data.Find(key);
    return value && value->IsString() ? value->AsString().c_str() : "";
}

int Integer(const OpData& data, const char* key)
{
    const auto* value = data.Find(key);
    return value ? value->AsInt() : 0;
}

void DrawScalarSummary(const OpData& data)
{
    if (!data.IsObject()) return;
    for (const auto& [key, value] : data.AsObject()) {
        if (value.IsInt()) ImGui::Text("%s: %lld", key.c_str(), static_cast<long long>(value.AsInt64()));
        else if (value.IsBool()) ImGui::Text("%s: %s", key.c_str(), value.AsBool() ? "true" : "false");
    }
}

void DrawCellGrid(const OpData::Array& cells, int (&selected)[2])
{
    if (cells.empty()) return;
    int minX = Integer(cells.front(), "x"), maxX = minX;
    int minZ = Integer(cells.front(), "z"), maxZ = minZ;
    for (const auto& cell : cells) {
        minX = std::min(minX, Integer(cell, "x")); maxX = std::max(maxX, Integer(cell, "x"));
        minZ = std::min(minZ, Integer(cell, "z")); maxZ = std::max(maxZ, Integer(cell, "z"));
    }
    const float side = std::clamp(ImGui::GetContentRegionAvail().x, 80.0f, 260.0f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = static_cast<float>(static_cast<std::int64_t>(maxX) - minX + 1);
    const float height = static_cast<float>(static_cast<std::int64_t>(maxZ) - minZ + 1);
    const float step = side / std::max(width, height);
    ImGui::InvisibleButton("World cell grid", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, ImVec2(start.x + side, start.y + side), IM_COL32(30, 33, 38, 255));
    for (const auto& cell : cells) {
        const int x = Integer(cell, "x"), z = Integer(cell, "z");
        const ImVec2 low(start.x + static_cast<float>(static_cast<std::int64_t>(x) - minX) * step,
                         start.y + static_cast<float>(static_cast<std::int64_t>(z) - minZ) * step);
        const ImVec2 high(low.x + std::max(step - 1, 1.0f), low.y + std::max(step - 1, 1.0f));
        const std::string_view resident = String(cell, "resident"), state = String(cell, "state");
        const ImU32 color = state == "failed" ? IM_COL32(220, 87, 70, 255)
            : resident == "near" ? IM_COL32(68, 170, 124, 255)
            : resident == "hlod" ? IM_COL32(74, 132, 204, 255)
            : IM_COL32(89, 94, 107, 255);
        draw->AddRectFilled(low, high, color);
        if (selected[0] == x && selected[1] == z) draw->AddRect(low, high, IM_COL32(255, 221, 124, 255), 0.0f, 2.0f);
        if (hovered && ImGui::IsMouseHoveringRect(low, high)) {
            ImGui::SetTooltip("Cell %d, %d\n%s / %s\n%s", x, z, String(cell, "resident"), String(cell, "state"), String(cell, "diagnostic"));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { selected[0] = x; selected[1] = z; }
        }
    }
    ImGui::TextDisabled("Green: near   Blue: HLOD   Gray: unloaded   Red: failed");
}

} /// @note namespace

void WorldAuthoringPanel::ClearPreview()
{
    m_expected.clear();
    m_previewData = {};
}

void WorldAuthoringPanel::SyncContext(EditorContext& context)
{
    const auto generation = context.activeScene ? context.activeScene->GetRenderSceneGeneration() : 0;
    if (m_project == context.projectRoot && m_sceneGeneration == generation) return;
    if (m_project != context.projectRoot) {
        m_profile[0] = '\0'; m_profileText[0] = '\0'; m_profileExpected.clear();
    }
    m_project = context.projectRoot;
    m_sceneGeneration = generation;
    m_partition[0] = '\0'; m_volume[0] = '\0';
    m_partitionData = {}; m_streamData = {};
    m_message.clear(); m_error.clear(); m_streamError.clear(); m_details.clear();
    ClearPreview();
    m_initialized = false;
}

OpArgs WorldAuthoringPanel::PartitionArgs() const
{
    OpArgs args;
    if (m_partition[0]) args.Set("partition", std::string(m_partition));
    return args;
}

OpArgs WorldAuthoringPanel::PreviewArgs() const
{
    auto args = PartitionArgs();
    args.Set("volume", std::string(m_volume));
    args.Set("maxPoints", m_maxPoints);
    if (m_partialUpdate) {
        args.Set("boundsMin", Vector(m_boundsMin));
        args.Set("boundsMax", Vector(m_boundsMax));
    }
    return args;
}

bool WorldAuthoringPanel::Run(EditorContext& context, const char* operation, const OpArgs& args, OpData* output)
{
    auto result = InvokeOperator(context, operation, args);
    if (!result.ok) {
        m_error = result.errorCode + ": " + result.message;
        return false;
    }
    m_error.clear();
    m_message = result.message.empty() ? std::string(operation) + " completed" : result.message;
    if (output) *output = result.data;
    m_details.clear(); (void)WriteOpDataJson(result.data, m_details);
    return true;
}

bool WorldAuthoringPanel::ActionButton(EditorContext& context, const char* label, const char* operation,
                                       const OpArgs& args, OpData* output)
{
    ImGui::BeginDisabled(!CanInvokeOperator(context, operation, args));
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled();
    return clicked && Run(context, operation, args, output);
}

void WorldAuthoringPanel::ReadPartitionSettings(const OpData& data)
{
    if (const auto* value = data.Find("partition"); value && value->IsString()) CopyText(m_partition, value->AsString());
    if (const auto* value = data.Find("manifest"); value && value->IsString()) CopyText(m_manifest, value->AsString());
    if (const auto* value = data.Find("origin"); value && value->IsVec3()) {
        const auto origin = value->AsVec3(); m_origin[0] = origin.x; m_origin[1] = origin.y; m_origin[2] = origin.z;
    }
    const auto readFloat = [&data](const char* key, float& output) {
        if (const auto* value = data.Find(key); value && value->IsFloat()) output = value->AsFloat();
    };
    readFloat("cellSize", m_cellSize); readFloat("nearDistance", m_nearDistance);
    readFloat("hlodDistance", m_hlodDistance); readFloat("hysteresis", m_hysteresis);
    if (const auto* value = data.Find("maxConcurrentReads")) m_reads = value->AsInt(m_reads);
    if (const auto* value = data.Find("maxCommitsPerUpdate")) m_commits = value->AsInt(m_commits);
    if (const auto* value = data.Find("streamInEditor")) m_streamInEditor = value->AsBool(m_streamInEditor);
}

void WorldAuthoringPanel::InspectPartition(EditorContext& context)
{
    auto result = InvokeOperator(context, "worldcell.inspect", PartitionArgs());
    if (result.ok) { m_partitionData = result.data; ReadPartitionSettings(m_partitionData); }
    else if (result.errorCode != "NO_PARTITION") m_error = result.errorCode + ": " + result.message;
}

void WorldAuthoringPanel::RefreshStreaming(EditorContext& context)
{
    if (!CanInvokeOperator(context, "worldcell.stream_status", PartitionArgs())) return;
    const auto result = InvokeOperator(context, "worldcell.stream_status", PartitionArgs());
    if (result.ok) { m_streamData = result.data; m_streamError.clear(); }
    else m_streamError = result.errorCode + ": " + result.message;
}

void WorldAuthoringPanel::OnUpdate(EditorContext& context, float deltaTime)
{
    SyncContext(context);
    if (!visible) return;
    m_refreshClock += deltaTime;
    if (m_refreshClock >= 0.5f) { m_refreshClock = 0; RefreshStreaming(context); }
}

void WorldAuthoringPanel::DrawPartition(EditorContext& context)
{
    if (!ImGui::CollapsingHeader("World cells", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SetNextItemWidth(-110);
    if (ImGui::InputText("Partition", m_partition, sizeof(m_partition))) { ClearPreview(); m_partitionData = {}; }
    if (ImGui::Button("Use selected partition")) {
        if (auto* selected = context.GetSelectedGO()) { CopyText(m_partition, selected->instanceId); ClearPreview(); InspectPartition(context); }
    }
    ImGui::SameLine();
    if (ActionButton(context, "Inspect", "worldcell.inspect", PartitionArgs(), &m_partitionData)) ReadPartitionSettings(m_partitionData);
    ImGui::TextDisabled("Leave the target empty to resolve or create a partition.");
    if (!m_partitionData.IsNull()) {
        ImGui::Text("Persisted cells: %d", Integer(m_partitionData, "cells"));
        ImGui::TextWrapped("Manifest: %s", String(m_partitionData, "manifest"));
    }
    if (ImGui::TreeNode("Grid and loading distances")) {
        ImGui::InputText("Manifest path", m_manifest, sizeof(m_manifest));
        ImGui::InputFloat3("Grid origin (m)", m_origin);
        ImGui::InputFloat("Cell size (m)", &m_cellSize);
        ImGui::InputFloat("Near distance (m)", &m_nearDistance);
        ImGui::InputFloat("HLOD distance (m)", &m_hlodDistance);
        ImGui::InputFloat("Hysteresis (m)", &m_hysteresis);
        ImGui::InputInt("Concurrent reads", &m_reads);
        ImGui::InputInt("Commits per update", &m_commits);
        ImGui::Checkbox("Stream in editor", &m_streamInEditor);
        auto args = PartitionArgs();
        args.Set("manifest", std::string(m_manifest)); args.Set("origin", Vector(m_origin));
        args.Set("cellSize", m_cellSize); args.Set("nearDistance", m_nearDistance);
        args.Set("hlodDistance", m_hlodDistance); args.Set("hysteresis", m_hysteresis);
        args.Set("maxConcurrentReads", m_reads); args.Set("maxCommitsPerUpdate", m_commits);
        args.Set("streamInEditor", m_streamInEditor);
        if (ActionButton(context, "Apply grid settings", "worldcell.configure", args, &m_partitionData)) {
            ReadPartitionSettings(m_partitionData); ClearPreview();
        }
        ImGui::TreePop();
    }
    ImGui::Checkbox("Include scene content", &m_includeScene);
    ImGui::SameLine(); ImGui::Checkbox("Build HLOD on bake", &m_buildHlod);
    auto bake = PartitionArgs();
    bake.Set("includeScene", m_includeScene); bake.Set("buildHlod", m_buildHlod);
    bake.Set("maxCells", 4096); bake.Set("maxInstances", 1048576);
    if (ActionButton(context, "Bake cells", "worldcell.bake", bake)) InspectPartition(context);
    ImGui::SameLine();
    auto hlod = PartitionArgs(); hlod.Set("maxGroups", 65536); hlod.Set("maxInstances", 1048576);
    if (const auto* expected = m_partitionData.Find("expected"); expected && expected->IsString()) hlod.Set("expected", expected->AsString());
    if (ActionButton(context, "Build HLOD", "hlod.build", hlod)) InspectPartition(context);
}

void WorldAuthoringPanel::DrawPcg(EditorContext& context)
{
    if (!ImGui::CollapsingHeader("PCG", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::InputText("Volume", m_volume, sizeof(m_volume))) ClearPreview();
    if (ImGui::Button("Use selected volume")) {
        if (auto* selected = context.GetSelectedGO()) {
            CopyText(m_volume, selected->instanceId);
            if (const auto* volume = selected->GetComponent<scene::WorldPcgVolumeComponent>()) {
                CopyText(m_profile, volume->profilePath); m_overrideBounds = volume->overrideBounds;
                m_boundsMin[0] = volume->boundsMin.x; m_boundsMin[1] = volume->boundsMin.y; m_boundsMin[2] = volume->boundsMin.z;
                m_boundsMax[0] = volume->boundsMax.x; m_boundsMax[1] = volume->boundsMax.y; m_boundsMax[2] = volume->boundsMax.z;
                m_seed = volume->overrideSeed ? volume->seed : -1;
                m_protectManual = volume->protectManual; m_enabled = volume->enabled;
            }
            ClearPreview();
        }
    }
    if (ImGui::InputText("Profile path", m_profile, sizeof(m_profile))) { ClearPreview(); m_profileExpected.clear(); }
    if (ImGui::TreeNode("Volume settings")) {
        bool changed = ImGui::Checkbox("Override bounds", &m_overrideBounds);
        changed |= ImGui::InputFloat3("Bounds minimum (m)", m_boundsMin);
        changed |= ImGui::InputFloat3("Bounds maximum (m)", m_boundsMax);
        changed |= ImGui::InputInt("Seed (-1 = profile)", &m_seed);
        changed |= ImGui::Checkbox("Protect manual edits", &m_protectManual);
        changed |= ImGui::Checkbox("Enabled", &m_enabled);
        if (changed) ClearPreview();
        OpArgs args; if (m_volume[0]) args.Set("volume", std::string(m_volume));
        args.Set("profile", std::string(m_profile)); args.Set("seed", m_seed);
        args.Set("protectManual", m_protectManual); args.Set("enabled", m_enabled);
        if (m_overrideBounds) { args.Set("boundsMin", Vector(m_boundsMin)); args.Set("boundsMax", Vector(m_boundsMax)); }
        OpData configured;
        if (ActionButton(context, "Apply / create volume", "pcg.volume.configure", args, &configured)) {
            if (const auto* id = configured.Find("volume"); id && id->IsString()) CopyText(m_volume, id->AsString());
            ClearPreview();
        }
        ImGui::TreePop();
    }
    if (ImGui::Checkbox("Update only these bounds", &m_partialUpdate)) ClearPreview();
    if (m_partialUpdate) {
        bool changed = ImGui::InputFloat3("Update minimum (m)", m_boundsMin);
        changed |= ImGui::InputFloat3("Update maximum (m)", m_boundsMax);
        if (changed) ClearPreview();
    }
    if (ImGui::SliderInt("Preview samples", &m_maxPoints, 1, 1024)) ClearPreview();
    const auto previewArgs = PreviewArgs();
    ImGui::BeginDisabled(!m_volume[0]);
    if (ActionButton(context, "Preview", "pcg.preview", previewArgs, &m_previewData)) m_expected = String(m_previewData, "expected");
    ImGui::EndDisabled();
    ImGui::SameLine();
    auto generate = previewArgs; generate.Set("expected", m_expected);
    ImGui::BeginDisabled(m_expected.empty());
    if (ActionButton(context, "Generate", "pcg.generate", generate)) ClearPreview();
    ImGui::EndDisabled();
    if (!m_previewData.IsNull()) {
        ImGui::Text("Candidates %d  |  Instances %d  |  Groups %d  |  Cells %d",
            Integer(m_previewData, "candidateCount"), Integer(m_previewData, "instanceCount"),
            Integer(m_previewData, "groupCount"), Integer(m_previewData, "cellCount"));
        const int placementCap = Integer(m_previewData, "maxPlacementsPerCell");
        if (placementCap > 0) ImGui::Text("Placement cap %d per 128 m cell", placementCap);
        if (const auto* rejected = m_previewData.Find("rejected"); rejected && ImGui::TreeNode("Rejected points")) {
            DrawScalarSummary(*rejected); ImGui::TreePop();
        }
        if (const auto* points = m_previewData.Find("points"); points && points->IsArray() && ImGui::TreeNode("Preview positions")) {
            for (const auto& point : points->AsArray()) {
                if (const auto* position = point.Find("position"); position && position->IsVec3()) {
                    const auto value = position->AsVec3();
                    ImGui::Text("%s: %.1f, %.1f, %.1f", String(point, "rule"), value.x, value.y, value.z);
                }
            }
            ImGui::TreePop();
        }
    }
    if (!m_expected.empty()) ImGui::TextWrapped("Current preview: %s", m_expected.c_str());
    ImGui::TextDisabled("Preview again after editing a profile, volume or terrain.");
    auto* selected = context.GetSelectedGO();
    ImGui::BeginDisabled(!selected);
    OpArgs detach; if (selected) detach.Set("node", selected->instanceId);
    if (ActionButton(context, "Detach selected generated object", "pcg.detach", detach)) ClearPreview();
    ImGui::EndDisabled();

    if (ImGui::TreeNode("Profile text")) {
        ImGui::TextWrapped("Set maxPlacementsPerCell to cap the combined rules in each 128 m cell. Zero disables the cap.");
        OpArgs inspect; inspect.Set("path", std::string(m_profile));
        OpData profile;
        if (ActionButton(context, "Load profile / schema", "pcg.profile.inspect", inspect, &profile)) {
            const auto* text = profile.Find("text");
            if (!text || !text->IsString()) text = profile.Find("example");
            if (text && text->IsString() && text->AsString().size() < sizeof(m_profileText)) CopyText(m_profileText, text->AsString());
            else if (text && text->IsString()) {
                m_profileText[0] = '\0';
                m_error = "Profile exceeds the panel text limit. Edit the saved profile with an external editor.";
            }
            m_profileExpected = String(profile, "expected");
            if (m_profileExpected.empty()) m_profileExpected = String(profile, "fingerprint");
        }
        if (ImGui::InputTextMultiline("##profileText", m_profileText, sizeof(m_profileText), ImVec2(-1, 180))) ClearPreview();
        OpArgs save; save.Set("path", std::string(m_profile)); save.Set("text", std::string(m_profileText));
        if (!m_profileExpected.empty()) save.Set("expected", m_profileExpected);
        ImGui::BeginDisabled(!m_profile[0] || !m_profileText[0]);
        if (ActionButton(context, "Save profile", "pcg.profile.save", save, &profile)) {
            m_profileExpected = String(profile, "expected");
            if (m_profileExpected.empty()) m_profileExpected = String(profile, "fingerprint");
            ClearPreview();
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
}

void WorldAuthoringPanel::DrawStreaming(EditorContext& context)
{
    if (!ImGui::CollapsingHeader("Streaming", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::Button("Refresh streaming status")) RefreshStreaming(context);
    ImGui::Text("Near %d  |  HLOD %d  |  In flight %d", Integer(m_streamData, "near"), Integer(m_streamData, "hlod"), Integer(m_streamData, "inFlight"));
    ImGui::Text("Resident bytes: %s", String(m_streamData, "residentBytes"));
    if (const auto* source = m_streamData.Find("source"); source && source->IsVec3()) {
        const auto value = source->AsVec3(); ImGui::Text("Source (m): %.1f, %.1f, %.1f", value.x, value.y, value.z);
    }
    const char* diagnostic = String(m_streamData, "diagnostic");
    if (diagnostic[0]) ImGui::TextWrapped("%s", diagnostic);
    if (!m_streamError.empty()) ImGui::TextWrapped("%s", m_streamError.c_str());
    if (ImGui::Checkbox("Override source position", &m_overrideSource) && m_overrideSource) {
        if (const auto* source = m_streamData.Find("source"); source && source->IsVec3()) {
            const auto value = source->AsVec3(); m_source[0] = value.x; m_source[1] = value.y; m_source[2] = value.z;
        }
    }
    if (m_overrideSource) ImGui::InputFloat3("Source position (m)", m_source);
    ImGui::InputInt2("Cell (X, Z)", m_cell);
    const auto control = [this, &context](const char* label, const char* action) {
        auto args = PartitionArgs(); args.Set("action", std::string(action));
        args.Set("cellX", m_cell[0]); args.Set("cellZ", m_cell[1]);
        if (m_overrideSource) args.Set("source", Vector(m_source));
        if (ActionButton(context, label, "worldcell.stream_control", args)) RefreshStreaming(context);
    };
    control("Update", "update"); ImGui::SameLine(); control("Load near", "load_near"); ImGui::SameLine(); control("Load HLOD", "load_hlod");
    control("Unload", "unload"); ImGui::SameLine(); control("Clear override", "clear_override"); ImGui::SameLine(); control("Retry", "retry");
    control("Reset streaming", "reset");
    const auto* cells = m_streamData.Find("cells");
    if (cells && cells->IsArray()) DrawCellGrid(cells->AsArray(), m_cell);
    if (cells && cells->IsArray() && ImGui::BeginTable("World cell status", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 180))) {
        ImGui::TableSetupColumn("Cell"); ImGui::TableSetupColumn("Desired"); ImGui::TableSetupColumn("Resident");
        ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("Objects"); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(cells->AsArray().size()));
        while (clipper.Step()) for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto& cell = cells->AsArray()[static_cast<std::size_t>(index)];
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d, %d", Integer(cell, "x"), Integer(cell, "z"));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(String(cell, "desired"));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(String(cell, "resident"));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(String(cell, "state"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", String(cell, "diagnostic"));
            ImGui::TableNextColumn(); ImGui::Text("%d", Integer(cell, "objects"));
        }
        ImGui::EndTable();
    }
}

void WorldAuthoringPanel::OnRenderContent(EditorContext& context)
{
    SyncContext(context);
    if (!m_initialized && CanInvokeOperator(context, "worldcell.inspect", PartitionArgs())) {
        m_initialized = true; InspectPartition(context); RefreshStreaming(context);
    }
    ImGui::TextUnformatted("World / PCG");
    ImGui::TextDisabled("Configure cells, preview generation, then bake and stream.");
    if (!context.activeScene) ImGui::TextWrapped("Open a Scene to configure world cells and generation volumes.");
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.35f, 1));
        ImGui::TextWrapped("%s", m_error.c_str()); ImGui::PopStyleColor();
    } else if (!m_message.empty()) ImGui::TextWrapped("%s", m_message.c_str());
    DrawPartition(context);
    DrawPcg(context);
    DrawStreaming(context);
    ImGui::Separator();
    if (!m_details.empty() && ImGui::TreeNode("Last operation details")) {
        ImGui::BeginChild("World operation result", ImVec2(0, 160), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(m_details.c_str()); ImGui::EndChild(); ImGui::TreePop();
    }
}

} /// @note namespace fbzz::editor
