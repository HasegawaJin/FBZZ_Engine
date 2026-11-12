/// @file    WorldAuthoringPanel.cpp
/// @brief   World authoring controls share operation validation, transactions and streaming services with AI.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Editor/Panels/WorldAuthoringPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/UndoStack.hpp>
#include "../Tools/WorldAuthoring.hpp"
#include <Engine/Scene/Components/WorldPcgAuthoringComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace fbzz::editor {
namespace {

constexpr auto PROFILE_PICKER_HOST = widgets::AssetPickerHost::WORLD_PCG;
constexpr std::size_t MAX_PROFILE_RULES = 256;
constexpr float LIVE_PREVIEW_DELAY = 0.25f;

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

bool EditProfileLabel(const char* label, std::string& value)
{
    char text[257]{};
    CopyText(text, value);
    if (!ImGui::InputText(label, text, sizeof(text))) return false;
    value = text;
    return true;
}

bool EditProfileRange(const char* label, float& minimum, float& maximum)
{
    float values[]{minimum, maximum};
    if (!ImGui::InputFloat2(label, values)) return false;
    minimum = values[0];
    maximum = values[1];
    return true;
}

void AppendProfileRule(scene::WorldPcgProfile& profile, scene::WorldPcgRule rule)
{
    for (std::size_t number = 1; number <= MAX_PROFILE_RULES; ++number) {
        auto key = "rule_" + std::to_string(number);
        const auto hasKey = [&](const auto& existing) { return existing.key == key; };
        if (std::any_of(profile.rules.begin(), profile.rules.end(), hasKey)) continue;
        rule.key = std::move(key);
        profile.rules.push_back(std::move(rule));
        return;
    }
}

bool DrawRulePresentation(EditorContext& context, scene::WorldPcgRule& rule)
{
    bool changed = false;
    if (rule.parts.empty()) {
        changed |= widgets::AssetPathField("Placement asset", rule.prototypeRef, ".terrainpaint,.meshinstances,.fzdata",
            context.projectRoot, PROFILE_PICKER_HOST);
    }
    if (!ImGui::TreeNode("Mesh parts and LOD")) return changed;
    if (rule.parts.empty()) ImGui::TextDisabled("The placement asset supplies mesh parts and LODs.");
    else {
        for (std::size_t index = 0; index < rule.lodHeights.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            changed |= ImGui::InputFloat("LOD screen height", &rule.lodHeights[index], 0, 0, "%.4f");
            ImGui::PopID();
        }
        ImGui::BeginDisabled(rule.lodHeights.size() >= 4);
        if (ImGui::SmallButton("Add LOD")) {
            auto& heights = rule.lodHeights;
            if (heights.back() == 0.0f) {
                const float previous = heights.size() == 1 ? 0.2f : heights[heights.size() - 2];
                heights.back() = previous * 0.5f;
            }
            heights.push_back(0.0f);
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(rule.lodHeights.size() <= 1);
        if (ImGui::SmallButton("Remove last LOD")) {
            const auto last = rule.lodHeights.size() - 1;
            std::erase_if(rule.parts, [&](const auto& part) { return part.lod == last; });
            rule.lodHeights.pop_back();
            changed = true;
        }
        ImGui::EndDisabled();
    }
    std::size_t removePart = rule.parts.size();
    for (std::size_t index = 0; index < rule.parts.size(); ++index) {
        auto& part = rule.parts[index];
        ImGui::PushID(static_cast<int>(index));
        const std::string title = "Part " + std::to_string(index + 1);
        if (ImGui::TreeNode(title.c_str())) {
            changed |= widgets::AssetPathField("Mesh", part.meshPath, ".mesh,.fbx,.obj,.gltf,.glb,.fzasset",
                context.projectRoot, PROFILE_PICKER_HOST);
            changed |= widgets::AssetPathField("Material", part.materialPath, ".mat", context.projectRoot, PROFILE_PICKER_HOST);
            int lod = static_cast<int>(part.lod);
            if (ImGui::SliderInt("LOD", &lod, 0, static_cast<int>(rule.lodHeights.size()) - 1)) {
                part.lod = static_cast<std::uint32_t>(lod);
                changed = true;
            }
            if (ImGui::SmallButton("Remove part")) removePart = index;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removePart < rule.parts.size()) {
        rule.parts.erase(rule.parts.begin() + static_cast<std::ptrdiff_t>(removePart));
        changed = true;
    }
    ImGui::BeginDisabled(rule.parts.size() >= 64);
    if (ImGui::SmallButton("Add mesh part")) {
        if (rule.parts.empty()) rule.lodHeights = {0.0f};
        rule.parts.emplace_back();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::TreePop();
    return changed;
}

bool DrawRuleConditions(EditorContext& context, scene::WorldPcgRule& rule, const scene::WorldPcgProfile& profile)
{
    bool changed = false;
    if (ImGui::TreeNode("Ground and water")) {
        changed |= EditProfileLabel("Biome", rule.biome);
        changed |= EditProfileRange("Elevation (m)", rule.minHeight, rule.maxHeight);
        changed |= EditProfileRange("Slope (deg)", rule.minSlope, rule.maxSlope);
        changed |= ImGui::InputFloat("Elevation fade (m)", &rule.heightFade);
        changed |= ImGui::InputFloat("Slope fade (deg)", &rule.slopeFade);
        changed |= EditProfileRange("Shore distance (m)", rule.minWaterDistance, rule.maxWaterDistance);
        changed |= ImGui::InputFloat("Shore distance fade (m)", &rule.waterFade);
        if (ImGui::Checkbox("Filter by water level", &rule.useWaterLevel)) {
            if (!rule.useWaterLevel) rule.waterLevelFade = 0.0f;
            changed = true;
        }
        if (rule.useWaterLevel) {
            changed |= EditProfileRange("Height from water (m)", rule.minWaterOffset, rule.maxWaterOffset);
            changed |= ImGui::InputFloat("Water level fade (m)", &rule.waterLevelFade);
            ImGui::TextDisabled("Negative heights select submerged ground.");
        }
        changed |= ImGui::InputFloat("Distance below cliff (m)", &rule.maxCliffDistance);
        if (rule.maxCliffDistance > 0.0f) changed |= ImGui::InputFloat("Scale at far edge", &rule.cliffFarScale);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Terrain materials")) {
        changed |= widgets::AssetPathField("Required material", rule.includeMaterial, ".mat", context.projectRoot, PROFILE_PICKER_HOST);
        if (!rule.includeMaterial.empty()) changed |= ImGui::InputFloat("Minimum material weight", &rule.minMaterialWeight);
        changed |= widgets::AssetPathField("Excluded material", rule.excludeMaterial, ".mat", context.projectRoot, PROFILE_PICKER_HOST);
        if (!rule.excludeMaterial.empty()) changed |= ImGui::InputFloat("Excluded material weight", &rule.excludeMaterialWeight);
        if (!profile.pathMaterial.empty() || rule.UsesPathBand()) {
            changed |= EditProfileRange("Path distance (m)", rule.minPathDistance, rule.maxPathDistance);
            changed |= ImGui::InputFloat("Path fade (m)", &rule.pathFade);
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Clustering and neighbors")) {
        changed |= ImGui::InputFloat("Cluster strength", &rule.clusterStrength);
        changed |= ImGui::InputFloat("Cluster size (m)", &rule.noiseScale);
        changed |= EditProfileLabel("Shared patch group", rule.patchGroup);
        changed |= EditProfileRange("Patch range", rule.minPatch, rule.maxPatch);
        if (ImGui::BeginCombo("Neighbor rule", rule.neighborRule.empty() ? "None" : rule.neighborRule.c_str())) {
            if (ImGui::Selectable("None", rule.neighborRule.empty()) && !rule.neighborRule.empty()) {
                rule.neighborRule.clear(); rule.minNeighborDistance = 0.0f; rule.maxNeighborDistance = 0.0f;
                changed = true;
            }
            for (const auto& other : profile.rules) {
                if (&other == &rule) continue;
                ImGui::PushID(other.key.c_str());
                const auto& label = other.name.empty() ? other.key : other.name;
                if (ImGui::Selectable(label.c_str(), rule.neighborRule == other.key)) {
                    if (rule.neighborRule.empty()) rule.maxNeighborDistance = 6.0f;
                    rule.neighborRule = other.key;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (!rule.neighborRule.empty())
            changed |= EditProfileRange("Neighbor distance (m)", rule.minNeighborDistance, rule.maxNeighborDistance);
        ImGui::TreePop();
    }
    return changed;
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
    m_previewStatus.clear();
    m_previewPending = true;
    m_previewDelay = LIVE_PREVIEW_DELAY;
}

void WorldAuthoringPanel::SyncContext(EditorContext& context)
{
    const auto generation = context.activeScene ? context.activeScene->GetRenderSceneGeneration() : 0;
    if (m_project == context.projectRoot && m_sceneGeneration == generation) return;
    if (m_project != context.projectRoot) {
        m_profile.clear(); m_profileFile.clear(); m_profileLoadedPath.clear(); m_profileExpected.clear();
        m_profileDraft.reset(); m_profileText[0] = '\0'; m_editProfileText = false; m_profileDirty = false;
        m_selectedRule = 0; m_rockSource.clear(); m_pebbleSource.clear();
    }
    m_project = context.projectRoot;
    m_sceneGeneration = generation;
    m_partition[0] = '\0'; m_volume[0] = '\0';
    m_volumeDraft = false; m_partialUpdate = false;
    m_debugRule.clear(); m_debugReason.clear();
    m_partitionData = {}; m_streamData = {};
    m_volumeBounds = {};
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
        args.Set("boundsMin", Vector(m_updateBoundsMin));
        args.Set("boundsMax", Vector(m_updateBoundsMax));
    }
    return args;
}

bool WorldAuthoringPanel::Run(EditorContext& context, const char* operation, const OpArgs& args, OpData* output)
{
    auto result = InvokeOperator(context, operation, args);
    if (!result.ok) {
        if (std::string_view(operation) == "pcg.volume.inspect") m_volumeBounds = {};
        m_error = result.errorCode + ": " + result.message;
        return false;
    }
    m_error.clear();
    m_message = result.message.empty() ? std::string(operation) + " completed" : result.message;
    if (output) *output = result.data;
    if (std::string_view(operation) == "pcg.volume.inspect") {
        const auto revision = context.undoStack ? context.undoStack->GetRevision() : 0;
        if (revision != m_previewUndoRevision) ClearPreview();
        m_previewUndoRevision = revision;
    }
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
    if (context.undoStack && m_previewUndoRevision != context.undoStack->GetRevision()) {
        ClearPreview();
        m_volumeBounds = {};
        m_previewUndoRevision = context.undoStack->GetRevision();
    }
    if (!visible) return;
    if (m_livePreview && m_previewPending && m_volume[0]) {
        m_previewDelay -= (std::max)(0.0f, deltaTime);
        if (m_previewDelay <= 0.0f && CanInvokeOperator(context, "pcg.preview", PreviewArgs())) RefreshPcgPreview(context, true);
    }
    m_refreshClock += deltaTime;
    if (m_refreshClock >= 0.5f) { m_refreshClock = 0; RefreshStreaming(context); }
}

void WorldAuthoringPanel::RefreshPcgPreview(EditorContext& context, bool automatic)
{
    m_previewPending = false;
    m_previewStatus.clear();
    m_expected.clear();
    m_previewData = {};
    const auto* owner = context.activeScene ? context.activeScene->FindByGuid(m_volume) : nullptr;
    const auto* volume = owner ? context.activeScene->GetComponent<scene::WorldPcgVolumeComponent>(owner->GetID()) : nullptr;
    if (!volume) { m_previewStatus = "Select a PCG volume."; return; }

    auto args = PreviewArgs();
    args.Set("maxRejectedPoints", m_maxPoints);
    if (automatic) args.Set("live", true);
    const auto& profile = m_volumeDraft ? m_profile : volume->profilePath;
    if (m_volumeDraft) {
        if (profile != volume->profilePath && !SameWorldPcgProfile(context, profile, volume->profilePath))
            args.Set("draftProfilePath", profile);
        const int savedSeed = volume->overrideSeed ? volume->seed : -1;
        if (m_seed != savedSeed) args.Set("draftSeed", m_seed);
        if (m_enabled != volume->enabled) args.Set("draftEnabled", m_enabled);
        if (m_protectManual != volume->protectManual) args.Set("draftProtectManual", m_protectManual);
        if (m_overrideBounds && (!volume->overrideBounds || Vector(m_boundsMin) != volume->boundsMin || Vector(m_boundsMax) != volume->boundsMax)) {
            args.Set("draftBoundsMin", Vector(m_boundsMin)); args.Set("draftBoundsMax", Vector(m_boundsMax));
        } else if (!m_overrideBounds && volume->overrideBounds) args.Set("draftUseProfileBounds", true);
    }
    if (m_profileDirty && SameWorldPcgProfile(context, m_profileFile, profile)) {
        std::string text;
        if (m_editProfileText) text = m_profileText;
        else if (!m_profileDraft || !scene::SerializeWorldPcgProfile(*m_profileDraft, text, m_previewStatus)) return;
        args.Set("draftProfileText", std::move(text));
    }

    auto result = InvokeOperator(context, "pcg.preview", args);
    if (!result.ok) {
        m_previewStatus = result.errorCode == "PCG_LIVE_BUDGET" ? "Live preview paused. " + result.message : result.message;
        return;
    }
    m_previewData = std::move(result.data);
    m_expected = String(m_previewData, "expected");
    m_previewUndoRevision = context.undoStack ? context.undoStack->GetRevision() : 0;
    if (const auto* rules = m_previewData.Find("ruleCounts"); rules && !rules->Find(m_debugRule)) m_debugRule.clear();
    if (!automatic) {
        m_details.clear();
        (void)WriteOpDataJson(m_previewData, m_details);
    }
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
    ImGui::Checkbox("Stream terrain tiles", &m_includeTerrain);
    ImGui::Checkbox("Stream near light probes", &m_includeLightProbes);
    if (m_includeTerrain) {
        ImGui::Checkbox("Render-only HLOD terrain", &m_omitTerrainCollisionInHlod);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable only when physics actors stay in near cells.");
    }
    auto bake = PartitionArgs();
    bake.Set("includeScene", m_includeScene); bake.Set("buildHlod", m_buildHlod);
    bake.Set("includeTerrain", m_includeTerrain);
    bake.Set("includeLightProbes", m_includeLightProbes);
    bake.Set("omitTerrainCollisionInHlod", m_omitTerrainCollisionInHlod);
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
    if (ImGui::InputText("Volume", m_volume, sizeof(m_volume))) { ClearPreview(); m_volumeBounds = {}; m_volumeDraft = false; }
    if (ImGui::Button("Use selected volume")) {
        if (auto* selected = context.GetSelectedGO()) {
            CopyText(m_volume, selected->instanceId);
            m_volumeDraft = false;
            if (const auto* volume = selected->GetComponent<scene::WorldPcgVolumeComponent>()) {
                m_profile = volume->profilePath; m_overrideBounds = volume->overrideBounds;
                if (m_profileFile.empty() && !m_profileDirty) m_profileFile = m_profile;
                m_boundsMin[0] = volume->boundsMin.x; m_boundsMin[1] = volume->boundsMin.y; m_boundsMin[2] = volume->boundsMin.z;
                m_boundsMax[0] = volume->boundsMax.x; m_boundsMax[1] = volume->boundsMax.y; m_boundsMax[2] = volume->boundsMax.z;
                m_seed = volume->overrideSeed ? volume->seed : -1;
                m_protectManual = volume->protectManual; m_enabled = volume->enabled;
            }
            ClearPreview();
            m_volumeBounds = {};
            if (selected->GetComponent<scene::WorldPcgVolumeComponent>()) {
                OpArgs inspect; inspect.Set("volume", selected->instanceId);
                Run(context, "pcg.volume.inspect", inspect, &m_volumeBounds);
            }
        }
    }
    DrawPcgProfile(context);
    if (ImGui::TreeNode("Volume settings")) {
        bool changed = widgets::AssetPathField("Profile", m_profile, ".pcg", context.projectRoot, PROFILE_PICKER_HOST);
        changed |= ImGui::Checkbox("Override bounds", &m_overrideBounds);
        changed |= ImGui::InputFloat3("Bounds minimum (m)", m_boundsMin);
        changed |= ImGui::InputFloat3("Bounds maximum (m)", m_boundsMax);
        changed |= ImGui::InputInt("Seed (-1 = profile)", &m_seed);
        changed |= ImGui::Checkbox("Protect manual edits", &m_protectManual);
        changed |= ImGui::Checkbox("Enabled", &m_enabled);
        if (changed) { m_volumeDraft = true; ClearPreview(); }
        OpArgs args; if (m_volume[0]) args.Set("volume", std::string(m_volume));
        args.Set("profile", m_profile); args.Set("seed", m_seed);
        args.Set("protectManual", m_protectManual); args.Set("enabled", m_enabled);
        if (m_overrideBounds) { args.Set("boundsMin", Vector(m_boundsMin)); args.Set("boundsMax", Vector(m_boundsMax)); }
        else args.Set("useProfileBounds", true);
        OpData configured;
        if (ActionButton(context, "Apply / create volume", "pcg.volume.configure", args, &configured)) {
            if (const auto* id = configured.Find("volume"); id && id->IsString()) CopyText(m_volume, id->AsString());
            m_volumeDraft = false;
            ClearPreview();
            m_volumeBounds = {};
            OpArgs inspect; inspect.Set("volume", std::string(m_volume));
            Run(context, "pcg.volume.inspect", inspect, &m_volumeBounds);
        }
        ImGui::TreePop();
    }
    if (ImGui::Checkbox("Update only these bounds", &m_partialUpdate)) {
        if (m_partialUpdate) {
            const auto* minimum = m_volumeBounds.Find("boundsMin");
            const auto* maximum = m_volumeBounds.Find("boundsMax");
            const auto low = minimum && !m_volumeDraft ? minimum->AsVec3() : Vector(m_boundsMin);
            const auto high = maximum && !m_volumeDraft ? maximum->AsVec3() : Vector(m_boundsMax);
            m_updateBoundsMin[0] = low.x; m_updateBoundsMin[1] = low.y; m_updateBoundsMin[2] = low.z;
            m_updateBoundsMax[0] = high.x; m_updateBoundsMax[1] = high.y; m_updateBoundsMax[2] = high.z;
        }
        ClearPreview();
    }
    if (m_partialUpdate) {
        bool changed = ImGui::InputFloat3("Update minimum (m)", m_updateBoundsMin);
        changed |= ImGui::InputFloat3("Update maximum (m)", m_updateBoundsMax);
        if (changed) ClearPreview();
    }
    if (ImGui::SliderInt("Preview samples", &m_maxPoints, 1, 1024)) ClearPreview();
    if (ImGui::Checkbox("Live preview", &m_livePreview) && m_livePreview) ClearPreview();
    const auto previewArgs = PreviewArgs();
    ImGui::BeginDisabled(!m_volume[0] || !CanInvokeOperator(context, "pcg.preview", previewArgs));
    if (ImGui::Button("Preview")) RefreshPcgPreview(context, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    auto generate = previewArgs; generate.Set("expected", m_expected);
    ImGui::BeginDisabled(m_expected.empty());
    if (ActionButton(context, "Generate", "pcg.generate", generate)) ClearPreview();
    ImGui::EndDisabled();
    DrawPcgDebugControls(context);
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
    if (const auto* draft = m_previewData.Find("draft"); draft && draft->AsBool())
        ImGui::TextWrapped("Draft preview. Save the profile or apply the volume settings before generating.");
    if (!m_previewStatus.empty()) ImGui::TextWrapped("%s", m_previewStatus.c_str());
    else if (m_livePreview && m_previewPending && m_volume[0]) ImGui::TextDisabled("Updating preview after input settles...");
    auto* selected = context.GetSelectedGO();
    ImGui::BeginDisabled(!selected);
    OpArgs detach; if (selected) detach.Set("node", selected->instanceId);
    if (ActionButton(context, "Detach selected generated object", "pcg.detach", detach)) ClearPreview();
    ImGui::EndDisabled();
}

void WorldAuthoringPanel::DrawPcgDebugControls(EditorContext& context)
{
    if (!ImGui::TreeNode("Scene visualization")) return;
    ImGui::Checkbox("PCG bounds and exclusions", &m_showPcgBounds);
    ImGui::Checkbox("All biome volumes", &m_showBiomeBounds);
    ImGui::Checkbox("Accepted sites", &m_showAccepted);
    ImGui::Checkbox("Rejected sites", &m_showRejected);
    OpArgs inspect; inspect.Set("volume", std::string(m_volume));
    ImGui::BeginDisabled(!m_volume[0]);
    ActionButton(context, "Refresh volume bounds", "pcg.volume.inspect", inspect, &m_volumeBounds);
    ImGui::EndDisabled();
    const auto filter = [](const char* label, const OpData* counts, std::string& selected) {
        if (!counts || !counts->IsObject() || !ImGui::BeginCombo(label, selected.empty() ? "All" : selected.c_str())) return;
        if (ImGui::Selectable("All", selected.empty())) selected.clear();
        for (const auto& [name, count] : counts->AsObject()) {
            ImGui::PushID(name.c_str());
            if (ImGui::Selectable(name.c_str(), selected == name)) selected = name;
            ImGui::SameLine(); ImGui::TextDisabled("%lld", static_cast<long long>(count.AsInt64()));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    };
    filter("Rule", m_previewData.Find("ruleCounts"), m_debugRule);
    if (m_showRejected) filter("Rejection reason", m_previewData.Find("rejected"), m_debugReason);
    if (const auto* counts = m_previewData.Find("ruleCounts"); counts && ImGui::TreeNode("Accepted by rule")) {
        DrawScalarSummary(*counts); ImGui::TreePop();
    }
    ImGui::TextWrapped("Circles show placements. Crosses show rejected ground sites. Hover for details.");
    ImGui::TreePop();
}

bool WorldAuthoringPanel::ReadPcgProfileDraft(const OpData& data)
{
    scene::WorldPcgProfile profile;
    std::string diagnostic;
    if (!scene::DeserializeWorldPcgProfile(String(data, "text"), profile, diagnostic)) {
        m_error = std::move(diagnostic);
        return false;
    }
    m_profileDraft = std::move(profile);
    m_editProfileText = false;
    m_selectedRule = 0;
    ClearPreview();
    return true;
}

bool WorldAuthoringPanel::DrawPcgProfileRules(EditorContext& context, scene::WorldPcgProfile& profile)
{
    bool changed = false;
    if (ImGui::TreeNode("Profile settings")) {
        changed |= EditProfileLabel("Profile name", profile.name);
        changed |= EditProfileLabel("Profile key", profile.key);
        changed |= ImGui::InputScalar("Seed", ImGuiDataType_U64, &profile.seed);
        float minimum[]{profile.bounds.min.x, profile.bounds.min.z};
        float maximum[]{profile.bounds.max.x, profile.bounds.max.z};
        if (ImGui::InputFloat2("Minimum XZ (m)", minimum)) {
            profile.bounds.min.x = minimum[0]; profile.bounds.min.z = minimum[1]; changed = true;
        }
        if (ImGui::InputFloat2("Maximum XZ (m)", maximum)) {
            profile.bounds.max.x = maximum[0]; profile.bounds.max.z = maximum[1]; changed = true;
        }
        changed |= ImGui::InputScalar("Placement cap per 128 m cell", ImGuiDataType_U32, &profile.maxPlacementsPerCell);
        changed |= ImGui::InputScalar("Total instance limit", ImGuiDataType_U32, &profile.maxInstances);
        changed |= ImGui::InputScalar("Candidate limit", ImGuiDataType_U64, &profile.maxCandidates);
        changed |= widgets::AssetPathField("Path material", profile.pathMaterial, ".mat", context.projectRoot, PROFILE_PICKER_HOST);
        ImGui::TreePop();
    }

    ImGui::BeginDisabled(profile.rules.size() >= MAX_PROFILE_RULES);
    if (ImGui::Button("Add rule")) {
        scene::WorldPcgRule rule;
        rule.name = "New rule";
        AppendProfileRule(profile, std::move(rule));
        m_selectedRule = profile.rules.size() - 1;
        changed = true;
    }
    if (!profile.rules.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Duplicate rule")) {
            auto copy = profile.rules[m_selectedRule];
            copy.name = "Copy of " + (copy.name.empty() ? copy.key : copy.name);
            if (copy.name.size() > 256) copy.name = "Copied rule";
            AppendProfileRule(profile, std::move(copy));
            m_selectedRule = profile.rules.size() - 1;
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (profile.rules.empty()) return changed;

    ImGui::SameLine();
    if (ImGui::Button("Remove rule")) {
        profile.rules.erase(profile.rules.begin() + static_cast<std::ptrdiff_t>(m_selectedRule));
        m_selectedRule = 0;
        return true;
    }
    const auto labelFor = [](const auto& rule) -> const char* {
        return rule.name.empty() ? rule.key.c_str() : rule.name.c_str();
    };
    if (ImGui::BeginCombo("Rule", labelFor(profile.rules[m_selectedRule]))) {
        for (std::size_t index = 0; index < profile.rules.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(labelFor(profile.rules[index]), index == m_selectedRule)) m_selectedRule = index;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    auto& rule = profile.rules[m_selectedRule];
    ImGui::PushID(rule.key.c_str());
    changed |= EditProfileLabel("Rule name", rule.name);
    ImGui::TextDisabled("Rule key  %s", rule.key.c_str());
    changed |= DrawRulePresentation(context, rule);
    changed |= ImGui::InputFloat("Candidate spacing (m)", &rule.spacing);
    changed |= ImGui::SliderFloat("Density", &rule.density, 0.0f, 1.0f);
    changed |= ImGui::InputFloat("Minimum separation (m)", &rule.minSpacing);
    changed |= ImGui::InputFloat("Manual object clearance (m)", &rule.protectedDistance);
    if (ImGui::TreeNode("Size and orientation")) {
        changed |= EditProfileRange("Scale", rule.minScale, rule.maxScale);
        changed |= ImGui::InputFloat("Size bias", &rule.scaleBias);
        changed |= EditProfileRange("Yaw (deg)", rule.minYaw, rule.maxYaw);
        changed |= ImGui::Checkbox("Align to terrain", &rule.alignToSurface);
        changed |= ImGui::Checkbox("Align downhill", &rule.alignDownslope);
        if (rule.alignDownslope) changed |= ImGui::InputFloat("Downhill jitter (deg)", &rule.downslopeJitter);
        changed |= ImGui::InputFloat("Embed depth (m)", &rule.embedDepth);
        changed |= ImGui::InputFloat("Embed variation (m)", &rule.embedJitter);
        changed |= ImGui::Checkbox("Include in distant HLOD", &rule.includeHlod);
        ImGui::TreePop();
    }
    changed |= DrawRuleConditions(context, rule, profile);
    ImGui::PopID();
    return changed;
}

void WorldAuthoringPanel::DrawPcgProfile(EditorContext& context)
{
    if (!ImGui::TreeNode("Project placement profiles")) return;
    if (widgets::AssetPathField("Profile file", m_profileFile, ".pcg", context.projectRoot, PROFILE_PICKER_HOST) && m_profileDirty)
        ClearPreview();
    ImGui::BeginDisabled(m_profileDirty);
    if (ImGui::Button("New profile")) {
        m_profileDraft.emplace();
        m_profileDraft->name = "Custom placement";
        scene::WorldPcgRule rule;
        rule.name = "New rule";
        AppendProfileRule(*m_profileDraft, std::move(rule));
        m_selectedRule = 0;
        m_editProfileText = false;
        m_profileDirty = true;
        m_profileLoadedPath.clear(); m_profileExpected.clear(); m_error.clear();
        if (m_profileFile.empty()) m_profileFile = "Assets/World/Custom.pcg";
        ClearPreview();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_profileFile.empty());
    OpArgs inspect; inspect.Set("path", m_profileFile);
    OpData loaded;
    if (ActionButton(context, "Load profile", "pcg.profile.inspect", inspect, &loaded) && ReadPcgProfileDraft(loaded)) {
        m_profileLoadedPath = m_profileFile;
        m_profileExpected = String(loaded, "fingerprint");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (m_profileDirty) {
        ImGui::SameLine();
        if (ImGui::Button("Discard draft")) {
            m_profileDraft.reset(); m_editProfileText = false; m_profileText[0] = '\0';
            m_profileDirty = false;
            m_profileLoadedPath.clear(); m_profileExpected.clear(); m_error.clear();
            ClearPreview();
        }
    }

    if (ImGui::TreeNode("Rock nature preset")) {
        widgets::AssetPathField("Rock", m_rockSource, ".terrainpaint", context.projectRoot, PROFILE_PICKER_HOST);
        widgets::AssetPathField("Pebbles", m_pebbleSource, ".terrainpaint", context.projectRoot, PROFILE_PICKER_HOST);
        OpArgs rockArgs; rockArgs.Set("preset", std::string("rocks"));
        rockArgs.Set("rockSource", m_rockSource); rockArgs.Set("pebbleSource", m_pebbleSource);
        ImGui::BeginDisabled(m_profileDirty || m_rockSource.empty() || m_pebbleSource.empty());
        OpData draft;
        if (ActionButton(context, "Draft rock nature profile", "pcg.profile.inspect", rockArgs, &draft) && ReadPcgProfileDraft(draft)) {
            if (m_profileFile.empty()) m_profileFile = "Assets/World/RockNature.pcg";
            m_profileDirty = true;
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    if (!m_editProfileText && ImGui::SmallButton("Edit TOML")) {
        std::string text;
        if (!m_profileDraft || scene::SerializeWorldPcgProfile(*m_profileDraft, text, m_error)) {
            if (text.size() < sizeof(m_profileText)) {
                CopyText(m_profileText, text);
                m_profileDraft.reset(); m_editProfileText = true; m_error.clear();
            } else m_error = "The profile exceeds the TOML editor's text limit.";
        }
    }
    if (m_editProfileText) {
        if (ImGui::InputTextMultiline("##profileText", m_profileText, sizeof(m_profileText), ImVec2(-1, 180))) {
            m_profileDirty = true; ClearPreview();
        }
        if (ImGui::SmallButton("Edit rules")) {
            scene::WorldPcgProfile profile;
            if (scene::DeserializeWorldPcgProfile(m_profileText, profile, m_error)) {
                m_profileDraft = std::move(profile); m_editProfileText = false; m_selectedRule = 0;
            }
        }
    } else if (m_profileDraft && DrawPcgProfileRules(context, *m_profileDraft)) {
        m_profileDirty = true;
        ClearPreview();
    }

    ImGui::BeginDisabled(m_profileFile.empty() || (!m_profileDraft && (!m_editProfileText || !m_profileText[0])));
    if (ImGui::Button("Save profile")) {
        std::string text;
        if (!m_profileDraft || scene::SerializeWorldPcgProfile(*m_profileDraft, text, m_error)) {
            if (m_editProfileText) text = m_profileText;
            OpArgs save;
            save.Set("path", m_profileFile); save.Set("text", std::move(text));
            save.Set("expected", m_profileFile == m_profileLoadedPath ? m_profileExpected : std::string{});
            OpData saved;
            if (Run(context, "pcg.profile.save", save, &saved)) {
                m_profileLoadedPath = m_profileFile;
                m_profileExpected = String(saved, "fingerprint");
                m_profileDirty = false;
                ClearPreview();
                m_volumeBounds = {};
            }
        }
    }
    ImGui::EndDisabled();
    if (m_profileDirty) { ImGui::SameLine(); ImGui::TextDisabled("Unsaved changes"); }
    if (!m_profileLoadedPath.empty() && m_profileFile == m_profileLoadedPath && ImGui::Button("Use saved profile for volume")) {
        m_profile = m_profileLoadedPath;
        m_volumeDraft = true;
        ClearPreview();
        ImGui::SetNextItemOpen(true);
    }
    ImGui::TreePop();
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
    widgets::DrawAssetPickerModal(context.resources, context.imguiRenderer, PROFILE_PICKER_HOST);
    DrawStreaming(context);
    ImGui::Separator();
    if (!m_details.empty() && ImGui::TreeNode("Last operation details")) {
        ImGui::BeginChild("World operation result", ImVec2(0, 160), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(m_details.c_str()); ImGui::EndChild(); ImGui::TreePop();
    }
}

} /// @note namespace fbzz::editor
