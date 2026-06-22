// FBZZ Engine
// ProjectSettingsPanel.cpp | fbzz::editor
// Project settings editor UI
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <vector>

namespace fbzz::editor {

namespace {

constexpr float SIDEBAR_WIDTH = 180.0f;

bool SectionButton(const char* label, ProjectSettingsPanel::Section value, ProjectSettingsPanel::Section& current)
{
    const bool selected = current == value;
    if (ImGui::Selectable(label, selected)) {
        current = value;
        return true;
    }
    return false;
}


} // namespace

void ProjectSettingsPanel::OnRenderContent(EditorContext& ctx)
{
    struct UndoTracker {
        ImGuiID activeId = 0;
        ProjectSettings before;
        bool active = false;
        bool changed = false;
    };
    static UndoTracker undo;
    if (!ctx.undoStack || !ctx.undoStack->IsRecordingEnabled()) {
        undo.active = false;
        undo.changed = false;
        DrawSidebar();
        ImGui::SameLine();
        ImGui::BeginChild("##ProjectSettingsContent", { 0.0f, 0.0f }, false);
        DrawSection(ctx);
        ImGui::EndChild();
        return;
    }
    const ProjectSettings beforeDraw = ctx.projectSettings;
    const ImGuiID activeBefore = ImGui::GetActiveID();
    const std::uint64_t editGenerationBefore = m_editGeneration;

    DrawSidebar();
    ImGui::SameLine();

    ImGui::BeginChild("##ProjectSettingsContent", { 0.0f, 0.0f }, false);
    DrawSection(ctx);
    ImGui::EndChild();

    const ImGuiID activeAfter = ImGui::GetActiveID();
    const bool editedThisFrame = GImGui && GImGui->ActiveIdHasBeenEditedThisFrame;
    const bool structuralEdit = m_editGeneration != editGenerationBefore;
    auto pushCommand = [&](const ProjectSettings& before, const ProjectSettings& after) {
        if (!ctx.undoStack) return;
        EditorContext* context = &ctx;
        auto apply = [context](const ProjectSettings& value) {
            context->projectSettings = value;
            Time::targetFps = value.app.targetFps;
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Edit Project Settings",
            [apply, after]() { apply(after); },
            [apply, before]() { apply(before); }));
    };

    if (structuralEdit) {
        pushCommand(beforeDraw, ctx.projectSettings);
        undo.active = false;
        undo.changed = false;
    } else if (!undo.active && activeAfter != 0 && activeAfter != activeBefore) {
        undo.activeId = activeAfter;
        undo.before = beforeDraw;
        undo.active = true;
        undo.changed = editedThisFrame;
    } else if (undo.active && activeAfter == undo.activeId) {
        undo.changed |= editedThisFrame;
    } else if (undo.active && activeAfter != undo.activeId) {
        if (undo.changed) pushCommand(undo.before, ctx.projectSettings);
        undo.active = false;
        undo.changed = false;
    } else if (!undo.active && editedThisFrame && activeAfter == 0) {
        pushCommand(beforeDraw, ctx.projectSettings);
    }
}

void ProjectSettingsPanel::DrawSidebar()
{
    ImGui::BeginChild("##ProjectSettingsSidebar", { SIDEBAR_WIDTH, 0.0f }, true);
    SectionButton("Application", Section::Application, m_currentSection);
    SectionButton("Import",      Section::Import,      m_currentSection);
    SectionButton("Render",      Section::Render,      m_currentSection);
    SectionButton("Post Process",Section::PostProcess, m_currentSection);
    SectionButton("Physics",     Section::Physics,     m_currentSection);
    SectionButton("Audio",       Section::Audio,       m_currentSection);
    SectionButton("Screen",      Section::Screen,      m_currentSection);
    SectionButton("Tags",        Section::Tags,        m_currentSection);
    SectionButton("Layers",      Section::Layers,      m_currentSection);
    ImGui::EndChild();
}

void ProjectSettingsPanel::DrawSection(EditorContext& ctx)
{
    auto& settings = ctx.projectSettings;
    switch (m_currentSection) {
    case Section::Application: DrawApplication(settings); break;
    case Section::Import:      DrawImport(ctx); break;
    case Section::Render:      DrawRender(settings.render); break;
    case Section::PostProcess: DrawPostProcess(settings.render); break;
    case Section::Physics:     DrawPhysics(settings); break;
    case Section::Audio:       DrawAudio(settings); break;
    case Section::Screen:      DrawScreen(settings); break;
    case Section::Tags:        DrawTags(settings); break;
    case Section::Layers:      DrawLayers(settings); break;
    }
}

namespace {

// プリセットディレクトリのパス
std::string ImportPresetsDir(const EditorContext& ctx)
{
    const std::string root = ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
    return util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(root) / ".import_presets");
}

struct PresetEntry {
    std::string      name;
    std::string      path;
    FbxImportOptions options;
};

std::vector<PresetEntry> LoadImportPresets(const std::string& presetsDir)
{
    std::vector<PresetEntry> result;
    namespace fs = std::filesystem;
    const fs::path dir = util::FileSystem::PathFromUtf8(presetsDir);
    if (!util::FileSystem::Exists(dir)) return result;
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = util::StringUtils::ToLower(
                util::FileSystem::PathToUtf8(entry.path().extension()));
            if (ext != ".toml") continue;
            std::string text;
            if (!util::FileSystem::ReadText(util::FileSystem::PathToUtf8(entry.path()), text))
                continue;
            std::istringstream ss(text);
            const auto parsed = toml::parse(ss);
            if (!parsed) continue;
            PresetEntry p;
            p.name = entry.path().stem().string();
            p.path = util::FileSystem::PathToUtf8(entry.path());
            const auto& tbl = parsed.table();
            if (auto v = tbl["options"]["normal_map_convention"].value<int64_t>())
                p.options.normalMapConvention = static_cast<NormalMapConvention>(*v);
            if (auto v = tbl["options"]["generate_tex_descriptors"].value<bool>())
                p.options.generateTexDescriptors = *v;
            if (auto v = tbl["options"]["default_compression"].value<int64_t>())
                p.options.defaultCompression = static_cast<asset::TextureCompression>(*v);
            result.push_back(std::move(p));
        }
    } catch (...) {}
    std::sort(result.begin(), result.end(),
        [](const PresetEntry& a, const PresetEntry& b) { return a.name < b.name; });
    return result;
}

} // namespace

void ProjectSettingsPanel::DrawImport(EditorContext& ctx)
{
    ImGui::TextUnformatted("Import");
    ImGui::Separator();

    // ── Default FBX Settings ──────────────────────────────────────────────────
    ImGui::TextDisabled("Default settings applied to new FBX imports.");
    ImGui::Spacing();

    auto& opt = ctx.defaultImportOptions;

    if (ImGui::CollapsingHeader("FBX Defaults", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        {
            static constexpr const char* kConvNames[] = { "DirectX (keep G)", "OpenGL (flip G)" };
            int convIdx = static_cast<int>(opt.normalMapConvention);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Normal Map Convention", &convIdx, kConvNames, 2))
                opt.normalMapConvention = static_cast<NormalMapConvention>(convIdx);
        }
        ImGui::Checkbox("Auto-generate .tex descriptors", &opt.generateTexDescriptors);
        {
            static constexpr const char* kCompNames[] = {
                "Auto", "BC1", "BC3", "BC4", "BC5", "BC6H", "BC7", "None"
            };
            int compIdx = static_cast<int>(opt.defaultCompression);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Default Compression", &compIdx, kCompNames, 8))
                opt.defaultCompression = static_cast<asset::TextureCompression>(compIdx);
        }
        ImGui::Unindent();
    }

    ImGui::Spacing();

    // ── Import Presets ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Import Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        const std::string presetsDir = ImportPresetsDir(ctx);
        static std::vector<PresetEntry> s_presets;
        static bool s_presetsLoaded = false;
        static int  s_deleteIdx     = -1;

        if (!s_presetsLoaded) {
            s_presets       = LoadImportPresets(presetsDir);
            s_presetsLoaded = true;
        }

        if (ImGui::SmallButton("Refresh Presets"))
            s_presetsLoaded = false;

        ImGui::Spacing();

        if (s_presets.empty()) {
            ImGui::TextDisabled("No presets found in Assets/.import_presets/");
            ImGui::TextDisabled("Create presets via the Import Settings dialog\n(right-click FBX \xe2\x86\x92 Import with Settings...).");
        } else {
            if (ImGui::BeginTable("##presets_tbl", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Name",           ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Settings",       ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("",               ImGuiTableColumnFlags_WidthFixed,   130.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < static_cast<int>(s_presets.size()); ++i) {
                    const auto& p = s_presets[static_cast<size_t>(i)];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(p.name.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("Conv=%s  GenTex=%s",
                        p.options.normalMapConvention == NormalMapConvention::OpenGL ? "OpenGL" : "DirectX",
                        p.options.generateTexDescriptors ? "on" : "off");

                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushID(i);
                    if (ImGui::SmallButton("Use as Default")) {
                        opt = p.options;
                        ++m_editGeneration;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Copy this preset's values into Default FBX Settings above.");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) {
                        s_deleteIdx = i;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();

                if (s_deleteIdx >= 0) {
                    const std::string delPath =
                        s_presets[static_cast<size_t>(s_deleteIdx)].path;
                    try {
                        std::filesystem::remove(util::FileSystem::PathFromUtf8(delPath));
                        FBZZ_LOG_INFO("Deleted import preset: %s", delPath.c_str());
                    } catch (...) {
                        FBZZ_LOG_ERROR("Failed to delete preset: %s", delPath.c_str());
                    }
                    s_presets.erase(s_presets.begin() + s_deleteIdx);
                    s_deleteIdx = -1;
                }
            }
        }

        ImGui::Unindent();
    }

    ImGui::Spacing();

    // ── Exclude Patterns ──────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Exclude Patterns")) {
        ImGui::Indent();
        ImGui::TextDisabled("Files whose stem ends with these suffixes are skipped:");
        ImGui::Spacing();
        static constexpr const char* kExcluded[] = {
            "_backup", "_old", "_wip", "_ref", "_tmp", "_test", "_unused", "_bak"
        };
        for (const char* s : kExcluded)
            ImGui::BulletText("%s", s);
        ImGui::Spacing();
        ImGui::TextDisabled("(Built-in patterns. Not yet user-configurable.)");
        ImGui::Unindent();
    }
}

void ProjectSettingsPanel::DrawApplication(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Application");
    ImGui::Separator();

    if (ImGui::DragInt("Target FPS", &settings.app.targetFps, 1.0f, 0, 360))
        Time::targetFps = settings.app.targetFps;
    ImGui::SameLine();
    ImGui::TextDisabled("(0 = unlimited)");

    ImGui::Spacing();
    ImGui::SeparatorText("Scenes");
    char defaultScene[260];
    std::snprintf(defaultScene, sizeof(defaultScene), "%s", settings.game.project.defaultScene.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("Default Scene", defaultScene, sizeof(defaultScene)))
        settings.game.project.defaultScene = defaultScene;

    char startScene[260];
    std::snprintf(startScene, sizeof(startScene), "%s", settings.game.runtime.startScene.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("Start Scene", startScene, sizeof(startScene)))
        settings.game.runtime.startScene = startScene;
}

void ProjectSettingsPanel::DrawRender(renderer::RenderSettings& render)
{
    ImGui::TextUnformatted("Render");
    ImGui::Separator();

    const char* pipelineItems[] = { "Forward", "Deferred" };
    int pipelineIdx = static_cast<int>(render.pipeline);
    if (ImGui::Combo("Pipeline", &pipelineIdx, pipelineItems, 2))
        render.pipeline = static_cast<renderer::RenderingPipeline>(pipelineIdx);

    ImGui::Spacing();
    ImGui::Checkbox("Shadow", &render.shadowEnabled);
    if (render.shadowEnabled)
    {
        ImGui::Indent();
        static const uint32_t kResValues[] = { 512u, 1024u, 2048u, 4096u, 8192u };
        static const char*    kResLabels[] = { "512",  "1024",  "2048",  "4096",  "8192" };
        int resIdx = 3;
        for (int i = 0; i < 5; ++i)
            if (kResValues[i] == render.shadow.mapResolution) { resIdx = i; break; }
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("Resolution##shadow", &resIdx, kResLabels, 5))
            render.shadow.mapResolution = kResValues[resIdx];
        ImGui::SameLine();
        ImGui::TextDisabled("(Shadow Map)");

        static const char* kPcfLabels[] = { "0 – Hard", "1 – 3x3", "2 – 5x5", "3 – 7x7" };
        int pcfIdx = std::clamp(render.shadow.pcfRadius, 0, 3);
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("PCF Radius##shadow", &pcfIdx, kPcfLabels, 4))
            render.shadow.pcfRadius = pcfIdx;
        ImGui::SameLine();
        ImGui::TextDisabled("(Blur)");
        ImGui::Unindent();
    }
    ImGui::SameLine();
    {
        const char* kViewModeLabels[] = { "Lit", "Unlit", "Wireframe Lit", "Wireframe Unlit" };
        int idx = static_cast<int>(render.viewMode);
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::Combo("View Mode", &idx, kViewModeLabels, 4))
            render.viewMode = static_cast<renderer::ViewMode>(idx);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Colliders",    &render.showColliders);
    ImGui::SameLine();
    ImGui::Checkbox("Decal Bounds", &render.showDecalBounds);
    ImGui::Checkbox("Selection Outline", &render.showSelectionOutline);

    ImGui::Spacing();
    ImGui::SeparatorText("Debug");
    ImGui::Checkbox("Pass Viewer", &render.passViewerEnabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Shows RT thumbnails and CPU timings for each render pass\n"
            "in the ImGui window \"Render Debug\".");
    }

    ImGui::Spacing();
    ImGui::SliderFloat("Outline Width", &render.outlineWidth, 0.005f, 0.2f);
    ImGui::ColorEdit4("Outline Color", render.outlineColor);
}

void ProjectSettingsPanel::DrawPostProcess(renderer::RenderSettings& render)
{
    const PostProcessInspectorResult result = DrawPostProcessInspector(render.postProcess);
    if (result.structureChanged)
        ++m_editGeneration;
}

void ProjectSettingsPanel::DrawPhysics(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Physics");
    ImGui::Separator();

    ImGui::DragInt("Hz", &settings.physics.hz, 1.0f, 1, 1000);
    ImGui::DragInt("Substeps", &settings.physics.substeps, 1.0f, 1, 32);

    float gravity[3] = {
        settings.physics.gravity.x,
        settings.physics.gravity.y,
        settings.physics.gravity.z
    };
    if (ImGui::DragFloat3("Gravity", gravity, 0.05f, -1000.0f, 1000.0f))
        settings.physics.gravity = { gravity[0], gravity[1], gravity[2] };
}

void ProjectSettingsPanel::DrawAudio(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Audio");
    ImGui::Separator();

    ImGui::SliderFloat("BGM Volume", &settings.audio.bgmVolume, 0.0f, 1.0f);
    ImGui::SliderFloat("SE Volume", &settings.audio.seVolume, 0.0f, 1.0f);
}

void ProjectSettingsPanel::DrawScreen(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Screen");
    ImGui::Separator();

    ImGui::DragInt("Width", &settings.screen.width, 1.0f, 1, 7680);
    ImGui::DragInt("Height", &settings.screen.height, 1.0f, 1, 4320);
}

void ProjectSettingsPanel::DrawTags(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Tags");
    ImGui::Separator();

    if (ImGui::SmallButton("Reset Unity Preset")) {
        settings.game.tags = { "Untagged", "Respawn", "Finish", "EditorOnly",
                               "MainCamera", "Player", "GameController" };
        ++m_editGeneration;
    }
    ImGui::Spacing();

    int removeIdx = -1;
    for (int i = 0; i < static_cast<int>(settings.game.tags.size()); ++i) {
        ImGui::PushID(i);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", settings.game.tags[i].c_str());
        ImGui::SetNextItemWidth(-80.0f);
        if (ImGui::InputText("##tag", buf, sizeof(buf)))
            settings.game.tags[i] = buf;
        ImGui::SameLine();
        if (settings.game.tags[i] != "Untagged" && ImGui::SmallButton("Remove"))
            removeIdx = i;
        ImGui::PopID();
    }

    if (removeIdx >= 0) {
        settings.game.tags.erase(settings.game.tags.begin() + removeIdx);
        ++m_editGeneration;
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-80.0f);
    ImGui::InputText("##newtag", m_newTag, sizeof(m_newTag));
    ImGui::SameLine();
    if (ImGui::SmallButton("Add") && m_newTag[0] != '\0') {
        settings.game.tags.push_back(m_newTag);
        m_newTag[0] = '\0';
        ++m_editGeneration;
    }
}

void ProjectSettingsPanel::DrawLayers(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Layers");
    ImGui::Separator();

    if (ImGui::SmallButton("Reset Unity Preset")) {
        settings.game.layerNames = {
            "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
            "", "", "", "", "", "", "", "", "", "",
            "", "", "", "", "", "", "", "", "", "",
            "", "", "", "", "", ""
        };
        ++m_editGeneration;
    }
    ImGui::Spacing();

    for (int i = 0; i < 32; ++i) {
        ImGui::PushID(i);
        ImGui::Text("%2d", i);
        ImGui::SameLine();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", settings.game.layerNames[i].c_str());
        if (settings.game.layerNames[i].empty()) {
            ImGui::TextDisabled("User Layer %d", i);
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##layer", buf, sizeof(buf)))
            settings.game.layerNames[i] = buf;
        ImGui::PopID();
    }
}

} // namespace fbzz::editor
