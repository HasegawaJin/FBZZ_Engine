// FBZZ Engine
// BuildSettingsPanel.cpp | fbzz::editor
// Build Settings パネルの ImGui UI 実装
#include <Editor/Panels/BuildSettingsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/StandaloneLauncher.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <Windows.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <string>

namespace fbzz::editor {

namespace {

// GetModuleFileNameW で自身の exe パスを UTF-8 で返す
std::string GetSelfExePathUtf8()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const int size = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

// フォルダ選択ダイアログ (Win32 SHBrowseForFolder)
bool BrowseForFolder(HWND hwnd, std::string& outPath)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;

    wchar_t path[MAX_PATH]{};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);

    const int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    outPath.assign(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, outPath.data(), size, nullptr, nullptr);
    return true;
}

} // namespace

// =============================================================================
// 初期化
// =============================================================================

void BuildSettingsPanel::OnInit(EditorContext& ctx)
{
    if (!ctx.projectRoot.empty()) {
        m_settings.Load(ctx.projectRoot);
        m_settingsLoaded = true;
    }
}

// =============================================================================
// UI 本体
// =============================================================================

void BuildSettingsPanel::OnRenderContent(EditorContext& ctx)
{
    // プロジェクトが開かれたら設定をロードする (OnInit より後に projectRoot が決まる場合に対応)
    if (!m_settingsLoaded && !ctx.projectRoot.empty()) {
        m_settings.Load(ctx.projectRoot);
        m_settingsLoaded = true;
    }

    DrawScenesInBuild(ctx);
    ImGui::Separator();
    DrawOutputSettings(ctx);
    ImGui::Separator();
    DrawProgressAndActions(ctx);
}

// =============================================================================
// シーンリスト
// =============================================================================

void BuildSettingsPanel::DrawScenesInBuild(EditorContext& ctx)
{
    ImGui::Text("Scenes in Build");
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::BeginChild("##SceneList", { 0.0f, 140.0f }, true);

    for (int i = 0; i < static_cast<int>(m_settings.scenes.size()); ++i) {
        auto& entry = m_settings.scenes[i];

        ImGui::PushID(i);

        // 選択ハイライト
        const bool selected = (m_selectedSceneIdx == i);
        if (ImGui::Selectable("##row", selected,
                              ImGuiSelectableFlags_SpanAllColumns,
                              { 0.0f, ImGui::GetTextLineHeightWithSpacing() }))
            m_selectedSceneIdx = i;

        ImGui::SameLine();
        ImGui::Text("%d", i);
        ImGui::SameLine(40.0f);

        // パスの表示 (スクロールしないよう truncate)
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60.0f);
        ImGui::TextUnformatted(entry.path.c_str());
        ImGui::SameLine();
        ImGui::Checkbox("##en", &entry.enabled);

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    // 現在開いているシーンを追加するボタン
    const bool hasCurrentScene = !ctx.currentScenePath.empty() && !ctx.projectRoot.empty();
    ImGui::BeginDisabled(!hasCurrentScene);
    if (ImGui::Button("+ Add Current Scene")) {
        // currentScenePath (絶対パス) を projectRoot からの相対パスに変換して追加する。
        // WHY: BuildSettings では相対パスで保持することで、プロジェクトを別 PC に移動しても動く。
        namespace fs = std::filesystem;
        const fs::path scene = util::FileSystem::PathFromUtf8(ctx.currentScenePath);
        const fs::path root  = util::FileSystem::PathFromUtf8(ctx.projectRoot);
        // generic_string() で / 区切りに統一 (Windows の \ がシーンパスと混在しないよう)
        const fs::path rel   = util::FileSystem::RelativePath(scene, root);
        const std::string relStr = rel.empty() ? ctx.currentScenePath : rel.generic_string();

        // 重複チェック (同じパスは追加しない)
        const bool alreadyExists = std::any_of(
            m_settings.scenes.begin(), m_settings.scenes.end(),
            [&relStr](const SceneEntry& e) { return e.path == relStr; });

        if (!alreadyExists)
            m_settings.scenes.push_back({ relStr, true });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_selectedSceneIdx < 0);
    if (ImGui::Button("- Remove Selected")) {
        if (m_selectedSceneIdx >= 0 &&
            m_selectedSceneIdx < static_cast<int>(m_settings.scenes.size())) {
            m_settings.scenes.erase(m_settings.scenes.begin() + m_selectedSceneIdx);
            m_selectedSceneIdx = -1;
        }
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled("Scene at index 0 is used as the start scene.");
}

// =============================================================================
// 出力設定
// =============================================================================

void BuildSettingsPanel::DrawOutputSettings(EditorContext& ctx)
{
    ImGui::Text("Platform:  Windows x64");
    ImGui::Spacing();

    // 出力先ディレクトリ
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", m_settings.outputDirectory.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
        if (ImGui::InputText("Output Directory", buf, sizeof(buf)))
            m_settings.outputDirectory = buf;
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            HWND hwnd = nullptr;
            if (!ctx.projectRoot.empty()) {
                // エディタウィンドウのハンドルを GetForegroundWindow で代用する
                hwnd = GetForegroundWindow();
            }
            std::string chosen;
            if (BrowseForFolder(hwnd, chosen))
                m_settings.outputDirectory = chosen;
        }
    }

    // 製品名
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", m_settings.productName.c_str());
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputText("Product Name", buf, sizeof(buf)))
            m_settings.productName = buf;
    }

    // バージョン
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", m_settings.version.c_str());
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputText("Version", buf, sizeof(buf)))
            m_settings.version = buf;
    }

    ImGui::Spacing();
    ImGui::Checkbox("Development Build", &m_settings.developmentBuild);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("If true, writes development = true to game.manifest.toml");
}

// =============================================================================
// 進捗バー + ビルドボタン
// =============================================================================

void BuildSettingsPanel::DrawProgressAndActions(EditorContext& ctx)
{
    const bool isPlaying  = ctx.playMode && ctx.playMode->IsPlaying();
    const bool isBuilding = m_pipeline.GetState() == BuildPipeline::State::Running;

    if (isPlaying)
        ImGui::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, "Cannot build while playing");

#ifndef NDEBUG
    // WHY: Debug ビルドで Build すると debug シンボル入り exe と assimp-vc145-mtd.dll が
    //      パッケージに含まれる。配布には Release ビルドを使うべきである。
    ImGui::TextColored({ 1.0f, 0.6f, 0.1f, 1.0f },
        "[DEBUG BUILD] For distribution, switch to the Release preset.");
    ImGui::Spacing();
#endif

    ImGui::BeginDisabled(isPlaying || isBuilding);

    if (ImGui::Button("Build")) {
        m_settings.Save(ctx.projectRoot);
        m_pipeline.Start(m_settings, ctx.projectRoot, ctx.projectBuildRoot, ctx.standaloneTargetName, ctx.scriptsDllPath, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Build and Run")) {
        m_settings.Save(ctx.projectRoot);
        m_pipeline.Start(m_settings, ctx.projectRoot, ctx.projectBuildRoot, ctx.standaloneTargetName, ctx.scriptsDllPath, true);
    }

    ImGui::EndDisabled();

    // 進捗バーとステータス
    if (isBuilding) {
        ImGui::ProgressBar(m_pipeline.GetProgress(), { -1.0f, 0.0f }, m_pipeline.GetStatus());
        const std::string& buildLog = m_pipeline.GetBuildLog();
        if (!buildLog.empty()) {
            // WHY: CMake / MSBuild の失敗理由は標準出力に出るため、エディタ内で読めるようにする。
            ImGui::BeginChild("##RuntimeBuildLog", { 0.0f, 180.0f }, true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(buildLog.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
        if (ImGui::Button("Cancel")) {
            m_pipeline.Cancel();
        }
        m_pipeline.Tick();
    }

    // Build and Run: ビルド完了後に exe を起動する
    if (m_pipeline.GetState() == BuildPipeline::State::Done && m_pipeline.WantsRunAfter()) {
        StandaloneLauncher::LaunchExe(m_pipeline.GetOutputExePath(), "");
        // 重複起動を防ぐためリセットする
        m_pipeline.Reset();
    }

    // エラー表示
    if (m_pipeline.GetState() == BuildPipeline::State::Failed) {
        ImGui::TextColored({ 1.0f, 0.3f, 0.3f, 1.0f }, "Error: %s", m_pipeline.GetError());
        if (ImGui::Button("Reset")) m_pipeline.Reset();
    }

    // 完了メッセージ
    if (m_pipeline.GetState() == BuildPipeline::State::Done && !m_pipeline.WantsRunAfter()) {
        ImGui::TextColored({ 0.4f, 1.0f, 0.4f, 1.0f }, "Build complete");
        if (ImGui::Button("Reset##done")) m_pipeline.Reset();
    }
}

} // namespace fbzz::editor
