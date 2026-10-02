/// @file    ProjectSettingsPanel.cpp
/// @brief   プロジェクト設定とエディターの個人設定を編集するパネル。
/// @author  Hasegawa Jin
/// @date    2026-05-23
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/FileDialog.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Gamepad.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/PipelineDiagnostics.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::editor {

namespace {

using Section = ProjectSettingsPanel::Section;

/// @brief 差分印を置くための左の余白 [px]。
constexpr float kMarkerGutter = 6.0f;
/// @brief Input.inputactions を書き出すまでの待ち [s]。ProjectSettings の自動保存と揃える。
constexpr float kInputSaveDelay = 1.0f;

/// @note px 直値だと UI スケールを上げたときに名前がはみ出す (日本語だと «アプリケーション» が最初に溢れる)。
float SidebarWidth() { return ImGui::GetFontSize() * 11.0f; }

struct SectionInfo {
    const char* name;
    const char* description;
    /// @brief 名前には出てこないが、そのセクションで探されそうな語。
    const char* keywords;
};

constexpr SectionInfo kSections[] = {
    { "Application",   "Frame rate, startup scenes, screen size and cursor images.",
      "fps scene startup screen resolution window cursor 解像度 画面 シーン" },
    { "Graphics",      "Rendering pipeline, shadows and debug overlays.",
      "render pipeline shadow cascade pcss debug outline navmesh 描画 影" },
    { "Physics",       "Simulation rate, gravity and which layers collide.",
      "gravity substep hz collision matrix 重力 衝突" },
    { "Input",         "Game input bindings, saved to Input.inputactions.",
      "gamepad key binding axis action pad キー 入力" },
    { "Audio",         "Master volume, voice limit and mixer buses.",
      "volume bus mixer reverb bgm se 音量" },
    { "Tags & Layers", "Names used to classify GameObjects.",
      "tag layer タグ レイヤー" },
    { "Import",        "Defaults and presets for new FBX imports.",
      "fbx preset model texture compression インポート" },
    { "Preferences",   "Your personal editor settings, saved to editor_settings.toml.",
      "editor language hot reload sound 言語 日本語 ホットリロード" },
};

const SectionInfo& Info(Section section) { return kSections[static_cast<std::size_t>(section)]; }

bool ContainsCI(const char* text, const char* query)
{
    return text != nullptr && util::StringUtils::ContainsCI(text, query);
}

/// @brief 原文と訳の両方に当てる。日本語表示のまま英語の資料の語で引けるようにする。
bool LabelHits(const char* label, const char* query)
{
    return ContainsCI(label, query) || ContainsCI(LOCT(label), query);
}

const ProjectSettings& ProjectDefaults()
{
    static const ProjectSettings defaults = ProjectSettings::Default();
    return defaults;
}

const EditorSettings& EditorDefaults()
{
    static const EditorSettings defaults{};
    return defaults;
}

/// @note 配列版と曖昧にならないよう、汎用版は配列を受けない (MSVC は部分順序で決め切らない)。
template <class T>
    requires (!std::is_array_v<T>)
bool SameValue(const T& a, const T& b) { return a == b; }

template <class T, std::size_t N>
bool SameValue(const T (&a)[N], const T (&b)[N]) { return std::equal(a, a + N, b); }

template <class T>
    requires (!std::is_array_v<T>)
void AssignValue(T& target, const T& source) { target = source; }

template <class T, std::size_t N>
void AssignValue(T (&target)[N], const T (&source)[N]) { std::copy(source, source + N, target); }

/// @brief 列挙値のコンボ。labels の並びが列挙子の値と一致していること。
template <class E, std::size_t N>
bool EnumCombo(E& value, const char* const (&labels)[N])
{
    const int current = std::clamp(static_cast<int>(value), 0, static_cast<int>(N) - 1);
    bool changed = false;
    if (ImGui::BeginCombo("##v", labels[current])) {
        for (int i = 0; i < static_cast<int>(N); ++i) {
            const bool selected = i == current;
            if (ImGui::Selectable(labels[i], selected) && !selected) {
                value = static_cast<E>(i);
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::string LocalClockNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

void StatusDot(ThemeColor color)
{
    ImGui::TextColored(EditorTheme::Color(color), "\xE2\x97\x8F");
    ImGui::SameLine();
}

/// @brief 取り消せない操作の確認。Open 済みのポップアップ id に対して描く。
/// @return 実行が選ばれたら true。
bool ConfirmPopup(const char* id, const char* message, const char* confirmLabel)
{
    bool confirmed = false;
    if (ImGui::BeginPopup(id)) {
        ImGui::TextUnformatted(message);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Danger));
        if (ImGui::Button(confirmLabel)) {
            confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(LOC("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return confirmed;
}


std::string ImportPresetsDir(const EditorContext& ctx)
{
    const std::string root = ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
    return util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(root) / ".import_presets");
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
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::PathToUtf8(entry.path().extension()));
        if (ext != ".toml") continue;
        std::string text;
        if (!util::FileSystem::ReadText(util::FileSystem::PathToUtf8(entry.path()), text)) continue;
        std::istringstream ss(text);
        const auto parsed = toml::parse(ss);
        if (!parsed) continue;
        PresetEntry p;
        p.name = util::FileSystem::PathToUtf8(entry.path().stem());
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
    std::sort(result.begin(), result.end(),
              [](const PresetEntry& a, const PresetEntry& b) { return a.name < b.name; });
    return result;
}

struct PresetCache {
    std::vector<PresetEntry> presets;
    bool loaded = false;
};

PresetCache& Presets()
{
    static PresetCache cache;
    return cache;
}


/// @note ランタイム側 (ProjectSettings::Load) が同じ規則で探す。ここが唯一の定義。
std::string InputActionsPath(const EditorContext& ctx)
{
    const std::string root = ctx.projectRoot.empty() ? "." : ctx.projectRoot;
    return util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(root) / "ProjectSettings" / "Input.inputactions");
}

/// @brief バインド 1 件の行。
/// @return 削除が要求されたら true。
bool DrawBindingRow(const input::InputBinding& binding, int index, bool rebinding,
                    bool& outRebindRequested)
{
    ImGui::PushID(index);
    const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::Bullet();
    ImGui::SameLine();

    /// @note 待機中の行を目立たせる。対象が分からないと別のバインドを潰す。
    if (rebinding)
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "%s", LOCT("< Press an input... (Esc to cancel) >"));
    else
        ImGui::TextUnformatted(input::InputActionMap::DescribeBinding(binding).c_str());

    const float buttons = ImGui::CalcTextSize(LOCT("Rebind")).x + ImGui::CalcTextSize(LOCT("Remove")).x
                        + ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), rowRight - buttons));
    if (ImGui::SmallButton(LOC("Rebind"))) outRebindRequested = true;
    ImGui::SameLine();
    const bool removeRequested = ImGui::SmallButton(LOC("Remove"));

    ImGui::PopID();
    return removeRequested;
}

void DrawBindingList(const char* label, std::vector<input::InputBinding>& bindings,
                     const std::function<void(int)>& beginRebind,
                     const std::function<bool(int)>& isRebindTarget, bool& outDirty)
{
    ImGui::TextDisabled("%s", label);
    ImGui::Indent();

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(bindings.size()); ++i) {
        bool rebindRequested = false;
        if (DrawBindingRow(bindings[static_cast<std::size_t>(i)], i, isRebindTarget(i), rebindRequested))
            removeIndex = i;
        if (rebindRequested) beginRebind(i);
    }
    if (removeIndex >= 0) {
        bindings.erase(bindings.begin() + removeIndex);
        outDirty = true;
    }

    /// @note 末尾に足してすぐ待機に入る。空のバインドを置いたままにすると «反応しない行» が残る。
    if (ImGui::SmallButton(LOC("+ Add Binding")))
        beginRebind(static_cast<int>(bindings.size()));

    ImGui::Unindent();
}

std::string JoinNames(const std::vector<std::string>& names)
{
    std::string joined;
    for (const std::string& name : names) {
        joined += name;
        joined += ' ';
    }
    return joined;
}

} /// @note namespace


void ProjectSettingsPanel::OnRenderContent(EditorContext& ctx)
{
    const bool trackUndo = ctx.undoStack && ctx.undoStack->IsRecordingEnabled();
    if (!trackUndo) {
        m_undo.active  = false;
        m_undo.changed = false;
    }
    /// @note 描く前の値を控える。Undo は «掴む前» の値へ戻す必要がある。
    const ProjectSettings beforeDraw = trackUndo ? ctx.projectSettings : ProjectSettings{};
    const ImGuiID activeBefore = ImGui::GetActiveID();
    const std::uint64_t generationBefore = m_editGeneration;

    DrawSidebar();
    ImGui::SameLine();

    ImGui::BeginChild("##ProjectSettingsContent", { 0.0f, 0.0f }, false);
    DrawSaveStatus(ctx);
    ImGui::Separator();
    ImGui::BeginChild("##ProjectSettingsScroll", { 0.0f, 0.0f }, false);
    DrawContent(ctx);
    ImGui::EndChild();
    ImGui::EndChild();

    if (trackUndo) TrackUndo(ctx, beforeDraw, activeBefore, generationBefore);
    FlushEditorPreferences(ctx);
    TickInputAutoSave(ctx);
}

void ProjectSettingsPanel::OnShutdown()
{
    if (!m_inputDirty || m_inputPath.empty()) return;
    if (!input::InputActionMap::SaveToFile(m_inputPath))
        FBZZ_LOG_ERROR("Input: failed to save %s on shutdown", m_inputPath.c_str());
    m_inputDirty = false;
}

void ProjectSettingsPanel::TrackUndo(EditorContext& ctx, const ProjectSettings& beforeDraw,
                                     ImGuiID activeBefore, std::uint64_t generationBefore)
{
    const ImGuiID activeAfter = ImGui::GetActiveID();
    const bool editedThisFrame = GImGui && GImGui->ActiveIdHasBeenEditedThisFrame;
    const bool structuralEdit = m_editGeneration != generationBefore;

    auto pushCommand = [&ctx](const ProjectSettings& before, const ProjectSettings& after) {
        /// @note ProjectSettings に触れない編集 (検索欄・Input・Preferences) で空の履歴を積まない。
        if (before.ToToml() == after.ToToml()) return;
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
        m_undo.active  = false;
        m_undo.changed = false;
    } else if (!m_undo.active && activeAfter != 0 && activeAfter != activeBefore) {
        m_undo.activeId = activeAfter;
        m_undo.before   = beforeDraw;
        m_undo.active   = true;
        m_undo.changed  = editedThisFrame;
    } else if (m_undo.active && activeAfter == m_undo.activeId) {
        m_undo.changed |= editedThisFrame;
    } else if (m_undo.active && activeAfter != m_undo.activeId) {
        if (m_undo.changed) pushCommand(m_undo.before, ctx.projectSettings);
        m_undo.active  = false;
        m_undo.changed = false;
    } else if (!m_undo.active && editedThisFrame && activeAfter == 0) {
        pushCommand(beforeDraw, ctx.projectSettings);
    }
}

void ProjectSettingsPanel::DrawSidebar()
{
    ImGui::BeginChild("##ProjectSettingsSidebar", { SidebarWidth(), 0.0f }, true);

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", LOCT("Search settings..."), m_search, sizeof(m_search));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", LOCT("Searches every section by name, description and value names (Ctrl+F)."));
    ImGui::Spacing();

    auto entry = [this](Section section) {
        const SectionInfo& info = Info(section);
        const int hits = m_lastSectionMatches[static_cast<std::size_t>(section)];
        char label[160];
        if (Searching())
            std::snprintf(label, sizeof(label), "%s  (%d)###%s", LOCT(info.name), hits, info.name);
        else
            std::snprintf(label, sizeof(label), "%s###%s", LOCT(info.name), info.name);

        const bool dim = Searching() && hits == 0;
        if (dim) ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
        /// @note 検索中に選ぶと検索を解いてそのセクションへ移る (結果一覧からの «ここへ行く»)。
        if (ImGui::Selectable(label, !Searching() && m_currentSection == section)) {
            m_currentSection = section;
            m_search[0] = '\0';
        }
        if (dim) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", LOCT(info.description));
    };

    ImGui::TextDisabled("%s", LOCT("PROJECT"));
    for (int i = 0; i < static_cast<int>(Section::Editor); ++i)
        entry(static_cast<Section>(i));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", LOCT("EDITOR"));
    entry(Section::Editor);

    ImGui::EndChild();
}

void ProjectSettingsPanel::DrawSaveStatus(EditorContext& ctx)
{
    const float lineRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::BeginGroup();

    const bool editorSide = !Searching()
        && (m_currentSection == Section::Editor || m_currentSection == Section::Import);
    const bool inputPage = !Searching() && m_currentSection == Section::Input;
    const char* tooltipPath = ctx.projectSettingsPath.c_str();

    if (editorSide) {
        StatusDot(m_editorPrefsDirty ? ThemeColor::Warning : ThemeColor::Success);
        ImGui::TextDisabled("%s", LOCT(m_editorPrefsDirty ? "Saving..." : "Saved automatically to editor_settings.toml"));
        tooltipPath = "Assets/EditorConfig/editor_settings.toml";
    } else if (inputPage) {
        tooltipPath = m_inputPath.c_str();
        switch (m_inputSaveState) {
        case InputSaveState::Saved:
            StatusDot(ThemeColor::Success);
            if (m_inputSavedClock.empty()) ImGui::TextDisabled("%s", LOCT("Input bindings are saved"));
            else ImGui::TextDisabled("%s %s", LOCT("Input bindings saved at"), m_inputSavedClock.c_str());
            break;
        case InputSaveState::Pending:
            StatusDot(ThemeColor::Warning);
            ImGui::TextUnformatted(LOCT(input::InputActionMap::IsRebinding()
                ? "Waiting for the rebind to finish before saving..." : "Saving..."));
            break;
        case InputSaveState::Failed:
            StatusDot(ThemeColor::Danger);
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger), "%s", LOCT("Could not save Input.inputactions"));
            ImGui::SameLine();
            if (ImGui::SmallButton(LOC("Retry"))) {
                m_inputDirty = true;
                m_inputIdle  = kInputSaveDelay;
            }
            break;
        }
    } else {
        switch (ctx.projectSettingsSaveState) {
        case EditorContext::SettingsSaveState::Saved:
            StatusDot(ThemeColor::Success);
            if (ctx.projectSettingsSavedClock.empty()) ImGui::TextDisabled("%s", LOCT("All changes are saved"));
            else ImGui::TextDisabled("%s %s", LOCT("Saved at"), ctx.projectSettingsSavedClock.c_str());
            break;
        case EditorContext::SettingsSaveState::Pending:
            StatusDot(ThemeColor::Warning);
            ImGui::TextUnformatted(LOCT("Saving..."));
            break;
        case EditorContext::SettingsSaveState::Failed:
            StatusDot(ThemeColor::Danger);
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger), "%s", LOCT("Could not save ProjectSettings.toml"));
            ImGui::SameLine();
            if (ImGui::SmallButton(LOC("Retry"))) ctx.requestProjectSettingsSave = true;
            break;
        }
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s\n%s", tooltipPath,
                          LOCT("Changes are saved automatically about a second after you stop editing."));

    /// @note 既定値へ戻す操作は右クリックにしか無いので、存在をここで教える。
    const char* hint = LOCT("Right-click a value to reset it");
    const float hintX = lineRight - ImGui::CalcTextSize(hint).x;
    ImGui::SameLine();
    if (hintX > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(hintX);
        ImGui::TextDisabled("%s", hint);
    } else {
        ImGui::NewLine();
    }
}

void ProjectSettingsPanel::DrawContent(EditorContext& ctx)
{
    ImGui::Indent(kMarkerGutter);

    if (!Searching()) {
        m_drawingSection = m_currentSection;
        const SectionInfo& info = Info(m_currentSection);
        widgets::BeginHeadingFont(1.2f);
        ImGui::TextUnformatted(LOCT(info.name));
        widgets::EndHeadingFont();
        ImGui::TextDisabled("%s", LOCT(info.description));
        DrawSection(ctx, m_currentSection);
    } else {
        /// @note 検索中はページ見出しを保留し、当たった行の直前で描く (FlushPendingHeaders)。
        m_matchCount = 0;
        m_sectionMatches.fill(0);
        for (int i = 0; i < static_cast<int>(Section::Count); ++i) {
            m_drawingSection = static_cast<Section>(i);
            const SectionInfo& info = Info(m_drawingSection);
            m_pendingPage  = true;
            m_pageMatched  = LabelHits(info.name, m_search) || ContainsCI(info.keywords, m_search);
            m_pendingGroup = nullptr;
            m_groupMatched = false;
            DrawSection(ctx, m_drawingSection);
            m_pendingPage = false;
            m_pageMatched = false;
        }
        m_lastSectionMatches = m_sectionMatches;
        if (m_matchCount == 0
            && widgets::EmptyState(nullptr, LOCT("No settings match"),
                                   LOCT("Try a different word, or clear the search."), LOC("Clear Search")))
            m_search[0] = '\0';
    }

    ImGui::Unindent(kMarkerGutter);
}

void ProjectSettingsPanel::DrawSection(EditorContext& ctx, Section section)
{
    ImGui::PushID(static_cast<int>(section));
    m_editingProjectSettings = section != Section::Import && section != Section::Editor;
    auto& settings = ctx.projectSettings;
    switch (section) {
    case Section::Application:   DrawApplication(ctx, settings); break;
    case Section::Graphics:      DrawGraphics(ctx, settings.render); break;
    case Section::Physics:       DrawPhysics(settings); break;
    case Section::Input:         DrawInput(ctx); break;
    case Section::Audio:         DrawAudio(settings); break;
    case Section::TagsAndLayers: DrawTagsAndLayers(settings); break;
    case Section::Import:        DrawImport(ctx); break;
    case Section::Editor:        DrawEditorPreferences(ctx); break;
    case Section::Count:         break;
    }
    EndGroup();
    m_editingProjectSettings = true;
    ImGui::PopID();
}


bool ProjectSettingsPanel::Matches(const char* label, const char* keywords)
{
    if (!Searching()) return true;
    const bool hit = m_pageMatched || m_groupMatched
                  || LabelHits(label, m_search) || ContainsCI(keywords, m_search);
    if (!hit) return false;
    FlushPendingHeaders();
    ++m_matchCount;
    ++m_sectionMatches[static_cast<std::size_t>(m_drawingSection)];
    return true;
}

bool ProjectSettingsPanel::BeginGroup(const char* label, bool defaultOpen, const char* keywords)
{
    if (Searching()) {
        m_pendingGroup = label;
        m_groupMatched = LabelHits(label, m_search) || ContainsCI(keywords, m_search);
        return true;
    }
    ImGui::Spacing();
    return ImGui::CollapsingHeader(LOC(label), defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

void ProjectSettingsPanel::EndGroup()
{
    m_pendingGroup = nullptr;
    m_groupMatched = false;
}

void ProjectSettingsPanel::FlushPendingHeaders()
{
    if (m_pendingPage) {
        m_pendingPage = false;
        const SectionInfo& info = Info(m_drawingSection);
        if (m_matchCount > 0) {
            ImGui::Spacing();
            ImGui::Spacing();
        }
        widgets::BeginHeadingFont(1.2f);
        ImGui::TextUnformatted(LOCT(info.name));
        widgets::EndHeadingFont();
        ImGui::SameLine();
        if (ImGui::SmallButton(LOC("Open"))) {
            m_currentSection = m_drawingSection;
            m_search[0] = '\0';
        }
        ImGui::Separator();
    }
    if (m_pendingGroup) {
        ImGui::SeparatorText(LOC(m_pendingGroup));
        m_pendingGroup = nullptr;
    }
}

template <class T, class Draw>
bool ProjectSettingsPanel::Field(const char* label, T& value, const T* def, const char* tooltip, Draw&& draw)
{
    if (!Matches(label, tooltip)) return false;

    const bool modified = def != nullptr && !SameValue(value, *def);
    const float markerX = ImGui::GetCursorScreenPos().x - kMarkerGutter + 1.0f;

    const widgets::PropertyRowScope row = widgets::BeginPropertyField(LOCT(label));
    bool changed = draw();
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    if (def != nullptr && ImGui::BeginPopupContextItem("##fieldMenu")) {
        if (ImGui::MenuItem(LOC("Reset to Default"), nullptr, false, modified)) {
            AssignValue(value, *def);
            changed = true;
            MarkStructuralEdit();
        }
        ImGui::EndPopup();
    }
    widgets::EndPropertyField(row);

    /// @note 既定値から変えた行だけ左端に印を付ける。«どこを触ったか» を一覧で拾えるようにする。
    if (modified) {
        const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
        ImGui::GetWindowDrawList()->AddRectFilled(
            { markerX, row.top }, { markerX + 3.0f, bottom },
            EditorTheme::ColorU32(ThemeColor::Accent), 1.5f);
        if (ImGui::IsMouseHoveringRect({ markerX - 2.0f, row.top }, { markerX + 5.0f, bottom }))
            ImGui::SetTooltip("%s", LOCT("Changed from the default. Right-click the value to reset it."));
    }

    if (changed && !m_editingProjectSettings) m_editorPrefsDirty = true;
    return changed;
}

void ProjectSettingsPanel::InfoRow(const char* label, const char* text)
{
    if (!Matches(label)) return;
    const widgets::PropertyRowScope row = widgets::BeginPropertyField(LOCT(label));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
    widgets::EndPropertyField(row);
}

void ProjectSettingsPanel::MarkStructuralEdit()
{
    if (m_editingProjectSettings) ++m_editGeneration;
    else m_editorPrefsDirty = true;
}

void ProjectSettingsPanel::FlushEditorPreferences(EditorContext& ctx)
{
    /// @note ドラッグ中に毎フレーム editor_settings.toml を書かない。手を離してから 1 回。
    if (!m_editorPrefsDirty || ImGui::IsAnyItemActive()) return;
    m_editorPrefsDirty = false;
    ctx.requestEditorSettingsSave = true;
}

void ProjectSettingsPanel::TickInputAutoSave(EditorContext& ctx)
{
    if (m_inputPath.empty()) m_inputPath = InputActionsPath(ctx);

    /// @note リバインドはパネルの外 (入力の到着) で確定するので、待機が明けた瞬間を変更として数える。
    const bool rebinding = input::InputActionMap::IsRebinding();
    if (m_wasRebinding && !rebinding) {
        m_inputDirty = true;
        m_inputIdle  = 0.0f;
    }
    m_wasRebinding = rebinding;
    if (!m_inputDirty) return;

    m_inputSaveState = InputSaveState::Pending;
    m_inputIdle += ImGui::GetIO().DeltaTime;
    /// @note 待機の途中でファイルへ落とすと、次の起動が壊れたバインドで立ち上がる。
    if (rebinding || ImGui::IsAnyItemActive() || m_inputIdle < kInputSaveDelay) return;

    m_inputDirty = false;
    if (input::InputActionMap::SaveToFile(m_inputPath)) {
        m_inputSaveState  = InputSaveState::Saved;
        m_inputSavedClock = LocalClockNow();
    } else {
        m_inputSaveState = InputSaveState::Failed;
        FBZZ_LOG_ERROR("Input: failed to save %s", m_inputPath.c_str());
    }
}


void ProjectSettingsPanel::DrawApplication(EditorContext& ctx, ProjectSettings& settings)
{
    const ProjectSettings& d = ProjectDefaults();

    if (BeginGroup("Frame Rate")) {
        if (Field("Target FPS", settings.app.targetFps, &d.app.targetFps,
                  "1 秒あたりの更新回数の上限。0 で無制限 (Editor の描画にも効く)",
                  [&] { return ImGui::DragInt("##v", &settings.app.targetFps, 1.0f, 0, 360, "%d fps"); }))
            Time::targetFps = settings.app.targetFps;
    }
    EndGroup();

    if (BeginGroup("Scenes")) {
        Field("Default Scene", settings.game.project.defaultScene, &d.game.project.defaultScene,
              "プロジェクトの既定シーン。Assets からの相対パス",
              [&] { return widgets::AssetPathField("##v", settings.game.project.defaultScene, ".scene", ctx.projectRoot); });
        Field("Start Scene", settings.game.runtime.startScene, &d.game.runtime.startScene,
              "実行時に最初に読み込むシーン。Assets からの相対パス",
              [&] { return widgets::AssetPathField("##v", settings.game.runtime.startScene, ".scene", ctx.projectRoot); });
    }
    EndGroup();

    if (BeginGroup("Screen")) {
        Field("Width", settings.screen.width, &d.screen.width, "画面の横幅 [px]",
              [&] { return ImGui::DragInt("##v", &settings.screen.width, 1.0f, 1, 7680, "%d px"); });
        Field("Height", settings.screen.height, &d.screen.height, "画面の高さ [px]",
              [&] { return ImGui::DragInt("##v", &settings.screen.height, 1.0f, 1, 4320, "%d px"); });
    }
    EndGroup();

    if (BeginGroup("Cursor", false))
        DrawCursor(ctx, settings.cursor);
    EndGroup();
}

void ProjectSettingsPanel::DrawCursor(EditorContext& ctx, CursorAppearance& cursor)
{
    /// @note 拘束と表示はここに無い。«その画面が今どう遊ばれているか» で決まり、スクリプトが cursor.Push で名乗る。
    const CursorAppearance& d = ProjectDefaults().cursor;
    Field("Hardware Cursor", cursor.hardwareCursor, &d.hardwareCursor,
          "OS カーソルの絵を下の画像で差し替える。外すと常に既定の矢印になる (自前で UI に描く場合など)",
          [&] { return ImGui::Checkbox("##v", &cursor.hardwareCursor); });

    if (!cursor.hardwareCursor) ImGui::BeginDisabled();
    for (std::size_t i = 0; i < core::kCursorShapeCount; ++i) {
        const auto shape = static_cast<core::CursorShape>(i);
        const char* name = core::ToString(shape);
        if (!Matches(name, "cursor image hotspot カーソル 画像")) continue;

        ImGui::PushID(static_cast<int>(i));
        auto& entry = cursor.shapes[i];
        const widgets::PropertyRowScope row = widgets::BeginPropertyField(name);
        if (widgets::AssetPathField("##v", entry.path, ".png,.tga,.bmp", ctx.projectRoot))
            MarkStructuralEdit();
        widgets::EndPropertyField(row);

        if (!entry.path.empty()) {
            float hotspot[2] = { entry.hotspotX, entry.hotspotY };
            const widgets::PropertyRowScope hotspotRow = widgets::BeginPropertyField(LOCT("Hotspot"));
            if (ImGui::DragFloat2("##v", hotspot, 1.0f, 0.0f, 4096.0f, "%.0f px")) {
                entry.hotspotX = hotspot[0];
                entry.hotspotY = hotspot[1];
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("画像の左上から «実際に指す点» までの画素。矢印なら先端、十字なら中心");
            widgets::EndPropertyField(hotspotRow);
        }
        ImGui::PopID();
    }
    if (!cursor.hardwareCursor) ImGui::EndDisabled();

    /// @note 編集中に反映しない。Editor ではウィンドウ全体のカーソルが変わり、パネルの上でもゲームの絵が出る。
    if (!Searching()) ImGui::TextDisabled("%s", LOCT("Images are loaded when Play starts."));
}


void ProjectSettingsPanel::DrawPipelineAsset(EditorContext& ctx)
{
    if (!BeginGroup("Render Pipeline Asset", true, "quality asset pipeline 品質 アセット")) {
        EndGroup();
        return;
    }
    const bool playing = ctx.playMode && !ctx.playMode->IsInEditor();
    ImGui::BeginDisabled(playing);
    auto& settings = ctx.projectSettings;
    renderer::RenderSettings resolved;
    bool valid = asset::ResolveRenderPipelineSettings(settings.render,
        settings.renderPipelineAssetPath, resolved);
    std::string reference = asset::DecodeGuidRef(settings.renderPipelineAssetPath);
    if (Field("Default Pipeline Asset", reference, static_cast<const std::string*>(nullptr),
        "RenderPipelineAsset (.fzdata)", [&] {
            return widgets::AssetPathField("##v", reference, ".fzdata", ctx.projectRoot);
        })) {
        if (reference.empty() && valid) settings.render = resolved;
        settings.renderPipelineAssetPath = asset::EncodeGuidRef(reference);
        valid = asset::ResolveRenderPipelineSettings(settings.render,
            settings.renderPipelineAssetPath, resolved);
        m_pipelineAssetError.clear();
        MarkStructuralEdit();
    }
    if (Matches("Pipeline Asset Actions", "create export detach inspect asset アセット 保存")) {
        if (ImGui::SmallButton(LOC("Save as Pipeline Asset..."))) {
            std::string destination;
            if (FileDialog::SaveFile(nullptr, {{"Render Pipeline Asset", "*.fzdata"}}, destination)) {
                std::error_code error;
                auto absolute = util::FileSystem::PathFromUtf8(destination);
                absolute.replace_extension(".fzdata");
                const auto assetsRoot = std::filesystem::weakly_canonical(
                    util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
                const auto canonical = error ? std::filesystem::path{}
                    : std::filesystem::weakly_canonical(absolute, error);
                const auto relative = error ? std::filesystem::path{} : canonical.lexically_relative(assetsRoot);
                const bool insideAssets = !relative.empty() && !relative.is_absolute()
                    && *relative.begin() != ".." && relative.extension() == ".fzdata";
                if (!insideAssets) {
                    m_pipelineAssetError = "Destination must be inside this project's Assets directory.";
                } else {
                    const std::string assetPath = "Assets/" + util::FileSystem::PathToUtf8(relative);
                    if (asset::CreateRenderPipelineAsset(assetPath, resolved)) {
                        const std::string diskPath = util::FileSystem::PathToUtf8(canonical);
                        (void)asset::AssetDatabase::GuidFromPath(diskPath);
                        settings.renderPipelineAssetPath = asset::EncodeGuidRef(assetPath);
                        valid = asset::ResolveRenderPipelineSettings(settings.render,
                            settings.renderPipelineAssetPath, resolved);
                        ctx.requestAssetBrowserRefresh = true;
                        SelectAsset(ctx, diskPath);
                        m_pipelineAssetError.clear();
                        MarkStructuralEdit();
                    } else {
                        m_pipelineAssetError = "Asset creation failed. Existing files are not replaced.";
                    }
                }
            }
        }
        if (!settings.renderPipelineAssetPath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(LOC("Inspect Asset"))) {
                SelectAsset(ctx, asset::AssetManager::ResolveAssetPath(settings.renderPipelineAssetPath));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(LOC("Detach Asset"))) {
                if (valid) settings.render = resolved;
                settings.renderPipelineAssetPath.clear();
                valid = false;
                m_pipelineAssetError.clear();
                MarkStructuralEdit();
            }
        }
    }
    ImGui::EndDisabled();
    if (!Searching()) {
        if (!m_pipelineAssetError.empty())
            ImGui::TextWrapped("%s", m_pipelineAssetError.c_str());
        else if (!settings.renderPipelineAssetPath.empty() && !valid)
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "%s", LOCT("Invalid pipeline asset: inline fallback active"));
        else
            ImGui::TextDisabled("%s", valid ? LOCT("Pipeline asset active") : LOCT("Inline settings active"));
    }
    EndGroup();
}

void ProjectSettingsPanel::DrawGraphics(EditorContext& ctx, renderer::RenderSettings& inlineRender)
{
    DrawPipelineAsset(ctx);
    renderer::RenderSettings resolved;
    const bool assigned = asset::ResolveRenderPipelineSettings(inlineRender,
        ctx.projectSettings.renderPipelineAssetPath, resolved);
    auto& render = assigned ? resolved : inlineRender;
    const bool playing = ctx.playMode && !ctx.playMode->IsInEditor();
    const renderer::RenderSettings& d = ProjectDefaults().render;

    if (BeginGroup("Rendering")) {
        ImGui::BeginDisabled(assigned || playing);
        static const char* const kRenderModes[] = { "Raster", "Hybrid", "Path Tracing" };
        static const char* const kPathProfiles[] = { "Reference", "Game" };
        Field("Render Mode", render.modeRequest.mode, &d.modeRequest.mode,
              "要求する描画構成。対応する GPU・シーン・描画パスが揃わない場合は Raster を使う。要求は保存される",
              [&] { return EnumCombo(render.modeRequest.mode, kRenderModes); });
        if (render.modeRequest.mode == renderer::RenderMode::HYBRID) {
            Field("Ray Traced Shadows", render.modeRequest.rayShadow, &d.modeRequest.rayShadow,
                  "影を RT へ置き換える要求。未対応なら ShadowMap を使う",
                  [&] { return ImGui::Checkbox("##v", &render.modeRequest.rayShadow); });
            Field("Ray Traced Reflections", render.modeRequest.rayReflection, &d.modeRequest.rayReflection,
                  "反射の RT 補完を要求。未対応なら既存の反射を使う",
                  [&] { return ImGui::Checkbox("##v", &render.modeRequest.rayReflection); });
            Field("Ray Traced Diffuse GI", render.modeRequest.rayDiffuseGi, &d.modeRequest.rayDiffuseGi,
                  "拡散間接光を RT へ置き換える要求。未対応なら既存の環境光を使う",
                  [&] { return ImGui::Checkbox("##v", &render.modeRequest.rayDiffuseGi); });
        }
        if (render.modeRequest.mode == renderer::RenderMode::PATH_TRACING) {
            Field("Path Profile", render.modeRequest.pathProfile, &d.modeRequest.pathProfile,
                  "Reference はカメラレイで参照画像を生成し、Game は Deferred の表面から照明を計算する要求",
                  [&] { return EnumCombo(render.modeRequest.pathProfile, kPathProfiles); });
        }
        if (!Searching() && ctx.resources) {
            for (const auto target : { scene::UIRenderTargetView::GameViewport,
                                       scene::UIRenderTargetView::SceneViewport }) {
                const auto* view = ctx.resources->Rendering().FindView(static_cast<uint32_t>(target) + 1u);
                if (!view || !view->output.IsValid() || view->nativeWidth == 0 || view->nativeHeight == 0)
                    continue;
                const auto& plan = view->renderPlan;
                const auto modeLabel = [&](renderer::RenderMode mode) {
                    const auto index = static_cast<uint32_t>(mode);
                    return index < 3u ? kRenderModes[index] : "Invalid";
                };
                ImGui::TextDisabled("%s 最終描画: %s -> %s (%u x %u)",
                    target == scene::UIRenderTargetView::GameViewport ? "Game View" : "Scene View",
                    modeLabel(plan.requestedMode), plan.IsValid() ? modeLabel(plan.effectiveMode) : "描画不可",
                    view->width, view->height);
                if (plan.fallbackReason != renderer::RenderPlanReason::NONE)
                    ImGui::TextWrapped("%s", renderer::DescribeRenderPlanReason(plan.fallbackReason));
                if (!plan.IsValid())
                    ImGui::TextWrapped("%s", renderer::DescribeRenderPlanReason(plan.failureReason));
            }
        }
        static const char* const kPipelines[] = { "Forward", "Deferred", "Forward+", "Deferred+" };
        Field("Pipeline", render.pipeline, &d.pipeline,
              "描画経路。+ 付きはクラスタ分割でライトを間引く (ライトが多い場面向け)",
              [&] { return EnumCombo(render.pipeline, kPipelines); });

        /// @note 効かない設定は選んだ Pipeline の真下に出す。原因がここなので、Volume 側に出しても直し方が分からない。
        if (!Searching()) {
            if (const auto inert = renderer::CollectInertSettings(render); !inert.empty()) {
                const ImVec4 warn = EditorTheme::Color(ThemeColor::Warning);
                ImGui::TextColored(warn, "%s (%d)", LOCT("Settings ignored by this pipeline"),
                                   static_cast<int>(inert.size()));
                ImGui::Indent();
                for (const renderer::InertSetting& issue : inert) {
                    ImGui::TextColored(warn, "%s", issue.label);
                    ImGui::Indent();
                    ImGui::TextDisabled("%s", issue.reason);
                    if (issue.remedy) ImGui::TextDisabled("-> %s", issue.remedy);
                    ImGui::Unindent();
                }
                ImGui::Unindent();
                ImGui::Spacing();
            }
        }

        const bool clustered = render.pipeline == renderer::RenderingPipeline::ForwardPlus
                            || render.pipeline == renderer::RenderingPipeline::DeferredPlus;
        if (clustered) {
            Field("Clustered Lights", render.clustered.enabled, &d.clustered.enabled,
                  "ライトを視錐台のクラスタへ振り分けて、画素ごとに近いライトだけを評価する",
                  [&] { return ImGui::Checkbox("##v", &render.clustered.enabled); });
            if (render.clustered.enabled) {
                Field("Cluster Distance", render.clustered.maxDistance, &d.clustered.maxDistance,
                      "クラスタを張る最大距離。これより遠いライトは振り分けない",
                      [&] { return ImGui::DragFloat("##v", &render.clustered.maxDistance, 1.0f, 1.0f, 10000.0f, "%.0f m"); });
            }
        }

        Field("Particle Budget", render.particleBudgetEnabled, &d.particleBudgetEnabled,
              "画面内のパーティクル総数に上限をかける",
              [&] { return ImGui::Checkbox("##v", &render.particleBudgetEnabled); });
        if (render.particleBudgetEnabled)
            Field("Particle Budget Count", render.particleBudget, &d.particleBudget, "上限の粒子数",
                  [&] { return ImGui::DragInt("##v", &render.particleBudget, 100.0f, 0, 1000000); });
        ImGui::EndDisabled();
    }
    EndGroup();

    if (BeginGroup("Shadows")) {
        ImGui::BeginDisabled(assigned || playing);
        DrawShadows(render);
        ImGui::EndDisabled();
    }
    EndGroup();

    if (BeginGroup("Player Options Preview", false, "brightness render scale 明るさ 解像度")) {
        ImGui::BeginDisabled(playing);
        DrawPlayerOptionsPreview(ctx, inlineRender);
        ImGui::EndDisabled();
    }
    EndGroup();

    if (BeginGroup("Debug Overlays", false))
        DrawDebugOverlays(inlineRender);
    EndGroup();

    /// @note Bloom / SSR / TAA などの «ルック» は場所ごとに変わるので、所有者は Post Process Volume に一本化してある。
    if (!Searching()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", LOCT("Post processing is configured per area with a Post Process Volume and a Post Process Profile (.fzdata)."));
    }
}

void ProjectSettingsPanel::DrawShadows(renderer::RenderSettings& render)
{
    const renderer::ShadowSettings& d = ProjectDefaults().render.shadow;
    Field("Shadows", render.shadowEnabled, &ProjectDefaults().render.shadowEnabled,
          "太陽 (Directional Light) の影を描く",
          [&] { return ImGui::Checkbox("##v", &render.shadowEnabled); });
    if (!render.shadowEnabled) return;

    static const uint32_t kResValues[] = { 512u, 1024u, 2048u, 4096u, 8192u };
    static const char* const kResLabels[] = { "512", "1024", "2048", "4096", "8192" };
    Field("Atlas Resolution", render.shadow.mapResolution, &d.mapResolution,
          "全カスケードが共有するアトラス 1 枚の解像度。2 分割以上では 2x2 タイルに分けるので、1 カスケードはこの値 / 2",
          [&] {
              int index = 3;
              for (int i = 0; i < 5; ++i)
                  if (kResValues[i] == render.shadow.mapResolution) index = i;
              if (!EnumCombo(index, kResLabels)) return false;
              render.shadow.mapResolution = kResValues[index];
              return true;
          });
    Field("Shadow Distance", render.shadow.autoFitDistance, &d.autoFitDistance,
          "影が届く最大距離。カスケード分割の全体レンジでもある",
          [&] { return ImGui::DragFloat("##v", &render.shadow.autoFitDistance, 1.0f, 5.0f, 2000.0f, "%.0f m"); });

    static const char* const kCascadeLabels[] = { "1 (single)", "2", "3", "4" };
    Field("Cascades", render.shadow.cascadeCount, &d.cascadeCount,
          "視錐台を距離で区切り、手前ほど狭い範囲に 1 タイルを割り当てる。同じ解像度のまま近距離の密度だけを上げられる",
          [&] {
              int index = std::clamp(render.shadow.cascadeCount, 1, renderer::kMaxShadowCascades) - 1;
              if (!EnumCombo(index, kCascadeLabels)) return false;
              render.shadow.cascadeCount = index + 1;
              return true;
          });

    if (render.shadow.cascadeCount > 1) {
        Field("Split Lambda", render.shadow.cascadeSplitLambda, &d.cascadeSplitLambda,
              "0 = 等分割 / 1 = 対数分割。上げるほど手前のカスケードが狭く (細かく) なる",
              [&] { return ImGui::SliderFloat("##v", &render.shadow.cascadeSplitLambda, 0.0f, 1.0f, "%.2f"); });
        Field("Cascade Blend", render.shadow.cascadeBlend, &d.cascadeBlend,
              "カスケード境界のクロスフェード幅",
              [&] { return ImGui::SliderFloat("##v", &render.shadow.cascadeBlend, 0.0f, 0.5f, "%.2f"); });
    }

    /// @note 解像度を上げるより分割数や距離を変える方が効くことが多い。それはこの表を見ないと判断できない。
    if (Matches("Cascade Texel Density", "cascade texel テクセル 密度")) {
        const int      count    = std::clamp(render.shadow.cascadeCount, 1, renderer::kMaxShadowCascades);
        const uint32_t tileSize = (std::max)(render.shadow.mapResolution / (count > 1 ? 2u : 1u), 1u);
        const float    nearZ    = 0.1f;
        const float    lambda   = std::clamp(render.shadow.cascadeSplitLambda, 0.0f, 1.0f);
        const float    distance = (std::max)(render.shadow.autoFitDistance, 1.0f);
        /// @note ComputeFrustumSliceSphere と同じ係数 (fovY 60 / aspect 16:9 の代表値)。
        constexpr float kSphereFactor = 1.177f;

        const widgets::PropertyRowScope row = widgets::BeginPropertyField(LOCT("Cascade Texel Density"));
        ImGui::BeginGroup();
        float sliceNear = nearZ;
        for (int i = 0; i < count; ++i) {
            const float ratio    = static_cast<float>(i + 1) / static_cast<float>(count);
            const float logSplit = nearZ * std::pow(distance / nearZ, ratio);
            const float uniform  = nearZ + (distance - nearZ) * ratio;
            const float sliceFar = (i == count - 1) ? distance : (lambda * logSplit + (1.0f - lambda) * uniform);
            const float texelCm  = (sliceFar * kSphereFactor * 2.0f) / static_cast<float>(tileSize) * 100.0f;
            ImGui::TextDisabled("#%d  %.1f-%.1f m   1 texel = %.1f cm", i, sliceNear, sliceFar, texelCm);
            sliceNear = sliceFar;
        }
        ImGui::EndGroup();
        widgets::EndPropertyField(row);
    }

    static const char* const kPcfLabels[] = { "0 (hard)", "1 (3x3)", "2 (5x5)", "3 (7x7)" };
    Field("PCF Radius", render.shadow.pcfRadius, &d.pcfRadius, "影の縁のぼかし幅",
          [&] {
              int index = std::clamp(render.shadow.pcfRadius, 0, 3);
              if (!EnumCombo(index, kPcfLabels)) return false;
              render.shadow.pcfRadius = index;
              return true;
          });
    Field("PCSS", render.shadow.pcssEnabled, &d.pcssEnabled,
          "Percentage Closer Soft Shadows。遮蔽物から離れるほど影の縁が広がる",
          [&] { return ImGui::Checkbox("##v", &render.shadow.pcssEnabled); });
    if (render.shadow.pcssEnabled) {
        Field("PCSS Light Radius", render.shadow.pcssLightRadius, &d.pcssLightRadius,
              "仮想的な光源の半径 (ワールド単位)。大きいほどソフト",
              [&] { return ImGui::DragFloat("##v", &render.shadow.pcssLightRadius, 0.1f, 0.0f, 50.0f, "%.2f"); });
    }
}

void ProjectSettingsPanel::DrawPlayerOptionsPreview(EditorContext& ctx, renderer::RenderSettings& render)
{
    /// @note 保存されない値を出すのは、効きを Play + スクリプト無しで目で確かめるため。
    if (!Searching())
        ImGui::TextDisabled("%s", LOCT("Values owned by the in-game Options screen. Changes here are not saved."));

    Field("Brightness", render.userBrightness, static_cast<const float*>(nullptr),
          "Option 画面の明るさ。確認用で保存されない",
          [&] { return ImGui::SliderFloat("##v", &render.userBrightness, 0.1f, 4.0f, "%.2f"); });

    /// @note 描画スケールは値が変わるたびに中間 RT を作り直す。ドラッグ中は draft に溜め、離した時点で 1 回だけ反映する。
    if (!m_renderScaleDragging) m_renderScaleDraft = render.renderScale;
    Field("Render Scale", m_renderScaleDraft, static_cast<const float*>(nullptr),
          "内部解像度の倍率。確認用で保存されない",
          [&] {
              ImGui::SliderFloat("##v", &m_renderScaleDraft, renderer::kMinRenderScale, renderer::kMaxRenderScale, "%.2f");
              m_renderScaleDragging = ImGui::IsItemActive();
              if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
              render.renderScale = m_renderScaleDraft;
              return true;
          });

    uint32_t internalWidth = 0, internalHeight = 0;
    renderer::ResolveRenderResolution(
        static_cast<uint32_t>((std::max)(ctx.viewportWidth, 1.0f)),
        static_cast<uint32_t>((std::max)(ctx.viewportHeight, 1.0f)),
        m_renderScaleDraft, internalWidth, internalHeight);
    char resolution[64];
    std::snprintf(resolution, sizeof(resolution), "%.0fx%.0f -> %ux%u",
                  ctx.viewportWidth, ctx.viewportHeight, internalWidth, internalHeight);
    InfoRow("Scene View Resolution", resolution);
}

void ProjectSettingsPanel::DrawDebugOverlays(renderer::RenderSettings& render)
{
    const renderer::RenderSettings& d = ProjectDefaults().render;
    auto toggle = [&](const char* label, bool renderer::RenderSettings::* member, const char* tooltip) {
        Field(label, render.*member, &(d.*member), tooltip,
              [&] { return ImGui::Checkbox("##v", &(render.*member)); });
    };
    toggle("Colliders",         &renderer::RenderSettings::showColliders,
           "Collider の形状をワイヤーで描く。緑 = 静的 / 黄 = 動く剛体 / 暗い黄 = 眠り / 紫 = トリガー");
    toggle("Terrain Collision", &renderer::RenderSettings::showTerrainCollision, "地形のコリジョン形状を描く");
    toggle("NavMesh",           &renderer::RenderSettings::showNavMesh, "NavMesh の歩行可能面を描く");
    toggle("AI Sensors",        &renderer::RenderSettings::showNavSensors, "NavMeshSensor の視界・聴覚範囲を描く");
    toggle("UI Rects",          &renderer::RenderSettings::showUIRects, "UI 要素の矩形とピボットを重ねる");
    toggle("Decal Bounds",      &renderer::RenderSettings::showDecalBounds, "デカールの投影範囲を描く");
    toggle("Selection Outline", &renderer::RenderSettings::showSelectionOutline, "選択中のオブジェクトに輪郭を付ける");
    if (render.showSelectionOutline) {
        Field("Outline Width", render.outlineWidth, &d.outlineWidth, "選択輪郭の太さ",
              [&] { return ImGui::SliderFloat("##v", &render.outlineWidth, 0.005f, 0.2f, "%.3f"); });
        Field("Outline Color", render.outlineColor, &d.outlineColor, "選択輪郭の色",
              [&] { return ImGui::ColorEdit4("##v", render.outlineColor); });
    }
    static const char* const kViewModes[] = { "Lit", "Unlit", "Wireframe Lit", "Wireframe Unlit" };
    Field("View Mode", render.viewMode, &d.viewMode, "ライティングとワイヤーフレームの表示切り替え",
          [&] { return EnumCombo(render.viewMode, kViewModes); });
    Field("Cluster Heatmap", render.clustered.debugHeatmap, &d.clustered.debugHeatmap,
          "クラスタごとのライト数を色で重ねる (デバッグ)",
          [&] { return ImGui::Checkbox("##v", &render.clustered.debugHeatmap); });
    Field("Force All Lights", render.clustered.forceAllLights, &d.clustered.forceAllLights,
          "振り分けを無視して全ライトを評価する",
          [&] { return ImGui::Checkbox("##v", &render.clustered.forceAllLights); });
    Field("Visualize Cascades", render.shadow.debugVisualizeCascades, &d.shadow.debugVisualizeCascades,
          "カスケードを色分けする",
          [&] { return ImGui::Checkbox("##v", &render.shadow.debugVisualizeCascades); });

    /// @note Script Gizmos / Skeleton / IK などはエディター設定が正本で毎フレーム上書きされるので、置き場所だけ案内する。
    if (!Searching()) {
        ImGui::TextDisabled("%s", LOCT("Scene View only overlays (Script Gizmos, Skeleton, IK, ...): Viewport > Overlays"));
        ImGui::TextDisabled("%s", LOCT("Render Pass Viewer: Debug > Render Pass Viewer"));
    }
}


void ProjectSettingsPanel::DrawPhysics(ProjectSettings& settings)
{
    const PhysicsSettings& d = ProjectDefaults().physics;

    if (BeginGroup("Simulation")) {
        Field("Fixed Rate", settings.physics.hz, &d.hz, "物理の固定更新回数 [Hz]。上げるほど安定するが重い",
              [&] { return ImGui::DragInt("##v", &settings.physics.hz, 1.0f, 1, 1000, "%d Hz"); });
        Field("Substeps", settings.physics.substeps, &d.substeps, "1 回の固定更新をさらに分割する数",
              [&] { return ImGui::DragInt("##v", &settings.physics.substeps, 1.0f, 1, 32); });
        Field("Gravity", settings.physics.gravity, &d.gravity, "重力加速度 [m/s^2]。キャラクターのジャンプもこれを読む",
              [&] { return widgets::DragAxes("##v", settings.physics.gravity, 0.05f, -1000.0f, 1000.0f, "%.2f"); });
    }
    EndGroup();

    if (BeginGroup("Layer Collision Matrix", true, "collision layer matrix 衝突 レイヤー"))
        DrawCollisionMatrix(settings);
    EndGroup();
}

void ProjectSettingsPanel::DrawCollisionMatrix(ProjectSettings& settings)
{
    if (!Matches("Layer Collision Matrix", "collision layer matrix 衝突")) return;

    /// @note 名前の付いたレイヤーだけ並べる。32 本すべてだと 1024 マスになり、使っている数本を探せない。
    std::vector<int> used;
    for (int i = 0; i < 32; ++i)
        if (i == Layer::Default || !settings.game.layerNames[static_cast<std::size_t>(i)].empty())
            used.push_back(i);

    if (used.size() < 2) {
        ImGui::TextDisabled("%s", LOCT("Name at least two layers in Tags & Layers to edit the matrix."));
        return;
    }
    ImGui::TextDisabled("%s", LOCT("Unchecked pairs never collide. Triggers are filtered too."));

    /// @note 対称行列なので上三角だけ出す。全面だと同じ組が 2 回現れ、どちらを触ったのか分からない。
    if (ImGui::BeginTable("##collisionMatrix", static_cast<int>(used.size()) + 1,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInner
                          | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("");
        for (int column : used)
            ImGui::TableSetupColumn(LayerLabel(settings, column).c_str(), ImGuiTableColumnFlags_AngledHeader);
        ImGui::TableAngledHeadersRow();
        ImGui::TableHeadersRow();

        for (std::size_t row = 0; row < used.size(); ++row) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(LayerLabel(settings, used[row]).c_str());
            for (std::size_t column = 0; column < used.size(); ++column) {
                ImGui::TableNextColumn();
                if (column < row) continue;
                const int a = used[row];
                const int b = used[column];
                bool collide = settings.physics.collisionMatrix.CanCollide(a, b);
                ImGui::PushID(a * 32 + b);
                if (ImGui::Checkbox("##pair", &collide))
                    settings.physics.collisionMatrix.Set(a, b, collide);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s  x  %s", LayerLabel(settings, a).c_str(), LayerLabel(settings, b).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

std::string ProjectSettingsPanel::LayerLabel(const ProjectSettings& settings, int layer)
{
    const std::string& name = settings.game.layerNames[static_cast<std::size_t>(layer)];
    return std::to_string(layer) + ": " + (name.empty() ? "Default" : name);
}


void ProjectSettingsPanel::DrawInput(EditorContext& ctx)
{
    m_inputPath = InputActionsPath(ctx);

    if (!Searching()) {
        if (ImGui::Button(LOC("Reload from Disk"))) {
            if (input::InputActionMap::LoadFromFile(m_inputPath)) {
                m_inputDirty     = false;
                m_inputSaveState = InputSaveState::Saved;
            } else {
                FBZZ_LOG_WARN("Input: could not read %s; keeping current bindings", m_inputPath.c_str());
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Input.inputactions を読み直す。まだ書き出していない変更は捨てる");
        ImGui::SameLine();
        if (ImGui::Button(LOC("Reset to Defaults...")))
            ImGui::OpenPopup("##resetInput");
        if (ConfirmPopup("##resetInput", LOCT("Replace every binding with the engine defaults?"), LOC("Reset"))) {
            input::InputActionMap::LoadDefaults();
            m_inputDirty = true;
            m_inputIdle  = 0.0f;
        }

        /// @note 待機中は他の操作を誤爆させないよう、何を待っているかを明示する。
        if (input::InputActionMap::IsRebinding()) {
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "%s",
                               LOCT("Waiting for input. Press the key or button to assign (Esc to cancel)."));
            ImGui::SameLine();
            if (ImGui::SmallButton(LOC("Cancel"))) input::InputActionMap::CancelRebind();
        }
    }

    if (BeginGroup("Preview")) {
        /// @note アクション層は Play 中だけ有効。そのままでは «Current» 表示もハイライトも常に 0 で、ここで確かめられない。
        bool live = input::InputActionMap::IsEnabled();
        Field("Live Preview", live, static_cast<const bool*>(nullptr),
              "バインドの動作を Play せずにこのタブで確かめる。Play の開始・終了で自動的に切り替わる",
              [&] {
                  if (!ImGui::Checkbox("##v", &live)) return false;
                  input::InputActionMap::SetEnabled(live);
                  return true;
              });
    }
    EndGroup();

    if (BeginGroup("Connected Gamepads", false, "gamepad pad stick trigger vibration ゲームパッド")) {
        /// @note 実測値を出す。スティックのドリフト量は個体差があり、デッドゾーンは数値だけでは決められない。
        if (Matches("Connected Gamepads", "gamepad pad stick trigger vibration ゲームパッド")) {
            bool anyConnected = false;
            for (int pad = 0; pad < input::Gamepad::MAX_PADS; ++pad) {
                if (!input::Gamepad::IsConnected(pad)) continue;
                anyConnected = true;
                ImGui::PushID(pad);
                ImGui::Text("Pad %d", pad);
                ImGui::Indent();
                ImGui::Text("Left Stick : %+.3f, %+.3f",
                            input::Gamepad::Axis(input::GamepadAxis::LEFT_STICK_X, pad),
                            input::Gamepad::Axis(input::GamepadAxis::LEFT_STICK_Y, pad));
                ImGui::Text("Right Stick: %+.3f, %+.3f",
                            input::Gamepad::Axis(input::GamepadAxis::RIGHT_STICK_X, pad),
                            input::Gamepad::Axis(input::GamepadAxis::RIGHT_STICK_Y, pad));
                ImGui::Text("Triggers   : L %.3f  R %.3f",
                            input::Gamepad::Axis(input::GamepadAxis::LEFT_TRIGGER, pad),
                            input::Gamepad::Axis(input::GamepadAxis::RIGHT_TRIGGER, pad));
                std::string pressed;
                for (uint16_t i = 0; i < static_cast<uint16_t>(input::GamepadButton::COUNT); ++i) {
                    const auto button = static_cast<input::GamepadButton>(i);
                    if (!input::Gamepad::ButtonHeld(button, pad)) continue;
                    if (!pressed.empty()) pressed += ", ";
                    pressed += input::ToString(button);
                }
                ImGui::Text("Buttons    : %s", pressed.empty() ? "-" : pressed.c_str());
                if (ImGui::SmallButton(LOC("Test Vibration")))
                    input::Gamepad::SetVibration(0.5f, 0.5f, 0.3f, pad);
                ImGui::Unindent();
                ImGui::PopID();
            }
            if (!anyConnected) ImGui::TextDisabled("%s", LOCT("No gamepads are connected."));
        }
    }
    EndGroup();

    bool dirty = false;
    DrawInputAxes(dirty);
    DrawInputActions(dirty);
    if (dirty) {
        m_inputDirty = true;
        m_inputIdle  = 0.0f;
    }
}

void ProjectSettingsPanel::DrawInputAxes(bool& dirty)
{
    std::vector<std::string> names;
    for (const input::InputAxis& axis : input::InputActionMap::GetAxes()) names.push_back(axis.name);
    const std::string keywords = JoinNames(names) + "axis dead zone sensitivity 軸";

    if (BeginGroup("Axes", true, keywords.c_str())) {
        static const input::InputAxis kAxisDefaults{};
        std::string axisToRemove;

        for (const std::string& name : names) {
            input::InputAxis* axis = input::InputActionMap::FindAxis(name);
            if (!axis) continue;
            const bool groupMatched = m_groupMatched;
            if (!Matches(name.c_str())) continue;
            /// @note 軸名で当たったなら中の行も全部出す。
            m_groupMatched = true;

            ImGui::PushID(name.c_str());
            if (Searching()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            if (ImGui::TreeNode(name.c_str())) {
                dirty |= Field("Dead Zone", axis->deadZone, &kAxisDefaults.deadZone,
                               "この値より小さい入力を 0 とみなす。スティックのドリフト対策",
                               [&] { return ImGui::SliderFloat("##v", &axis->deadZone, 0.0f, 0.9f, "%.2f"); });
                dirty |= Field("Gravity", axis->gravity, &kAxisDefaults.gravity,
                               "キーを離したとき 0 へ戻る速さ [1/s]",
                               [&] { return ImGui::DragFloat("##v", &axis->gravity, 0.1f, 0.0f, 100.0f, "%.1f"); });
                dirty |= Field("Sensitivity", axis->sensitivity, &kAxisDefaults.sensitivity,
                               "キーを押したとき目標値へ向かう速さ [1/s]",
                               [&] { return ImGui::DragFloat("##v", &axis->sensitivity, 0.1f, 0.0f, 100.0f, "%.1f"); });
                dirty |= Field("Snap", axis->snap, &kAxisDefaults.snap,
                               "逆方向へ切り返したとき、いったん 0 から始める",
                               [&] { return ImGui::Checkbox("##v", &axis->snap); });
                dirty |= Field("Raw", axis->raw, &kAxisDefaults.raw,
                               "デッドゾーンと平滑化を適用しない。マウス移動量のように既に相対量である入力へ使う",
                               [&] { return ImGui::Checkbox("##v", &axis->raw); });

                char current[32];
                std::snprintf(current, sizeof(current), "%+.3f", input::InputActionMap::GetAxis(name));
                InfoRow("Current Value", current);

                ImGui::Spacing();
                DrawBindingList(LOCT("Positive (+1 while held)"), axis->positive,
                    [&](int i) { input::InputActionMap::BeginRebindAxis(name, 0, i); },
                    [&](int i) { return input::InputActionMap::IsRebindTarget(name, 0, i); }, dirty);
                DrawBindingList(LOCT("Negative (-1 while held)"), axis->negative,
                    [&](int i) { input::InputActionMap::BeginRebindAxis(name, 1, i); },
                    [&](int i) { return input::InputActionMap::IsRebindTarget(name, 1, i); }, dirty);
                DrawBindingList(LOCT("Analog (stick / mouse axis)"), axis->analog,
                    [&](int i) { input::InputActionMap::BeginRebindAxis(name, 2, i); },
                    [&](int i) { return input::InputActionMap::IsRebindTarget(name, 2, i); }, dirty);

                ImGui::Spacing();
                if (ImGui::SmallButton(LOC("Remove Axis"))) axisToRemove = name;
                ImGui::TreePop();
            }
            ImGui::PopID();
            m_groupMatched = groupMatched;
        }

        if (!axisToRemove.empty()) {
            input::InputActionMap::RemoveAxis(axisToRemove);
            dirty = true;
        }

        if (!Searching()) {
            static char s_newAxis[64] = {};
            ImGui::Spacing();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            const bool submit = ImGui::InputTextWithHint("##newaxis", LOCT("New axis name"), s_newAxis,
                                                         sizeof(s_newAxis), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            ImGui::BeginDisabled(s_newAxis[0] == '\0');
            if ((ImGui::SmallButton(LOC("Add Axis")) || submit) && s_newAxis[0] != '\0') {
                input::InputAxis axis{};
                axis.name = s_newAxis;
                if (input::InputActionMap::AddAxis(axis)) {
                    s_newAxis[0] = '\0';
                    dirty = true;
                }
            }
            ImGui::EndDisabled();
        }
    }
    EndGroup();
}

void ProjectSettingsPanel::DrawInputActions(bool& dirty)
{
    std::vector<std::string> names;
    for (const input::InputAction& action : input::InputActionMap::GetActions()) names.push_back(action.name);
    const std::string keywords = JoinNames(names) + "action binding アクション";

    if (BeginGroup("Actions", true, keywords.c_str())) {
        std::string actionToRemove;
        for (const std::string& name : names) {
            input::InputAction* action = input::InputActionMap::FindAction(name);
            if (!action) continue;
            if (!Matches(name.c_str())) continue;

            ImGui::PushID(name.c_str());
            /// @note 押下中のアクションは名前を光らせる。動作確認が Play を挟まずにできる。
            const bool held = input::InputActionMap::GetAction(name);
            if (held) ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Success));
            if (Searching()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            const bool open = ImGui::TreeNode(name.c_str());
            if (held) ImGui::PopStyleColor();

            if (open) {
                DrawBindingList(LOCT("Bindings"), action->bindings,
                    [&](int i) { input::InputActionMap::BeginRebindAction(name, i); },
                    [&](int i) { return input::InputActionMap::IsRebindTarget(name, -1, i); }, dirty);
                ImGui::Spacing();
                if (ImGui::SmallButton(LOC("Remove Action"))) actionToRemove = name;
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (!actionToRemove.empty()) {
            input::InputActionMap::RemoveAction(actionToRemove);
            dirty = true;
        }

        if (!Searching()) {
            static char s_newAction[64] = {};
            ImGui::Spacing();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            const bool submit = ImGui::InputTextWithHint("##newaction", LOCT("New action name"), s_newAction,
                                                         sizeof(s_newAction), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            ImGui::BeginDisabled(s_newAction[0] == '\0');
            if ((ImGui::SmallButton(LOC("Add Action")) || submit) && s_newAction[0] != '\0') {
                input::InputAction action{};
                action.name = s_newAction;
                if (input::InputActionMap::AddAction(action)) {
                    s_newAction[0] = '\0';
                    dirty = true;
                }
            }
            ImGui::EndDisabled();
        }
    }
    EndGroup();
}


void ProjectSettingsPanel::DrawAudio(ProjectSettings& settings)
{
    const AudioSettings& d = ProjectDefaults().audio;
    bool dirty = false;

    if (BeginGroup("Output")) {
        dirty |= Field("Master Volume", settings.audio.masterVolume, &d.masterVolume, "全体の音量",
                       [&] { return ImGui::SliderFloat("##v", &settings.audio.masterVolume, 0.0f, 1.0f, "%.2f"); });
        dirty |= Field("Voice Limit", settings.audio.voiceLimit, &d.voiceLimit,
                       "同時発音の上限。超えると AudioSource の Priority が低い音から畳まれる (ループ音は畳まれない)",
                       [&] { return ImGui::SliderInt("##v", &settings.audio.voiceLimit, 8, 128); });
        if (auto* audioManager = core::Application::Get().GetAudioManager()) {
            char voices[32];
            std::snprintf(voices, sizeof(voices), "%zu", audioManager->ActiveVoiceCount());
            InfoRow("Active Voices", voices);
        }
    }
    EndGroup();

    auto& buses = settings.audio.buses;
    if (buses.empty()) {
        buses = audio::DefaultBusLayout();
        dirty = true;
    }

    std::vector<std::string> busNames;
    for (const audio::BusDesc& bus : buses) busNames.push_back(bus.name);
    const std::string busKeywords = JoinNames(busNames) + "bus mixer reverb low-pass バス ミキサー 残響";

    if (BeginGroup("Mixer Buses", true, busKeywords.c_str()) && Matches("Mixer Buses", busKeywords.c_str())) {
        ImGui::TextDisabled("%s", LOCT("AudioSource Bus Name and audio.SetBusVolume() refer to these names."));

        int removeIndex = -1;
        if (ImGui::BeginTable("##buses", 6, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                                           | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(LOCT("Name"),     ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn(LOCT("Parent"),   ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn(LOCT("Volume"),   ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn(LOCT("Low-pass"), ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn(LOCT("Reverb"),   ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("",               ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < buses.size(); ++i) {
                audio::BusDesc& bus = buses[i];
                const bool isMaster = bus.name == audio::kMasterBusName;
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::BeginDisabled(isMaster);
                dirty |= widgets::InputString("##name", bus.name, 64);
                ImGui::EndDisabled();

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (isMaster) {
                    ImGui::TextDisabled("%s", LOCT("(output)"));
                } else {
                    dirty |= widgets::InputString("##parent", bus.parent, 64);
                }

                /// @note Master の音量は上の Master Volume が正本。ここでは触らせない。
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (isMaster) {
                    float shown = settings.audio.masterVolume;
                    ImGui::BeginDisabled();
                    ImGui::SliderFloat("##volume", &shown, 0.0f, 1.0f, "%.2f");
                    ImGui::EndDisabled();
                } else {
                    dirty |= ImGui::SliderFloat("##volume", &bus.volume, 0.0f, 1.0f, "%.2f");
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                dirty |= ImGui::SliderFloat("##lowpass", &bus.lowPassCutoff, 0.0f, 1.0f, "%.2f");

                ImGui::TableNextColumn();
                dirty |= ImGui::Checkbox("##reverb", &bus.reverb);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("AudioReverbZone の残響を受ける。切り替えると再生中の音がいったん止まる");

                ImGui::TableNextColumn();
                if (!isMaster && ImGui::SmallButton("x")) removeIndex = static_cast<int>(i);
                if (!isMaster && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", LOCT("Remove bus"));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (removeIndex >= 0) {
            buses.erase(buses.begin() + removeIndex);
            dirty = true;
            MarkStructuralEdit();
        }
        if (!Searching() && ImGui::SmallButton(LOC("+ Add Bus"))) {
            audio::BusDesc desc;
            desc.name   = "Bus " + std::to_string(buses.size());
            desc.parent = audio::kMasterBusName;
            buses.push_back(std::move(desc));
            dirty = true;
            MarkStructuralEdit();
        }
    }
    EndGroup();

    /// @note 音量は耳で合わせる作業なので即座に送る。ただしバス構成の組み直しは再生中の音を止めるので、
                    /// @note 名前・親・残響が変わったときだけ組み直す。
    if (!dirty) return;
    auto* audioManager = core::Application::Get().GetAudioManager();
    if (!audioManager) return;
    audioManager->SetVoiceLimit(static_cast<std::size_t>(settings.audio.voiceLimit));
    const std::vector<audio::BusDesc> layout = settings.audio.BuildBusLayout();
    const bool sameGraph =
        layout.size() == audioManager->BusLayout().size() &&
        std::equal(layout.begin(), layout.end(), audioManager->BusLayout().begin(),
                   [](const audio::BusDesc& a, const audio::BusDesc& b) {
                       /// @note 残響 DSP は submix の生成時にしか差し込めないので、reverb も組み直しの条件に入れる。
                       return a.name == b.name && a.parent == b.parent && a.reverb == b.reverb;
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


void ProjectSettingsPanel::DrawTagsAndLayers(ProjectSettings& settings)
{
    /// @note 同居させるのは、どちらも GameObject を分類する ID 表で、新しい敵種別を足すときに両方触るから。
    const std::string tagKeywords = JoinNames(settings.game.tags) + "tag タグ";
    if (BeginGroup("Tags", true, tagKeywords.c_str()) && Matches("Tag List", tagKeywords.c_str()))
        DrawTags(settings);
    EndGroup();

    std::vector<std::string> layerNames(settings.game.layerNames.begin(), settings.game.layerNames.end());
    const std::string layerKeywords = JoinNames(layerNames) + "layer レイヤー";
    if (BeginGroup("Layers", true, layerKeywords.c_str()) && Matches("Layer List", layerKeywords.c_str()))
        DrawLayers(settings);
    EndGroup();
}

void ProjectSettingsPanel::DrawTags(ProjectSettings& settings)
{
    auto& tags = settings.game.tags;
    const auto isDuplicate = [&tags](const std::string& tag) {
        return std::count(tags.begin(), tags.end(), tag) > 1;
    };

    int removeIndex = -1;
    if (ImGui::BeginTable("##tags", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##name",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 0; i < static_cast<int>(tags.size()); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            /// @note Untagged は «タグ無し» の意味を持つ予約名なので改名も削除もさせない。
            const bool reserved = tags[static_cast<std::size_t>(i)] == "Untagged";
            const bool duplicate = isDuplicate(tags[static_cast<std::size_t>(i)]);
            if (duplicate) {
                ImVec4 warn = EditorTheme::Color(ThemeColor::Danger);
                warn.w = 0.35f;
                ImGui::PushStyleColor(ImGuiCol_FrameBg, warn);
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(reserved);
            widgets::InputString("##tag", tags[static_cast<std::size_t>(i)], 64);
            ImGui::EndDisabled();
            if (duplicate) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", LOCT("Another tag has the same name."));
            }
            ImGui::TableNextColumn();
            if (!reserved && ImGui::SmallButton(LOC("Remove"))) removeIndex = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeIndex >= 0) {
        tags.erase(tags.begin() + removeIndex);
        MarkStructuralEdit();
    }

    if (Searching()) return;

    const std::string newTag = m_newTag;
    const bool exists = std::find(tags.begin(), tags.end(), newTag) != tags.end();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    const bool submit = ImGui::InputTextWithHint("##newtag", LOCT("New tag name"), m_newTag, sizeof(m_newTag),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(newTag.empty() || exists);
    if ((ImGui::SmallButton(LOC("Add Tag")) || submit) && !newTag.empty() && !exists) {
        tags.push_back(newTag);
        m_newTag[0] = '\0';
        MarkStructuralEdit();
    }
    ImGui::EndDisabled();
    if (exists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", LOCT("A tag with this name already exists."));

    ImGui::SameLine();
    if (ImGui::SmallButton(LOC("Reset to Unity Preset...")))
        ImGui::OpenPopup("##resetTags");
    if (ConfirmPopup("##resetTags", LOCT("Replace all tags with the Unity preset? Custom tags are removed."), LOC("Reset"))) {
        tags = ProjectDefaults().game.tags;
        MarkStructuralEdit();
    }
}

void ProjectSettingsPanel::DrawLayers(ProjectSettings& settings)
{
    if (ImGui::BeginTable("##layers", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##index", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##name",  ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < 32; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%2d", i);
            ImGui::TableNextColumn();
            char hint[32];
            std::snprintf(hint, sizeof(hint), "User Layer %d", i);
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s", settings.game.layerNames[static_cast<std::size_t>(i)].c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##layer", hint, buffer, sizeof(buffer)))
                settings.game.layerNames[static_cast<std::size_t>(i)] = buffer;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (Searching()) return;
    ImGui::PushID("layers");
    const bool resetRequested = ImGui::SmallButton(LOC("Reset to Unity Preset..."));
    ImGui::PopID();
    if (resetRequested) ImGui::OpenPopup("##resetLayers");
    if (ConfirmPopup("##resetLayers", LOCT("Replace all layer names with the Unity preset? Scenes keep their layer numbers."), LOC("Reset"))) {
        settings.game.layerNames = {
            "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
            "", "", "", "", "", "", "", "", "", "",
            "", "", "", "", "", "", "", "", "", "",
            "", "", "", "", "", ""
        };
        MarkStructuralEdit();
    }
}


void ProjectSettingsPanel::DrawImport(EditorContext& ctx)
{
    static const FbxImportOptions kDefaults{};
    auto& opt = ctx.defaultImportOptions;

    if (BeginGroup("FBX Defaults")) {
        static const char* const kSourceDcc[]   = { "Auto Detect", "Maya / FBX SDK", "Blender" };
        static const char* const kUpAxis[]      = { "Auto", "Y Up", "Z Up" };
        static const char* const kConvention[]  = { "DirectX (keep G)", "OpenGL (flip G)" };
        static const char* const kCompression[] = { "Auto", "BC1", "BC3", "BC4", "BC5", "BC6H", "BC7", "None" };

        Field("Source DCC", opt.sourceDcc, &kDefaults.sourceDcc, "書き出し元のツール。軸と単位の解釈が変わる",
              [&] { return EnumCombo(opt.sourceDcc, kSourceDcc); });
        Field("Source Up Axis", opt.upAxis, &kDefaults.upAxis, "書き出し元の上方向。Auto はファイルの記録に従う",
              [&] { return EnumCombo(opt.upAxis, kUpAxis); });
        Field("Unit Scale", opt.unitScaleMultiplier, &kDefaults.unitScaleMultiplier, "読み込み時に掛ける倍率",
              [&] { return ImGui::DragFloat("##v", &opt.unitScaleMultiplier, 0.01f, 0.001f, 100.0f, "%.3fx"); });
        Field("Generate Normals", opt.generateNormals, &kDefaults.generateNormals, "法線が無いメッシュに法線を作る",
              [&] { return ImGui::Checkbox("##v", &opt.generateNormals); });
        Field("Generate Tangents", opt.generateTangents, &kDefaults.generateTangents, "法線マップ用の接線を作る",
              [&] { return ImGui::Checkbox("##v", &opt.generateTangents); });
        Field("Normal Map Convention", opt.normalMapConvention, &kDefaults.normalMapConvention,
              "法線マップの緑チャンネルの向き。OpenGL 形式なら反転して取り込む",
              [&] { return EnumCombo(opt.normalMapConvention, kConvention); });
        Field("Texture Sidecars", opt.generateTexDescriptors, &kDefaults.generateTexDescriptors,
              "参照テクスチャの .meta を自動で作る",
              [&] { return ImGui::Checkbox("##v", &opt.generateTexDescriptors); });
        Field("Default Compression", opt.defaultCompression, &kDefaults.defaultCompression,
              "テクスチャの既定の圧縮形式。Auto は用途 (色・法線・マスク) から選ぶ",
              [&] { return EnumCombo(opt.defaultCompression, kCompression); });
    }
    EndGroup();

    if (BeginGroup("Import Presets", true, "preset プリセット") && Matches("Import Presets", "preset プリセット")) {
        PresetCache& cache = Presets();
        const std::string presetsDir = ImportPresetsDir(ctx);
        if (!cache.loaded) {
            cache.presets = LoadImportPresets(presetsDir);
            cache.loaded  = true;
        }
        if (ImGui::SmallButton(LOC("Refresh"))) cache.loaded = false;

        if (cache.presets.empty()) {
            ImGui::TextDisabled("%s", LOCT("No presets in Assets/.import_presets/."));
            ImGui::TextDisabled("%s", LOCT("Create one from the Import Settings dialog (right-click an FBX > Import with Settings...)."));
        } else if (ImGui::BeginTable("##presets", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                                                   | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(LOCT("Name"),     ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(LOCT("Settings"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("",               ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            int deleteIndex = -1;
            for (int i = 0; i < static_cast<int>(cache.presets.size()); ++i) {
                const PresetEntry& p = cache.presets[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(p.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s / %s / x%.2f / %s",
                    p.options.sourceDcc == FbxSourceDcc::Blender ? "Blender" :
                    p.options.sourceDcc == FbxSourceDcc::Maya ? "Maya" : "Auto",
                    p.options.upAxis == FbxUpAxis::ZUp ? "Z Up" :
                    p.options.upAxis == FbxUpAxis::YUp ? "Y Up" : "Auto",
                    p.options.unitScaleMultiplier,
                    p.options.normalMapConvention == NormalMapConvention::OpenGL ? "OpenGL" : "DirectX");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(LOC("Use as Default"))) {
                    opt = p.options;
                    MarkStructuralEdit();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", LOCT("Copy this preset into FBX Defaults above."));
                ImGui::SameLine();
                if (ImGui::SmallButton(LOC("Delete..."))) ImGui::OpenPopup("##deletePreset");
                if (ConfirmPopup("##deletePreset", LOCT("Delete this preset file? This cannot be undone."), LOC("Delete")))
                    deleteIndex = i;
                ImGui::PopID();
            }
            ImGui::EndTable();

            if (deleteIndex >= 0) {
                const std::string path = cache.presets[static_cast<std::size_t>(deleteIndex)].path;
                std::error_code ec;
                if (std::filesystem::remove(util::FileSystem::PathFromUtf8(path), ec)) {
                    FBZZ_LOG_INFO("Deleted import preset: %s", path.c_str());
                    cache.presets.erase(cache.presets.begin() + deleteIndex);
                } else {
                    FBZZ_LOG_ERROR("Failed to delete preset: %s", path.c_str());
                }
            }
        }
    }
    EndGroup();

    if (BeginGroup("Exclude Patterns", false, "exclude suffix backup 除外") && Matches("Exclude Patterns", "exclude suffix backup 除外")) {
        ImGui::TextDisabled("%s", LOCT("Files whose name ends with these suffixes are skipped (built in):"));
        static constexpr const char* kExcluded[] = { "_backup", "_old", "_wip", "_ref", "_tmp", "_test", "_unused", "_bak" };
        std::string joined;
        for (const char* suffix : kExcluded) {
            if (!joined.empty()) joined += "  ";
            joined += suffix;
        }
        ImGui::TextUnformatted(joined.c_str());
    }
    EndGroup();
}


void ProjectSettingsPanel::DrawEditorPreferences(EditorContext& ctx)
{
    const EditorSettings& d = EditorDefaults();

    if (BeginGroup("Language", true, "english japanese 日本語 英語 表示")) {
        loc::Language language = loc::GetLanguage();
        static constexpr loc::Language kDefaultLanguage = loc::Language::English;
        const bool changed = Field("Display Language", language, &kDefaultLanguage,
            "エディター UI の表示言語。選ぶとすぐ切り替わる",
            [&] {
                bool picked = false;
                if (ImGui::BeginCombo("##v", loc::DisplayName(language))) {
                    for (const loc::Language option : loc::kLanguages) {
                        /// @note 候補は常にその言語自身の表記で出す。日本語表示のまま «英語» としか出ないと戻し方が分からない。
                        if (ImGui::Selectable(loc::DisplayName(option), option == language) && option != language) {
                            language = option;
                            picked = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                return picked;
            });
        if (changed) loc::SetLanguage(language);

        /// @note 訳の埋まり具合。無いと «英語のまま» の箇所が訳の欠けか仕組みの不具合か区別できない。
        if (loc::GetLanguage() != loc::Language::English
            && Matches("Translation Coverage", "translation missing untranslated 訳")) {
            char entries[32];
            std::snprintf(entries, sizeof(entries), "%d", loc::TranslationCount());
            InfoRow("Entries", entries);

            const std::vector<std::string>& missing = loc::MissingKeys();
            char missingText[32];
            std::snprintf(missingText, sizeof(missingText), "%zu", missing.size());
            InfoRow("Untranslated (seen this session)", missingText);

            if (!missing.empty() && ImGui::TreeNode(LOC("Untranslated strings"))) {
                ImGui::TextDisabled("%s", LOCT("Open the panels you want translated, then add these to Localization_ja.inl."));
                if (ImGui::SmallButton(LOC("Copy to Clipboard"))) {
                    std::string text;
                    for (const std::string& key : missing) text += "{ \"" + key + "\", \"\" },\n";
                    ImGui::SetClipboardText(text.c_str());
                }
                if (ImGui::BeginChild("##missing", { 0.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f }, true)) {
                    for (const std::string& key : missing) ImGui::TextUnformatted(key.c_str());
                }
                ImGui::EndChild();
                ImGui::TreePop();
            }
        }
    }
    EndGroup();

    if (BeginGroup("Scene Auto Save", true, "autosave backup crash recovery オートセーブ 自動保存 復旧")) {
        Field("Enabled", ctx.sceneAutoSaveEnabled, &d.autoSaveEnabled,
              "未保存の変更があるとき、一定間隔で Library/AutoSave へ退避する。本体のシーンファイルは上書きしない",
              [&] { return ImGui::Checkbox("##v", &ctx.sceneAutoSaveEnabled); });
        int minutes = (std::max)(1, ctx.sceneAutoSaveIntervalSec / 60);
        const int defaultMinutes = (std::max)(1, d.autoSaveIntervalSec / 60);
        if (Field("Interval", minutes, &defaultMinutes,
                  "変更してからこの時間が経つと保存する。直前 10 秒は通知が出て、延期できる",
                  [&] { return ImGui::SliderInt("##v", &minutes, 1, 60, "%d min"); }))
            ctx.sceneAutoSaveIntervalSec = minutes * 60;

        if (ctx.sceneAutoSaveEnabled) {
            char next[48];
            if (ctx.sceneAutoSaveRemainingSec < 0.0f)
                std::snprintf(next, sizeof(next), "%s", LOCT("Not scheduled (no unsaved changes)"));
            else
                std::snprintf(next, sizeof(next), "%d:%02d",
                              static_cast<int>(ctx.sceneAutoSaveRemainingSec) / 60,
                              static_cast<int>(ctx.sceneAutoSaveRemainingSec) % 60);
            InfoRow("Next Auto Save", next);
        }
    }
    EndGroup();

    if (BeginGroup("Hot Reload", true, "script shader dll compile sound スクリプト シェーダー 音")) {
        Field("Watch Scripts & Shaders", ctx.hotReloadEnabled, &d.hotReloadEnabled,
              "Assets のスクリプトとシェーダーの保存を監視し、自動でビルドしてリロードする",
              [&] { return ImGui::Checkbox("##v", &ctx.hotReloadEnabled); });
        Field("Completion Sound", ctx.hotReloadSound, &d.hotReloadSound,
              "ホットリロードの成功・失敗を音で知らせる。エディターを見ていなくても気づける",
              [&] { return ImGui::Checkbox("##v", &ctx.hotReloadSound); });
    }
    EndGroup();
}

} /// @note namespace fbzz::editor
