// FBZZ Engine
// ProjectSettingsPanel.cpp | fbzz::editor
// Project settings editor UI
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Gamepad.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
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

    // 検索バー — 入力文字列に部分一致するセクションのみ表示する。
    static char s_filter[64] = {};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search...", s_filter, sizeof(s_filter));
    ImGui::Separator();

    // 空フィルター時は全表示、入力時は大文字小文字無視の部分一致フィルタリング。
    // keywords は「そのセクションに含まれるが名前には出てこない語」。
    // WHY 必要か: 項目を統合したことで "Screen" や "Bloom" のような
    //      旧項目名・内容語で引いたときに何も出なくなる。検索の当たりを維持する。
    auto btn = [&](const char* name, Section sec, const char* keywords = "") {
        const bool match = s_filter[0] == '\0'
            || util::StringUtils::ContainsCI(name, s_filter)
            || (keywords[0] != '\0' && util::StringUtils::ContainsCI(keywords, s_filter));
        if (match) SectionButton(name, sec, m_currentSection);
    };

    btn("Application",    Section::Application,
        "screen resolution window fps scene startup 解像度 画面");
    btn("Graphics",       Section::Graphics,
        "render post process shadow bloom fog debug outline navmesh 描画 影");
    btn("Physics",        Section::Physics, "gravity substep 重力");
    btn("Input",          Section::Input,   "gamepad key binding pad キー 入力");
    btn("Audio",          Section::Audio,   "volume bgm se 音量");
    btn("Tags & Layers",  Section::TagsAndLayers, "tag layer タグ レイヤー");
    btn("Import",         Section::Import,  "fbx preset model texture インポート");

    ImGui::EndChild();
}

void ProjectSettingsPanel::DrawSection(EditorContext& ctx)
{
    auto& settings = ctx.projectSettings;
    switch (m_currentSection) {
    case Section::Application:   DrawApplication(settings); break;
    case Section::Graphics:      DrawGraphics(ctx, settings.render); break;
    case Section::Physics:       DrawPhysics(settings); break;
    case Section::Input:         DrawInput(ctx); break;
    case Section::Audio:         DrawAudio(settings); break;
    case Section::TagsAndLayers: DrawTagsAndLayers(settings); break;
    case Section::Import:        DrawImport(ctx); break;
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
            if (auto v = tbl["options"]["source_dcc"].value<int64_t>())
                p.options.sourceDcc = static_cast<FbxSourceDcc>(*v);
            if (auto v = tbl["options"]["up_axis"].value<int64_t>())
                p.options.upAxis = static_cast<FbxUpAxis>(*v);
            if (auto v = tbl["options"]["normal_map_convention"].value<int64_t>())
                p.options.normalMapConvention = static_cast<NormalMapConvention>(*v);
            if (auto v = tbl["options"]["unit_scale_multiplier"].value<float>())
                p.options.unitScaleMultiplier = *v;
            if (auto v = tbl["options"]["generate_normals"].value<bool>())
                p.options.generateNormals = *v;
            if (auto v = tbl["options"]["generate_tangents"].value<bool>())
                p.options.generateTangents = *v;
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
            static constexpr const char* kSourceDccNames[] = { "Auto Detect", "Maya / FBX SDK", "Blender" };
            int sourceDccIdx = static_cast<int>(opt.sourceDcc);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Source DCC", &sourceDccIdx, kSourceDccNames, 3))
                opt.sourceDcc = static_cast<FbxSourceDcc>(sourceDccIdx);
        }
        {
            static constexpr const char* kAxisNames[] = { "Auto", "Y Up", "Z Up" };
            int axisIdx = static_cast<int>(opt.upAxis);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Source Up Axis", &axisIdx, kAxisNames, 3))
                opt.upAxis = static_cast<FbxUpAxis>(axisIdx);
        }
        ImGui::DragFloat("Unit Scale", &opt.unitScaleMultiplier, 0.01f, 0.001f, 100.0f, "%.3fx");
        ImGui::Checkbox("Generate Normals", &opt.generateNormals);
        ImGui::SameLine();
        ImGui::Checkbox("Generate Tangents", &opt.generateTangents);
        {
            static constexpr const char* kConvNames[] = { "DirectX (keep G)", "OpenGL (flip G)" };
            int convIdx = static_cast<int>(opt.normalMapConvention);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Normal Map Convention", &convIdx, kConvNames, 2))
                opt.normalMapConvention = static_cast<NormalMapConvention>(convIdx);
        }
        ImGui::Checkbox("Auto-generate texture .meta sidecars", &opt.generateTexDescriptors);
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
                    ImGui::TextDisabled("DCC=%s  Axis=%s  Scale=%.2f  Conv=%s  GenTex=%s",
                        p.options.sourceDcc == FbxSourceDcc::Blender ? "Blender" :
                        p.options.sourceDcc == FbxSourceDcc::Maya ? "Maya" : "Auto",
                        p.options.upAxis == FbxUpAxis::ZUp ? "Z" :
                        p.options.upAxis == FbxUpAxis::YUp ? "Y" : "Auto",
                        p.options.unitScaleMultiplier,
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

    // 旧 Screen セクション。解像度はウィンドウ設定であり、
    // Target FPS と並べた方が「起動時の画面まわり」として一望できる。
    ImGui::Spacing();
    ImGui::SeparatorText("Screen");
    ImGui::DragInt("Width",  &settings.screen.width,  1.0f, 1, 7680);
    ImGui::DragInt("Height", &settings.screen.height, 1.0f, 1, 4320);
}

void ProjectSettingsPanel::DrawGraphics(EditorContext& ctx, renderer::RenderSettings& render)
{
    ImGui::TextUnformatted("Graphics");
    ImGui::Separator();

    // WHY Post Process セクションが無いか:
    //     Bloom / SSR / TAA といった「ルック」はシーン内の場所ごとに変わるもので、
    //     プロジェクト全体の設定として持つと屋外と洞窟を切り替えられない。
    //     所有者を Post Process Volume + Post Process Profile (.fzdata) へ一本化した。
    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
        DrawRenderCore(render);

    if (ImGui::CollapsingHeader("User Settings (preview)"))
        DrawRenderUserSettings(ctx, render);

    if (ImGui::CollapsingHeader("Debug"))
        DrawRenderDebug(render);

    ImGui::Spacing();
    ImGui::TextDisabled(
        "ポストプロセス / 高度グラフィクスは Post Process Volume で設定します。\n"
        "Hierarchy に Post Process Volume を追加し、Post Process Profile (.fzdata) を割り当ててください。");
}

// WHY 保存されない項目をここへ出すか:
//     明るさと描画スケールは Option 画面がプレイヤー設定として持つ値で、
//     プロジェクト設定には保存しない。ただし「効きを目で確かめる」手段が
//     Play + スクリプトしか無いと、調整のたびに再生し直すことになる。
//     保存されないことを明記したうえで、確認用のつまみだけ置く。
void ProjectSettingsPanel::DrawRenderUserSettings(EditorContext& ctx,
                                                  renderer::RenderSettings& render)
{
    ImGui::Indent();
    ImGui::TextDisabled("Option 画面がプレイヤー設定として持つ値。ここでの変更は保存されません。");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Brightness", &render.userBrightness, 0.1f, 4.0f, "%.2f");

    // WHY 描画スケールだけ直接書かないか: 値が変わるたびに中間 RT を作り直すため、
    //     ドラッグ中ずっと 16 枚の再確保が走る。手を離した時点で 1 度だけ反映する。
    if (!m_renderScaleDragging) m_renderScaleDraft = render.renderScale;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Render Scale", &m_renderScaleDraft,
                       renderer::kMinRenderScale, renderer::kMaxRenderScale, "%.2f");
    m_renderScaleDragging = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) render.renderScale = m_renderScaleDraft;

    // ドラッグ中は反映前の draft で予告する (何ピクセルになるかを見ながら決められる)。
    uint32_t internalWidth = 0, internalHeight = 0;
    renderer::ResolveRenderResolution(
        static_cast<uint32_t>(std::max(ctx.viewportWidth, 1.0f)),
        static_cast<uint32_t>(std::max(ctx.viewportHeight, 1.0f)),
        m_renderScaleDraft, internalWidth, internalHeight);
    ImGui::TextDisabled("Scene View: %.0fx%.0f -> %ux%u",
                        ctx.viewportWidth, ctx.viewportHeight,
                        internalWidth, internalHeight);

    ImGui::Unindent();
}

void ProjectSettingsPanel::DrawRenderCore(renderer::RenderSettings& render)
{
    ImGui::Indent();

    // WHAT: Combo のポップアップへ候補を個別に描画し、Forward+ / Deferred+ も常に表示する。
    // WHY: ImGui::Combo の配列オーバーロードは表示領域を呼び出し側へ委ねるため、
    //      狭い Project Settings パネルでは Plus 系の候補が確認しづらい。
    const char* pipelineItems[] = { "Forward", "Deferred", "Forward+", "Deferred+" };
    int pipelineIdx = std::clamp(static_cast<int>(render.pipeline), 0, 3);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Pipeline", pipelineItems[pipelineIdx]))
    {
        for (int index = 0; index < 4; ++index)
        {
            const bool selected = pipelineIdx == index;
            if (ImGui::Selectable(pipelineItems[index], selected))
                pipelineIdx = index;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    render.pipeline = static_cast<renderer::RenderingPipeline>(pipelineIdx);

    const bool clusteredPipeline = render.pipeline == renderer::RenderingPipeline::ForwardPlus
                                || render.pipeline == renderer::RenderingPipeline::DeferredPlus;
    if (clusteredPipeline)
    {
        ImGui::Indent();
        ImGui::Checkbox("Enable clustered lights", &render.clustered.enabled);
        if (render.clustered.enabled)
        {
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat("Max distance##clustered", &render.clustered.maxDistance,
                             1.0f, 1.0f, 10000.0f, "%.0f m");
            ImGui::Checkbox("Debug heatmap##clustered", &render.clustered.debugHeatmap);
            ImGui::Checkbox("Force all lights##clustered", &render.clustered.forceAllLights);
            ImGui::SameLine();
            ImGui::TextDisabled("(Linear validation mode)");
        }
        ImGui::Unindent();
    }

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
        ImGui::TextDisabled("(Atlas)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "全カスケードが共有するアトラス 1 枚ぶんの解像度。\n"
                "2 分割以上では 2x2 タイルへ分けるので、1 カスケードは この値 / 2 になる。\n"
                "分割数を増やしてもメモリと塗り量は変わらない (面積の配分が変わるだけ)。");

        // 影の到達距離。カスケード分割の全体レンジでもある。
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragFloat("Shadow Distance##shadow", &render.shadow.autoFitDistance,
                         1.0f, 5.0f, 2000.0f, "%.0f m");
        ImGui::SameLine();
        ImGui::TextDisabled("(影が届く最大距離)");

        // ── カスケード ────────────────────────────────────────────────────────
        ImGui::Spacing();
        static const char* kCascadeLabels[] = { "1 – 単一", "2", "3", "4" };
        int cascadeIdx = std::clamp(render.shadow.cascadeCount, 1, renderer::kMaxShadowCascades) - 1;
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("Cascades##shadow", &cascadeIdx, kCascadeLabels, 4))
            render.shadow.cascadeCount = cascadeIdx + 1;
        ImGui::SameLine();
        ImGui::TextDisabled("(Cascaded Shadow Maps)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "視錐台を距離で区切り、手前ほど狭い範囲へ 1 タイルを丸ごと割り当てる。\n"
                "同じ解像度・同じ塗り量のまま、近距離のテクセル密度だけを上げられる。\n"
                "1 を選ぶと従来の単一シャドウマップに戻る。");

        if (render.shadow.cascadeCount > 1)
        {
            ImGui::Indent();
            ImGui::SetNextItemWidth(100.0f);
            ImGui::SliderFloat("Split Lambda##shadow", &render.shadow.cascadeSplitLambda,
                               0.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            ImGui::TextDisabled("(0=等分割 / 1=対数分割)");

            ImGui::SetNextItemWidth(100.0f);
            ImGui::SliderFloat("Blend##shadow", &render.shadow.cascadeBlend, 0.0f, 0.5f, "%.2f");
            ImGui::SameLine();
            ImGui::TextDisabled("(境界のクロスフェード幅)");

            ImGui::Checkbox("Visualize Cascades##shadow", &render.shadow.debugVisualizeCascades);
            ImGui::SameLine();
            ImGui::TextDisabled("(緑=近 → 赤=遠)");
            ImGui::Unindent();
        }

        // 各カスケードの実テクセル密度。分割と解像度の効き方を数値で見せる。
        // WHY: 「解像度を上げる」より「分割数を増やす / 到達距離を縮める」ほうが
        //      効くことが多く、それはこの表を見ないと判断できない。
        {
            const int      count      = std::clamp(render.shadow.cascadeCount, 1, renderer::kMaxShadowCascades);
            const uint32_t tileSize   = std::max(render.shadow.mapResolution / (count > 1 ? 2u : 1u), 1u);
            const float    nearZ      = 0.1f;
            const float    lambda     = std::clamp(render.shadow.cascadeSplitLambda, 0.0f, 1.0f);
            const float    distance   = std::max(render.shadow.autoFitDistance, 1.0f);
            // ComputeFrustumSliceSphere と同じ k (fovY 60 / aspect 16:9 の代表値)。
            constexpr float kSphereFactor = 1.177f;

            ImGui::Spacing();
            ImGui::TextDisabled("Cascade texel density (tile %u px)", tileSize);
            float sliceNear = nearZ;
            for (int i = 0; i < count; ++i) {
                const float ratio    = static_cast<float>(i + 1) / static_cast<float>(count);
                const float logSplit = nearZ * std::pow(distance / nearZ, ratio);
                const float uniform  = nearZ + (distance - nearZ) * ratio;
                const float sliceFar = (i == count - 1)
                    ? distance : (lambda * logSplit + (1.0f - lambda) * uniform);
                const float radius   = sliceFar * kSphereFactor;
                const float texelCm  = (radius * 2.0f) / static_cast<float>(tileSize) * 100.0f;
                ImGui::BulletText("Cascade %d: %.1f–%.1f m   1 texel = %.1f cm",
                                  i, sliceNear, sliceFar, texelCm);
                sliceNear = sliceFar;
            }
        }

        static const char* kPcfLabels[] = { "0 – Hard", "1 – 3x3", "2 – 5x5", "3 – 7x7" };
        int pcfIdx = std::clamp(render.shadow.pcfRadius, 0, 3);
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("PCF Radius##shadow", &pcfIdx, kPcfLabels, 4))
            render.shadow.pcfRadius = pcfIdx;
        ImGui::SameLine();
        ImGui::TextDisabled("(Blur)");

        // PCSS — 距離に比例してペナンブラが広がる物理ベースのソフトシャドウ。
        ImGui::Spacing();
        ImGui::Checkbox("PCSS##shadow", &render.shadow.pcssEnabled);
        ImGui::SameLine();
        ImGui::TextDisabled("(Percentage Closer Soft Shadows)");
        if (render.shadow.pcssEnabled)
        {
            ImGui::Indent();
            ImGui::SetNextItemWidth(100.0f);
            ImGui::DragFloat("Light Radius##pcss", &render.shadow.pcssLightRadius,
                             0.1f, 0.0f, 50.0f, "%.2f");
            ImGui::SameLine();
            ImGui::TextDisabled("(大きいほどソフト)");
            ImGui::Unindent();
        }

        ImGui::Unindent();
    }
    ImGui::Spacing();
    {
        const char* kViewModeLabels[] = { "Lit", "Unlit", "Wireframe Lit", "Wireframe Unlit" };
        int idx = static_cast<int>(render.viewMode);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("View Mode", &idx, kViewModeLabels, 4))
            render.viewMode = static_cast<renderer::ViewMode>(idx);
    }

    ImGui::Unindent();
}

void ProjectSettingsPanel::DrawRenderDebug(renderer::RenderSettings& render)
{
    ImGui::Indent();

    ImGui::SeparatorText("Debug Visualization");
    ImGui::Checkbox("Colliders",         &render.showColliders);
    ImGui::SameLine();
    ImGui::Checkbox("Terrain Collision", &render.showTerrainCollision);
    ImGui::SameLine();
    ImGui::Checkbox("NavMesh",           &render.showNavMesh);
    ImGui::Checkbox("AI Sensors",        &render.showNavSensors);
    ImGui::SameLine();
    ImGui::Checkbox("UI Rects",          &render.showUIRects);
    ImGui::SameLine();
    ImGui::Checkbox("Decal Bounds",      &render.showDecalBounds);
    ImGui::SameLine();
    ImGui::Checkbox("Selection Outline", &render.showSelectionOutline);

    ImGui::Spacing();
    ImGui::SeparatorText("Debug");
    ImGui::Checkbox("Pass Viewer", &render.passViewerEnabled);
    ImGui::Checkbox("Particle Budget", &render.particleBudgetEnabled);
    if (render.particleBudgetEnabled)
        ImGui::DragInt("Particle Budget Count", &render.particleBudget, 100, 0, 1000000);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Shows RT thumbnails and CPU timings for each render pass\n"
            "in the ImGui window \"Render Debug\".");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Selection Outline");
    ImGui::SliderFloat("Outline Width", &render.outlineWidth, 0.005f, 0.2f);
    ImGui::ColorEdit4("Outline Color", render.outlineColor);

    ImGui::Unindent();
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

namespace {

// .inputactions の保存先。ProjectSettings.toml と同じディレクトリに置く。
// WHY: ランタイム側 (ProjectSettings::Load) が同じ規則で探すため、ここを唯一の定義とする。
std::string InputActionsPath(const EditorContext& ctx)
{
    const std::string root = ctx.projectRoot.empty() ? "." : ctx.projectRoot;
    return util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(root) / "ProjectSettings" / "Input.inputactions");
}

// バインド 1 件の行。削除が要求されたら true を返す。
bool DrawBindingRow(const input::InputBinding& binding, int index,
                    const char* rebindLabel, bool rebinding,
                    bool& outRebindRequested)
{
    ImGui::PushID(index);

    ImGui::AlignTextToFramePadding();
    ImGui::Bullet();
    ImGui::SameLine();

    // リバインド待機中のバインドは目立たせる。
    // WHY: 「押してください」の対象がどれか分からないと、別のバインドを潰す事故になる。
    if (rebinding) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.85f, 0.3f, 1.0f });
        ImGui::TextUnformatted("< 入力してください... (Esc で取消) >");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextUnformatted(input::InputActionMap::DescribeBinding(binding).c_str());
    }

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110.0f);
    if (ImGui::SmallButton(rebindLabel)) outRebindRequested = true;

    ImGui::SameLine();
    const bool removeRequested = ImGui::SmallButton("Remove");

    ImGui::PopID();
    return removeRequested;
}

// バインド配列 1 本ぶんの編集 UI。
void DrawBindingList(const char* label,
                     std::vector<input::InputBinding>& bindings,
                     const std::function<void(int bindingIndex)>& beginRebind,
                     const std::function<bool(int bindingIndex)>& isRebindTarget,
                     bool& outDirty)
{
    ImGui::TextDisabled("%s", label);
    ImGui::Indent();

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(bindings.size()); ++i) {
        bool rebindRequested = false;
        const bool removeRequested =
            DrawBindingRow(bindings[static_cast<size_t>(i)], i, "Rebind",
                           isRebindTarget(i), rebindRequested);
        if (rebindRequested) beginRebind(i);
        if (removeRequested) removeIndex = i;
    }

    if (removeIndex >= 0) {
        bindings.erase(bindings.begin() + removeIndex);
        outDirty = true;
    }

    if (ImGui::SmallButton("+ Add Binding")) {
        // 末尾に追加してすぐリバインド待機に入る。
        // WHY: 空のバインドを置いたまま放置されると「反応しないバインド」が残る。
        beginRebind(static_cast<int>(bindings.size()));
    }

    ImGui::Unindent();
}

} // namespace

void ProjectSettingsPanel::DrawInput(EditorContext& ctx)
{
    ImGui::TextUnformatted("Input");
    ImGui::Separator();

    ImGui::TextDisabled(
        "ゲーム入力のバインド定義。エディタ操作のショートカットは Hotkey Editor で編集します。");
    ImGui::Spacing();

    const std::string path = InputActionsPath(ctx);

    // ── ファイル操作 ─────────────────────────────────────────────────────────
    if (ImGui::Button("Save")) {
        if (input::InputActionMap::SaveToFile(path))
            FBZZ_LOG_INFO("Input: %s へ保存しました", path.c_str());
        else
            FBZZ_LOG_ERROR("Input: %s へ保存できませんでした", path.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        if (!input::InputActionMap::LoadFromFile(path))
            FBZZ_LOG_WARN("Input: %s を読み込めませんでした — 現在のバインドを維持します",
                          path.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults")) {
        input::InputActionMap::LoadDefaults();
        FBZZ_LOG_INFO("Input: 既定バインドへリセットしました");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", path.c_str());

    // リバインド中は他の操作を誤爆させないよう明示する。
    if (input::InputActionMap::IsRebinding()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.85f, 0.3f, 1.0f });
        ImGui::TextUnformatted("リバインド待機中 — 割り当てたい入力を押してください (Esc で取消)");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) input::InputActionMap::CancelRebind();
    }

    // ── ライブプレビュー ─────────────────────────────────────────────────────
    // WHY 必要か: アクション層は Play 中のみ有効 (エディット中の誤発火を防ぐため) なので、
    //     そのままではこのタブの "Current:" 表示やアクションのハイライトが常に 0 / 消灯になり、
    //     「今設定したバインドが効いているか」をここで確認できない。
    // WHY 安全か: Play 中でなければスクリプトは走っておらず、アクションを消費する側が居ない。
    //     評価するだけならシーンに影響しない。
    {
        static bool s_livePreview = false;
        if (ImGui::Checkbox("Live Preview", &s_livePreview))
            input::InputActionMap::SetEnabled(s_livePreview);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("バインドの動作をこのタブ上で確認できるようにします。\n"
                              "Play を開始・終了すると自動で切り替わります。");
        ImGui::SameLine();
        ImGui::TextDisabled(input::InputActionMap::IsEnabled() ? "(評価中)" : "(停止中)");

        // 外部 (PlayModeController) が状態を変えた場合にチェックボックス表示を追従させる。
        s_livePreview = input::InputActionMap::IsEnabled();
    }

    ImGui::Spacing();

    // ── 接続中のゲームパッド ─────────────────────────────────────────────────
    // WHY 実測値を出すか: スティックのドリフト量を目で見てデッドゾーンを決められる。
    //     「デッドゾーンをいくつにすべきか」は個体差があり、数値だけでは決められない。
    if (ImGui::CollapsingHeader("Connected Gamepads")) {
        bool anyConnected = false;
        for (int pad = 0; pad < input::Gamepad::MAX_PADS; ++pad) {
            if (!input::Gamepad::IsConnected(pad)) continue;
            anyConnected = true;

            ImGui::PushID(pad);
            ImGui::Text("Pad %d", pad);
            ImGui::Indent();

            const float leftX  = input::Gamepad::Axis(input::GamepadAxis::LEFT_STICK_X, pad);
            const float leftY  = input::Gamepad::Axis(input::GamepadAxis::LEFT_STICK_Y, pad);
            const float rightX = input::Gamepad::Axis(input::GamepadAxis::RIGHT_STICK_X, pad);
            const float rightY = input::Gamepad::Axis(input::GamepadAxis::RIGHT_STICK_Y, pad);
            ImGui::Text("Left Stick : %+.3f, %+.3f", leftX, leftY);
            ImGui::Text("Right Stick: %+.3f, %+.3f", rightX, rightY);
            ImGui::Text("Triggers   : L %.3f  R %.3f",
                        input::Gamepad::Axis(input::GamepadAxis::LEFT_TRIGGER, pad),
                        input::Gamepad::Axis(input::GamepadAxis::RIGHT_TRIGGER, pad));

            // 押下中のボタンを列挙する。
            std::string pressed;
            for (uint16_t i = 0; i < static_cast<uint16_t>(input::GamepadButton::COUNT); ++i) {
                const auto button = static_cast<input::GamepadButton>(i);
                if (!input::Gamepad::ButtonHeld(button, pad)) continue;
                if (!pressed.empty()) pressed += ", ";
                pressed += input::ToString(button);
            }
            ImGui::Text("Buttons    : %s", pressed.empty() ? "-" : pressed.c_str());

            if (ImGui::SmallButton("Test Vibration"))
                input::Gamepad::SetVibration(0.5f, 0.5f, 0.3f, pad);

            ImGui::Unindent();
            ImGui::PopID();
        }
        if (!anyConnected)
            ImGui::TextDisabled("接続されているゲームパッドはありません。");
    }

    ImGui::Spacing();

    // 編集はランタイムのマップを直接書き換える。保存は明示的な Save のみ。
    // WHY 自動保存しないか: リバインド途中の中途半端な状態がファイルへ落ちると、
    //     次回起動時に壊れたバインドで立ち上がる。確定操作をユーザーに握らせる。
    bool dirty = false;

    // ── 軸 ───────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Axes", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string axisToRemove;

        // GetAxes() は const 参照。編集には FindAxis() の可変参照を使う。
        std::vector<std::string> axisNames;
        axisNames.reserve(input::InputActionMap::GetAxes().size());
        for (const input::InputAxis& axis : input::InputActionMap::GetAxes())
            axisNames.push_back(axis.name);

        for (const std::string& name : axisNames) {
            input::InputAxis* axis = input::InputActionMap::FindAxis(name);
            if (!axis) continue;

            ImGui::PushID(name.c_str());
            if (ImGui::TreeNode(name.c_str())) {
                if (ImGui::DragFloat("Dead Zone", &axis->deadZone, 0.01f, 0.0f, 0.9f))
                    dirty = true;
                if (ImGui::DragFloat("Gravity", &axis->gravity, 0.1f, 0.0f, 100.0f))
                    dirty = true;
                if (ImGui::DragFloat("Sensitivity", &axis->sensitivity, 0.1f, 0.0f, 100.0f))
                    dirty = true;
                if (ImGui::Checkbox("Snap", &axis->snap)) dirty = true;
                ImGui::SameLine();
                if (ImGui::Checkbox("Raw", &axis->raw)) dirty = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Raw: デッドゾーンと平滑化を適用しない。\n"
                                      "マウス移動量のように既に相対量である入力へ使う。");

                // 現在値のライブ表示。バインドが期待どおり効いているかを即確認できる。
                ImGui::Text("Current: %+.3f", input::InputActionMap::GetAxis(name));

                ImGui::Spacing();

                DrawBindingList("Positive (押している間 +1)", axis->positive,
                    [&](int i) { input::InputActionMap::BeginRebindAxis(name, 0, i); },
                    [&](int i) { return input::InputActionMap::IsRebindTarget(name, 0, i); },
                    dirty);
                DrawBindingList("Negative (押している間 -1)", axis->negative,
                    [&](int i) { input::InputActionMap::BeginRebindAxis(name, 1, i); },
                    [&](int i) { return input::InputActionMap::IsRebindTarget(name, 1, i); },
                    dirty);
                DrawBindingList("Analog (スティック / マウス軸)", axis->analog,
                    [&](int i) { input::InputActionMap::BeginRebindAxis(name, 2, i); },
                    [&](int i) { return input::InputActionMap::IsRebindTarget(name, 2, i); },
                    dirty);

                ImGui::Spacing();
                if (ImGui::SmallButton("Remove Axis")) axisToRemove = name;

                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (!axisToRemove.empty()) {
            input::InputActionMap::RemoveAxis(axisToRemove);
            dirty = true;
        }

        ImGui::Spacing();
        static char s_newAxis[64] = {};
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("##newaxis", "New axis name", s_newAxis, sizeof(s_newAxis));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add Axis") && s_newAxis[0] != '\0') {
            input::InputAxis axis{};
            axis.name = s_newAxis;
            if (input::InputActionMap::AddAxis(axis)) {
                s_newAxis[0] = '\0';
                dirty = true;
            }
        }
    }

    ImGui::Spacing();

    // ── アクション ───────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string actionToRemove;

        std::vector<std::string> actionNames;
        actionNames.reserve(input::InputActionMap::GetActions().size());
        for (const input::InputAction& action : input::InputActionMap::GetActions())
            actionNames.push_back(action.name);

        for (const std::string& name : actionNames) {
            input::InputAction* action = input::InputActionMap::FindAction(name);
            if (!action) continue;

            ImGui::PushID(name.c_str());

            // 押下中のアクションは名前を光らせる。動作確認が Play を挟まずにできる。
            const bool held = input::InputActionMap::GetAction(name);
            if (held) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.4f, 1.0f, 0.5f, 1.0f });
            const bool open = ImGui::TreeNode(name.c_str());
            if (held) ImGui::PopStyleColor();

            if (open) {
                DrawBindingList("Bindings", action->bindings,
                    [&](int i) { input::InputActionMap::BeginRebindAction(name, i); },
                    [&](int i) { return input::InputActionMap::IsRebindTarget(name, -1, i); },
                    dirty);

                ImGui::Spacing();
                if (ImGui::SmallButton("Remove Action")) actionToRemove = name;

                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (!actionToRemove.empty()) {
            input::InputActionMap::RemoveAction(actionToRemove);
            dirty = true;
        }

        ImGui::Spacing();
        static char s_newAction[64] = {};
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("##newaction", "New action name", s_newAction, sizeof(s_newAction));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add Action") && s_newAction[0] != '\0') {
            input::InputAction action{};
            action.name = s_newAction;
            if (input::InputActionMap::AddAction(action)) {
                s_newAction[0] = '\0';
                dirty = true;
            }
        }
    }

    if (dirty) ++m_editGeneration;
}

void ProjectSettingsPanel::DrawAudio(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Audio");
    ImGui::Separator();

    bool dirty = ImGui::SliderFloat("Master Volume", &settings.audio.masterVolume, 0.0f, 1.0f);

    ImGui::Spacing();
    ImGui::TextUnformatted("Mixer Buses");
    ImGui::TextDisabled("AudioSource の Bus Name と、スクリプトの audio.SetBusVolume() が"
                        " ここで定義した名前を指す");

    auto& buses = settings.audio.buses;
    if (buses.empty()) {
        buses = audio::DefaultBusLayout();
        dirty = true;
    }

    int removeIndex = -1;
    for (size_t i = 0; i < buses.size(); ++i) {
        audio::BusDesc& bus = buses[i];
        const bool isMaster = bus.name == audio::kMasterBusName;
        ImGui::PushID(static_cast<int>(i));

        char nameBuffer[64];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", bus.name.c_str());
        ImGui::SetNextItemWidth(140.0f);
        if (isMaster) {
            ImGui::BeginDisabled();
            ImGui::InputText("##name", nameBuffer, sizeof(nameBuffer));
            ImGui::EndDisabled();
        } else if (ImGui::InputText("##name", nameBuffer, sizeof(nameBuffer))) {
            bus.name = nameBuffer;
            dirty = true;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        char parentBuffer[64];
        std::snprintf(parentBuffer, sizeof(parentBuffer), "%s", bus.parent.c_str());
        if (isMaster) {
            ImGui::BeginDisabled();
            ImGui::InputTextWithHint("##parent", "(output)", parentBuffer, sizeof(parentBuffer));
            ImGui::EndDisabled();
        } else if (ImGui::InputTextWithHint("##parent", "Master",
                                            parentBuffer, sizeof(parentBuffer))) {
            bus.parent = parentBuffer;
            dirty = true;
        }

        // Master の音量は上の Master Volume が正本なので、ここでは触らせない。
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (isMaster) {
            ImGui::BeginDisabled();
            float shown = settings.audio.masterVolume;
            ImGui::SliderFloat("##volume", &shown, 0.0f, 1.0f);
            ImGui::EndDisabled();
        } else {
            dirty |= ImGui::SliderFloat("##volume", &bus.volume, 0.0f, 1.0f);
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        dirty |= ImGui::SliderFloat("##lowpass", &bus.lowPassCutoff, 0.0f, 1.0f, "LPF %.2f");

        if (!isMaster) {
            ImGui::SameLine();
            if (ImGui::SmallButton("-")) removeIndex = static_cast<int>(i);
        }
        ImGui::PopID();
    }

    // 削除ボタンは Master 以外にしか出さないので、添字は素直に使える。
    if (removeIndex >= 0) {
        buses.erase(buses.begin() + removeIndex);
        dirty = true;
    }

    if (ImGui::Button("Add Bus")) {
        audio::BusDesc desc;
        desc.name   = "Bus " + std::to_string(buses.size());
        desc.parent = audio::kMasterBusName;
        buses.push_back(std::move(desc));
        dirty = true;
    }

    // WHY 即座に反映するか: 音量調整はスライダーを動かしながら耳で合わせる作業で、
    //     保存してから確かめる形にすると往復が成立しない。
    //     ただしバス構成の作り直しは再生中の音を止めるため、名前や親の変更では
    //     グラフを組み直さず、音量とフィルターだけを送る。
    if (dirty) {
        if (auto* audioManager = core::Application::Get().GetAudioManager()) {
            const std::vector<audio::BusDesc> layout = settings.audio.BuildBusLayout();
            const bool sameGraph =
                layout.size() == audioManager->BusLayout().size() &&
                std::equal(layout.begin(), layout.end(), audioManager->BusLayout().begin(),
                           [](const audio::BusDesc& a, const audio::BusDesc& b) {
                               return a.name == b.name && a.parent == b.parent;
                           });
            if (sameGraph) {
                for (const audio::BusDesc& desc : layout) {
                    audioManager->SetBusVolume(desc.name, desc.volume);
                    audioManager->SetBusLowPass(desc.name, desc.lowPassCutoff);
                }
            } else {
                audioManager->ApplyBusLayout(layout);
            }
        }
        ++m_editGeneration;
    }
}

void ProjectSettingsPanel::DrawTagsAndLayers(ProjectSettings& settings)
{
    ImGui::TextUnformatted("Tags & Layers");
    ImGui::Separator();

    // WHY 同居させるか: どちらも「GameObject を分類する ID テーブル」で、
    //      編集タイミングもほぼ同時 (新しい敵種別を足すときにタグとレイヤーを両方触る)。
    //      別項目に分けると往復が必要になるだけで、分離の利点がない。
    if (ImGui::CollapsingHeader("Tags", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        DrawTags(settings);
        ImGui::Unindent();
    }
    if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        DrawLayers(settings);
        ImGui::Unindent();
    }
}

void ProjectSettingsPanel::DrawTags(ProjectSettings& settings)
{
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
    if (ImGui::SmallButton("Reset Unity Preset##layers")) {
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
