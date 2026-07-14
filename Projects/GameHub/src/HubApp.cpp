// FBZZ Engine
// HubApp.cpp | fbzz::hub
// Hub ImGui UI implementation
#include "HubApp.hpp"
#include "MigrationManager.hpp"
#include "ProcessLauncher.hpp"
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <Windows.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

constexpr float SIDEBAR_WIDTH = 196.0f;
constexpr float COMPACT_SIDEBAR_WIDTH = 164.0f;
constexpr float CARD_HEIGHT = 146.0f;
constexpr float THUMBNAIL_SIZE = 88.0f;
const ImVec4 ACCENT_COLOR = ImVec4(0.31f, 0.62f, 1.0f, 1.0f);
const ImVec4 SUCCESS_COLOR = ImVec4(0.34f, 0.78f, 0.55f, 1.0f);
const ImVec4 WARNING_COLOR = ImVec4(1.0f, 0.68f, 0.25f, 1.0f);
const ImVec4 ERROR_COLOR = ImVec4(1.0f, 0.36f, 0.36f, 1.0f);

// Hub 全体のテーマを一箇所で構築し、画面ごとの PushStyleColor の重複を避ける。
void ApplyHubTheme(std::string_view theme)
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = { 18.0f, 18.0f };
    style.FramePadding = { 12.0f, 7.0f };
    style.CellPadding = { 10.0f, 7.0f };
    style.ItemSpacing = { 10.0f, 9.0f };
    style.ItemInnerSpacing = { 8.0f, 6.0f };
    style.WindowRounding = 0.0f;
    style.ChildRounding = 9.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 9.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;

    const bool midnight = theme == "midnight";
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.91f, 0.93f, 0.97f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.53f, 0.62f, 1.0f);
    colors[ImGuiCol_WindowBg] = midnight
        ? ImVec4(0.035f, 0.047f, 0.075f, 1.0f)
        : ImVec4(0.070f, 0.076f, 0.090f, 1.0f);
    colors[ImGuiCol_ChildBg] = midnight
        ? ImVec4(0.055f, 0.071f, 0.108f, 1.0f)
        : ImVec4(0.095f, 0.102f, 0.120f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.086f, 0.120f, 0.99f);
    colors[ImGuiCol_Border] = ImVec4(0.18f, 0.22f, 0.30f, 0.72f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.125f, 0.17f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.18f, 0.25f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.22f, 0.31f, 1.0f);
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_Button] = ImVec4(0.13f, 0.19f, 0.28f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.34f, 0.52f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.23f, 0.45f, 0.70f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.12f, 0.18f, 0.27f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.29f, 0.44f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.40f, 0.62f, 1.0f);
    colors[ImGuiCol_CheckMark] = ACCENT_COLOR;
    colors[ImGuiCol_SliderGrab] = ACCENT_COLOR;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.72f, 1.0f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.17f, 0.21f, 0.28f, 0.8f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.31f, 0.62f, 1.0f, 0.18f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.31f, 0.62f, 1.0f, 0.55f);
    colors[ImGuiCol_ResizeGripActive] = ACCENT_COLOR;
    colors[ImGuiCol_NavHighlight] = ACCENT_COLOR;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.015f, 0.025f, 0.76f);
}

// ASCII を中心としたプロジェクト名とパスの検索・並び替え用に小文字コピーを作る。
std::string LowerCopy(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

// 長いパスを操作ボタンへ重ねず、省略記号付きでカード内の利用可能幅へ収める。
std::string EllipsizeText(std::string_view value, float maxWidth)
{
    constexpr std::string_view ellipsis = "...";
    if (maxWidth <= ImGui::CalcTextSize(ellipsis.data()).x) return std::string(ellipsis);
    if (ImGui::CalcTextSize(value.data(), value.data() + value.size()).x <= maxWidth)
        return std::string(value);

    std::string result(value);
    while (!result.empty()) {
        size_t eraseAt = result.size() - 1;
        while (eraseAt > 0 &&
               (static_cast<unsigned char>(result[eraseAt]) & 0xC0u) == 0x80u)
            --eraseAt;
        result.erase(eraseAt);

        const std::string candidate = result + ellipsis.data();
        if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth) return candidate;
    }
    return std::string(ellipsis);
}

// カード内の状態を色と文言の両方で示し、色覚だけに依存しないフィードバックを提供する。
void DrawStatusPill(const char* label, const ImVec4& color)
{
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(textSize.x + 18.0f, textSize.y + 8.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y },
        ImGui::ColorConvertFloat4ToU32({ color.x, color.y, color.z, 0.15f }), 12.0f);
    drawList->AddRect(pos, { pos.x + size.x, pos.y + size.y },
        ImGui::ColorConvertFloat4ToU32({ color.x, color.y, color.z, 0.55f }), 12.0f);
    drawList->AddText({ pos.x + 9.0f, pos.y + 4.0f },
        ImGui::ColorConvertFloat4ToU32(color), label);
    ImGui::Dummy(size);
}

// 補足文を無効色で折り返し、狭いウィンドウでも右端へはみ出さないよう描画する。
void DrawMutedWrappedText(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

int ParseVersionMajor(const std::string& version)
{
    int value = 0;
    for (char ch : version) {
        if (ch < '0' || ch > '9') {
            break;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

std::filesystem::path Utf8ToPath(const std::string& text)
{
    return engine_util::FileSystem::PathFromUtf8(text);
}

std::string ToStoredPath(const std::filesystem::path& path)
{
    return engine_util::FileSystem::PathToUtf8(engine_util::FileSystem::MakeAbsolute(path));
}

bool SelectFolder(const wchar_t* title, std::string& outPath)
{
    BROWSEINFOW browseInfo{};
    browseInfo.lpszTitle = title;
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidList = SHBrowseForFolderW(&browseInfo);
    if (!pidList) {
        return false;
    }

    wchar_t path[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(pidList, path);
    CoTaskMemFree(pidList);
    if (!ok) {
        return false;
    }

    outPath = engine_util::StringUtils::ToNarrow(path);
    return true;
}

bool IsSdkRoot(const std::filesystem::path& path)
{
    return engine_util::FileSystem::Exists(path / "fbzz-sdk.toml")
        && engine_util::FileSystem::Exists(path / "cmake" / "FBZZ" / "FBZZConfig.cmake")
        && engine_util::FileSystem::Exists(path / "include" / "Engine");
}

std::string ResolveSdkRootForTemplate(const HubConfig& config)
{
    if (!config.GetSdkRoot().empty()) {
        const std::filesystem::path configured = engine_util::FileSystem::PathFromUtf8(config.GetSdkRoot());
        if (IsSdkRoot(configured))
            return engine_util::FileSystem::PathToUtf8(configured);
        // 旧 engine_root がソースルートを指す設定は、同 checkout の版別 SDK へ移行する。
        const std::filesystem::path versioned = configured / "SDK" / FBZZ_VERSION;
        if (IsSdkRoot(versioned))
            return engine_util::FileSystem::PathToUtf8(versioned);
    }

    std::filesystem::path current = engine_util::FileSystem::GetCurrentDirectory();
    if (IsSdkRoot(current)) {
        return engine_util::FileSystem::PathToUtf8(current);
    }
    if (IsSdkRoot(current / "SDK" / FBZZ_VERSION))
        return engine_util::FileSystem::PathToUtf8(current / "SDK" / FBZZ_VERSION);

    current = engine_util::FileSystem::GetExecutableDirectory();
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        if (IsSdkRoot(current)) {
            return engine_util::FileSystem::PathToUtf8(current);
        }
        if (IsSdkRoot(current / "SDK" / FBZZ_VERSION))
            return engine_util::FileSystem::PathToUtf8(current / "SDK" / FBZZ_VERSION);
        current = current.parent_path();
    }

    return {};
}

void ApplySdkEnvironment(const std::string& sdkRoot)
{
    if (sdkRoot.empty()) {
        return;
    }

    const std::wstring rootW = engine_util::StringUtils::ToWide(sdkRoot);
    SetEnvironmentVariableW(L"FBZZ_SDK_ROOT", rootW.c_str());

    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegSetValueExW(
            key,
            L"FBZZ_SDK_ROOT",
            0,
            REG_EXPAND_SZ,
            reinterpret_cast<const BYTE*>(rootW.c_str()),
            static_cast<DWORD>((rootW.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        SendMessageTimeoutW(
            HWND_BROADCAST,
            WM_SETTINGCHANGE,
            0,
            reinterpret_cast<LPARAM>(L"Environment"),
            SMTO_ABORTIFHUNG,
            100,
            nullptr);
    }
}

} // namespace

bool HubApp::Init(fbzz::renderer::ResourceManager& resources, fbzz::renderer::IImGuiRenderer& imgui)
{
    m_thumbnailCache.Init(resources, imgui);
    m_config.Load();
    SyncSettingsBuffers();
    ApplyHubTheme(m_config.GetTheme());
    ApplySdkEnvironment(ResolveSdkRootForTemplate(m_config));
    m_templateManager.Refresh();
    m_projectManager.LoadFromConfig(m_config);
    return true;
}

// 新規作成を毎回初期状態で開き、古い入力やテンプレート選択の持ち越しを防ぐ。
void HubApp::PrepareNewProjectDialog()
{
    m_templateManager.Refresh();
    m_newProjectNameBuffer.fill('\0');
    m_newProjectDestinationBuffer.fill('\0');
    m_selectedTemplateIndex = 0;
    m_showNewProjectDialog = true;
}

// 永続化済み設定を編集用バッファへ複製し、Reload 時にも UI と設定値を一致させる。
void HubApp::SyncSettingsBuffers()
{
    m_editorExeBuffer.fill('\0');
    m_engineRootBuffer.fill('\0');
    strncpy_s(m_editorExeBuffer.data(), m_editorExeBuffer.size(),
              m_config.GetEditorExe().c_str(), _TRUNCATE);
    strncpy_s(m_engineRootBuffer.data(), m_engineRootBuffer.size(),
        m_config.GetSdkRoot().c_str(), _TRUNCATE);
    m_selectedThemeIndex = m_config.GetTheme() == "midnight" ? 0 : 1;
}

void HubApp::Shutdown()
{
    m_config.Save();
    m_thumbnailCache.Clear();
}

void HubApp::Render()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("FBZZ Hub", nullptr, flags);
    ImGui::PopStyleVar();

    RenderSidebar();
    ImGui::SameLine();

    ImGui::BeginChild("MainPanel", ImVec2(0, 0), false);
    if (m_activePanel == Panel::Projects) RenderProjectsPanel();
    if (m_activePanel == Panel::Learn) RenderLearnPanel();
    if (m_activePanel == Panel::Settings) RenderSettingsPanel();
    ImGui::EndChild();

    ImGui::End();

    RenderNewProjectDialog();
    RenderMigrationDialog();
    RenderErrorModal();
}

void HubApp::RenderSidebar()
{
    const float sidebarWidth = ImGui::GetMainViewport()->WorkSize.x < 820.0f
        ? COMPACT_SIDEBAR_WIDTH : SIDEBAR_WIDTH;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.050f, 0.080f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 18.0f));
    ImGui::BeginChild("Sidebar", ImVec2(sidebarWidth, 0), false);

    const ImVec2 logoPos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        logoPos, { logoPos.x + 38.0f, logoPos.y + 38.0f },
        ImGui::ColorConvertFloat4ToU32(ACCENT_COLOR), 9.0f);
    ImGui::GetWindowDrawList()->AddText(
        { logoPos.x + 9.0f, logoPos.y + 10.0f }, IM_COL32_WHITE, "FZ");
    ImGui::Dummy({ 46.0f, 38.0f });
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("FBZZ Hub");
    ImGui::TextDisabled("Game workspace");
    ImGui::EndGroup();

    ImGui::Dummy({ 0.0f, 24.0f });
    ImGui::TextDisabled("WORKSPACE");
    ImGui::Dummy({ 0.0f, 4.0f });

    if (ImGui::Selectable("  Projects", m_activePanel == Panel::Projects, 0, { 0.0f, 42.0f })) {
        m_activePanel = Panel::Projects;
    }
    if (ImGui::Selectable("  Learn", m_activePanel == Panel::Learn, 0, { 0.0f, 42.0f })) {
        m_activePanel = Panel::Learn;
    }
    if (ImGui::Selectable("  Settings", m_activePanel == Panel::Settings, 0, { 0.0f, 42.0f })) {
        m_activePanel = Panel::Settings;
    }

    ImGui::SetCursorPosY((std::max)(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 72.0f));
    ImGui::Separator();
    ImGui::TextDisabled("ENGINE");
    ImGui::Text("FBZZ %s", FBZZ_VERSION);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void HubApp::RenderProjectsPanel()
{
    if (m_reloadProjectsAfterRender) {
        m_projectManager.LoadFromConfig(m_config);
        m_reloadProjectsAfterRender = false;
    }

    const auto& projects = m_projectManager.GetProjects();
    std::vector<const ProjectEntry*> visibleProjects;
    visibleProjects.reserve(projects.size());
    for (const auto& project : projects)
        if (MatchesSearch(project)) visibleProjects.push_back(&project);

    const auto compareText = [](std::string_view left, std::string_view right) {
        const std::string lowerLeft = LowerCopy(left);
        const std::string lowerRight = LowerCopy(right);
        return lowerLeft < lowerRight ? -1 : (lowerLeft > lowerRight ? 1 : 0);
    };
    std::stable_sort(visibleProjects.begin(), visibleProjects.end(), [&](const ProjectEntry* left,
                                                                         const ProjectEntry* right) {
        int comparison = 0;
        if (m_projectSort == ProjectSort::Name)
            comparison = compareText(left->name, right->name);
        else if (m_projectSort == ProjectSort::EngineVersion)
            comparison = compareText(left->engineVersion, right->engineVersion);
        else
            comparison = left->lastOpened < right->lastOpened ? -1
                       : (left->lastOpened > right->lastOpened ? 1 : 0);
        return m_sortAscending ? comparison < 0 : comparison > 0;
    });

    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const bool stackFilters = panelWidth < 480.0f;
    const bool stackActions = panelWidth < 330.0f;
    ImGui::BeginChild("Toolbar", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
    ImGui::TextColored(ACCENT_COLOR, "PROJECTS");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu registered / %zu shown", projects.size(), visibleProjects.size());
    DrawMutedWrappedText("Create, validate, and launch FBZZ projects from one workspace.");

    if (ImGui::Button("New Project", { 118.0f, 0.0f })) PrepareNewProjectDialog();
    if (!stackActions) ImGui::SameLine();
    if (ImGui::Button("Add Existing", { 118.0f, 0.0f })) AddExistingProject();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        m_config.Load();
        SyncSettingsBuffers();
        ApplyHubTheme(m_config.GetTheme());
        m_templateManager.Refresh();
        m_projectManager.LoadFromConfig(m_config);
    }

    const float filterWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(stackFilters ? -1.0f : filterWidth * 0.46f);
    ImGui::InputTextWithHint("##ProjectSearch", "Search projects or paths...",
                             m_searchBuffer.data(), m_searchBuffer.size());
    if (!stackFilters) ImGui::SameLine();
    const char* sortLabels[] = { "Last opened", "Name", "Engine version" };
    int sortIndex = static_cast<int>(m_projectSort);
    ImGui::SetNextItemWidth((std::max)(100.0f, ImGui::GetContentRegionAvail().x - 106.0f));
    if (ImGui::Combo("##ProjectSort", &sortIndex, sortLabels, 3))
        m_projectSort = static_cast<ProjectSort>(sortIndex);
    ImGui::SameLine();
    if (ImGui::Button(m_sortAscending ? "Ascending" : "Descending", { 96.0f, 0.0f }))
        m_sortAscending = !m_sortAscending;
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("ProjectsList", ImVec2(0, 0), false);
    if (projects.empty()) {
        ImGui::Dummy({ 0.0f, 70.0f });
        const char* title = "No projects yet";
        const char* detail = "Create a project from a template or register an existing FBZZ project.";
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
            (ImGui::GetWindowWidth() - ImGui::CalcTextSize(title).x) * 0.5f));
        ImGui::TextUnformatted(title);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("%s", detail);
        ImGui::PopTextWrapPos();
        ImGui::Dummy({ 0.0f, 10.0f });
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
            (ImGui::GetWindowWidth() - 250.0f) * 0.5f));
        if (ImGui::Button("Create New Project", { 150.0f, 38.0f })) PrepareNewProjectDialog();
        if (ImGui::GetContentRegionAvail().x >= 110.0f) ImGui::SameLine();
        if (ImGui::Button("Add Existing", { 100.0f, 38.0f })) AddExistingProject();
    } else if (visibleProjects.empty()) {
        ImGui::Dummy({ 0.0f, 70.0f });
        const char* noMatch = "No projects match your search.";
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
            (ImGui::GetWindowWidth() - ImGui::CalcTextSize(noMatch).x) * 0.5f));
        ImGui::TextDisabled("%s", noMatch);
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
            (ImGui::GetWindowWidth() - 112.0f) * 0.5f));
        if (ImGui::Button("Clear Search", { 112.0f, 0.0f })) m_searchBuffer.fill('\0');
    } else {
        for (const ProjectEntry* project : visibleProjects) {
            RenderProjectCard(*project);
        }
    }
    ImGui::EndChild();

    if (!m_pendingRemoveProject.empty()) {
        if (m_config.RemoveProject(m_pendingRemoveProject)) {
            m_config.Save();
        }
        m_pendingRemoveProject.clear();
        m_reloadProjectsAfterRender = true;
    }

}

void HubApp::RenderProjectCard(const ProjectEntry& project)
{
    ImGui::PushID(project.path.c_str());

    const char* statusLabel = "Ready";
    ImVec4 statusColor = SUCCESS_COLOR;
    if (!project.pathExists) {
        statusLabel = "Path missing";
        statusColor = ERROR_COLOR;
    } else if (project.path == m_failedMigrationProject) {
        statusLabel = "Migration failed";
        statusColor = ERROR_COLOR;
    } else if (!project.projFileValid || !project.layoutValid || !project.cmakeExists ||
               !project.apiHeaderExists || !project.settingsExists) {
        statusLabel = "Layout warning";
        statusColor = WARNING_COLOR;
    } else if (project.migrationRequired) {
        statusLabel = "Migration required";
        statusColor = WARNING_COLOR;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.095f, 0.135f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
    ImGui::BeginChild("Card", ImVec2(0, CARD_HEIGHT), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const ImVec2 cardMin = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        cardMin, { cardMin.x + 4.0f, cardMin.y + ImGui::GetWindowHeight() },
        ImGui::ColorConvertFloat4ToU32(statusColor), 8.0f,
        ImDrawFlags_RoundCornersLeft);

    const float cardContentWidth = ImGui::GetContentRegionAvail().x;
    const bool showThumbnail = cardContentWidth >= 500.0f;
    if (showThumbnail) {
        RenderProjectThumbnail(project);
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    const float metadataWidth = (std::max)(80.0f,
        cardContentWidth - (showThumbnail ? THUMBNAIL_SIZE + ImGui::GetStyle().ItemSpacing.x : 0.0f) - 112.0f);
    const std::string displayName = EllipsizeText(project.name, metadataWidth);
    const std::string displayPath = EllipsizeText(project.path, metadataWidth);
    ImGui::TextColored(ImVec4(0.95f, 0.97f, 1.0f, 1.0f), "%s", displayName.c_str());
    ImGui::TextDisabled("%s", displayPath.c_str());
    if (ImGui::IsItemHovered() && displayPath != project.path)
        ImGui::SetTooltip("%s", project.path.c_str());
    ImGui::TextDisabled("Last opened  %s", project.lastOpened.empty() ? "Never" : project.lastOpened.c_str());
    ImGui::TextDisabled("Engine  %s",
        project.engineVersion.empty() ? "Unknown" : project.engineVersion.c_str());
    DrawStatusPill(statusLabel, statusColor);
    ImGui::EndGroup();

    ImGui::SetCursorPos({ ImGui::GetWindowWidth() - 112.0f, 20.0f });
    ImGui::BeginDisabled(!project.pathExists);
    if (ImGui::Button("Open", { 88.0f, 34.0f })) OpenProject(project);
    ImGui::EndDisabled();
    ImGui::SetCursorPos({ ImGui::GetWindowWidth() - 112.0f, 63.0f });
    if (ImGui::Button("Actions", { 88.0f, 30.0f })) ImGui::OpenPopup("ProjectMenu");

    if (project.migrationRequired) {
        ImGui::SetCursorPos({ ImGui::GetWindowWidth() - 112.0f, 101.0f });
        if (ImGui::SmallButton("Migrate now")) RequestMigration(project);
    }

    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("ProjectMenu");
    if (ImGui::BeginPopup("ProjectMenu")) {
        ImGui::TextDisabled("PROJECT ACTIONS");
        ImGui::Separator();
        if (ImGui::MenuItem("Open in Editor", nullptr, false, project.pathExists)) OpenProject(project);
        if (ImGui::MenuItem("Reveal in Explorer", nullptr, false, project.pathExists)) RevealProject(project);
        if (ImGui::MenuItem("Migrate Project", nullptr, false,
                            project.pathExists && project.migrationRequired)) RequestMigration(project);
        ImGui::Separator();
        if (ImGui::MenuItem("Remove from Hub")) RemoveProject(project);
        ImGui::EndPopup();
    }

    const bool cardHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool openByDoubleClick = cardHovered && !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && project.pathExists;

    if (!project.pathExists && cardHovered) {
        ImGui::SetTooltip("Project path was not found: %s", project.path.c_str());
    }

    ImGui::EndChild();
    if (openByDoubleClick) OpenProject(project);
    ImGui::Spacing();
    ImGui::PopID();
}


void HubApp::RenderProjectThumbnail(const ProjectEntry& project)
{
    const ImVec2 size(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
    const ThumbnailTexture* texture = project.thumbnailExists
        ? m_thumbnailCache.GetOrLoad(project.thumbnailPath)
        : nullptr;

    if (texture && texture->imTextureId) {
        ImGui::Image(
            texture->imTextureId,
            size,
            ImVec2(0.0f, 0.0f),
            ImVec2(1.0f, 1.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", project.thumbnailPath.c_str());
        }
        return;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const ImU32 fillColor = project.thumbnailExists
        ? IM_COL32(42, 68, 92, 255)
        : IM_COL32(42, 42, 46, 255);
    const ImU32 borderColor = project.thumbnailExists
        ? IM_COL32(86, 150, 210, 255)
        : IM_COL32(92, 92, 96, 255);
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), fillColor, 4.0f);
    drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderColor, 4.0f);

    const char* label = project.thumbnailExists ? "Load Failed" : "No Image";
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    drawList->AddText(
        ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + (size.y - textSize.y) * 0.5f),
        IM_COL32(230, 232, 236, 255),
        label);

    ImGui::Dummy(size);
    if (project.thumbnailExists && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", project.thumbnailPath.c_str());
    }
}

void HubApp::RenderNewProjectDialog()
{
    if (m_showNewProjectDialog) {
        ImGui::OpenPopup("New Project");
        m_showNewProjectDialog = false;
    }

    const ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;
    const ImVec2 dialogSize(
        (std::max)(420.0f, (std::min)(760.0f, workSize.x - 32.0f)),
        (std::max)(460.0f, (std::min)(600.0f, workSize.y - 32.0f)));
    ImGui::SetNextWindowSize(dialogSize, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_NoCollapse)) {
        return;
    }

    const auto& templates = m_templateManager.GetTemplates();
    if (!templates.empty() &&
        (m_selectedTemplateIndex < 0 || m_selectedTemplateIndex >= static_cast<int>(templates.size())))
        m_selectedTemplateIndex = 0;

    const ProjectNameInfo nameInfo = TemplateManager::MakeProjectNameInfo(m_newProjectNameBuffer.data());
    const bool validName = TemplateManager::IsValidProjectNameInfo(nameInfo);
    const bool hasDestination = m_newProjectDestinationBuffer[0] != '\0';
    const std::filesystem::path targetPath = hasDestination
        ? Utf8ToPath(m_newProjectDestinationBuffer.data()) / nameInfo.targetName
        : std::filesystem::path{};
    const bool targetExists = hasDestination && !nameInfo.targetName.empty() &&
        engine_util::FileSystem::Exists(targetPath);
    const bool canCreate = !templates.empty() && validName && hasDestination && !targetExists;

    ImGui::TextColored(ACCENT_COLOR, "CREATE PROJECT");
    DrawMutedWrappedText("Choose a starting point, then configure the project identity and location.");
    ImGui::Separator();

    const ImVec2 bodyAvailable = ImGui::GetContentRegionAvail();
    const bool useColumns = bodyAvailable.x >= 620.0f;
    const float bodyHeight = (std::max)(250.0f, bodyAvailable.y - 58.0f);
    const float templateWidth = useColumns ? (std::min)(230.0f, bodyAvailable.x * 0.34f) : 0.0f;
    const float templateHeight = useColumns ? bodyHeight : (std::min)(150.0f, bodyHeight * 0.38f);

    ImGui::BeginChild("TemplateBrowser", { templateWidth, templateHeight }, true);
    ImGui::TextDisabled("TEMPLATE");
    ImGui::Dummy({ 0.0f, 4.0f });
    if (templates.empty()) {
        ImGui::TextWrapped("No templates were found. Check the GameHub Templates directory.");
    } else {
        for (int i = 0; i < static_cast<int>(templates.size()); ++i) {
            const auto& item = templates[static_cast<size_t>(i)];
            ImGui::PushID(i);
            if (ImGui::Selectable(item.displayName.c_str(), i == m_selectedTemplateIndex,
                                  0, { 0.0f, 46.0f }))
                m_selectedTemplateIndex = i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.description.c_str());
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextWrapped("%s",
            templates[static_cast<size_t>(m_selectedTemplateIndex)].description.c_str());
    }
    ImGui::EndChild();

    if (useColumns) ImGui::SameLine();
    const float setupHeight = useColumns
        ? bodyHeight
        : (std::max)(180.0f, bodyHeight - templateHeight - ImGui::GetStyle().ItemSpacing.y);
    ImGui::BeginChild("ProjectSetup", { 0.0f, setupHeight }, true);
    ImGui::TextDisabled("PROJECT DETAILS");
    ImGui::Dummy({ 0.0f, 5.0f });
    ImGui::TextUnformatted("Project name");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ProjectName", "Example: My Adventure",
                             m_newProjectNameBuffer.data(), m_newProjectNameBuffer.size());
    ImGui::TextDisabled("ASCII letters, numbers, spaces, '-' and '_' are supported.");

    ImGui::Dummy({ 0.0f, 8.0f });
    ImGui::TextUnformatted("Parent folder");
    ImGui::SetNextItemWidth(-88.0f);
    ImGui::InputTextWithHint("##ProjectLocation", "Choose where the project folder will be created",
                             m_newProjectDestinationBuffer.data(), m_newProjectDestinationBuffer.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse", { 78.0f, 0.0f })) {
        std::string selectedPath;
        if (SelectFolder(L"Select project parent folder", selectedPath))
            strncpy_s(m_newProjectDestinationBuffer.data(), m_newProjectDestinationBuffer.size(),
                      selectedPath.c_str(), _TRUNCATE);
    }

    ImGui::Dummy({ 0.0f, 10.0f });
    ImGui::TextDisabled("GENERATED IDENTITY");
    ImGui::Text("Target      %s", nameInfo.targetName.empty() ? "-" : nameInfo.targetName.c_str());
    ImGui::Text("Namespace   %s", nameInfo.cppNamespace.empty() ? "-" : nameInfo.cppNamespace.c_str());
    ImGui::Text("Project ID  %s", nameInfo.projectId.empty() ? "-" : nameInfo.projectId.c_str());

    ImGui::Dummy({ 0.0f, 8.0f });
    if (!validName) {
        DrawStatusPill("Enter a valid project name", WARNING_COLOR);
    } else if (!hasDestination) {
        DrawStatusPill("Choose a destination folder", WARNING_COLOR);
    } else if (targetExists) {
        DrawStatusPill("A folder with this target name already exists", ERROR_COLOR);
    } else {
        DrawStatusPill("Ready to create", SUCCESS_COLOR);
        ImGui::TextWrapped("%s", engine_util::FileSystem::PathToUtf8(targetPath).c_str());
    }
    ImGui::EndChild();

    ImGui::Separator();
    const float footerStart = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 220.0f;
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), footerStart));

    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button("Create Project", ImVec2(124.0f, 36.0f))) {
        std::string error;
        const auto& selectedTemplate = templates[static_cast<size_t>(m_selectedTemplateIndex)];
        const std::string createdAt = CurrentTimestamp();
        if (m_templateManager.Instantiate(
                selectedTemplate,
                Utf8ToPath(m_newProjectDestinationBuffer.data()),
                nameInfo,
                createdAt,
        ResolveSdkRootForTemplate(m_config),
                error)) {
            const std::string projectPath = ToStoredPath(Utf8ToPath(m_newProjectDestinationBuffer.data()) / nameInfo.targetName);
            m_config.AddProject(projectPath, createdAt);
            m_config.Save();
            m_reloadProjectsAfterRender = true;
            ImGui::CloseCurrentPopup();
        } else {
            m_errorMessage = error;
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(86.0f, 36.0f))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void HubApp::RenderMigrationDialog()
{
    if (m_showMigrationDialog) {
        ImGui::OpenPopup("Project Migration");
        m_showMigrationDialog = false;
    }

    if (!ImGui::BeginPopupModal("Project Migration", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextUnformatted(m_pendingMigrationName.c_str());
    ImGui::Separator();
    if (m_pendingMigrationIsMajor) {
        ImGui::TextWrapped("This project has a major engine version difference. A backup will be created before generated project files are updated.");
    } else {
        ImGui::TextWrapped("A backup will be created before generated project files are updated.");
    }
    ImGui::TextWrapped("Assets and Src/Scripts are not modified by this migration.");

    if (ImGui::Button("Migrate", ImVec2(96.0f, 0.0f))) {
        std::string error;
        if (MigrationManager::MigrateProject(m_pendingMigrationProject, error)) {
            m_reloadProjectsAfterRender = true;
            m_failedMigrationProject.clear();
            m_pendingMigrationProject.clear();
            m_pendingMigrationName.clear();
            ImGui::CloseCurrentPopup();
        } else {
            m_failedMigrationProject = m_pendingMigrationProject;
            m_errorMessage = error;
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(96.0f, 0.0f))) {
        m_pendingMigrationProject.clear();
        m_pendingMigrationName.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void HubApp::RenderLearnPanel()
{
    ImGui::TextColored(ACCENT_COLOR, "LEARN");
    ImGui::TextDisabled("Documentation and integration references for FBZZ Engine projects.");
    ImGui::Separator();

    ImGui::BeginChild("LearnDesign", { 0.0f, 0.0f },
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextUnformatted("Engine design");
    DrawMutedWrappedText("Architecture, conventions, and system-level design notes.");
    if (ImGui::Button("Open Design Docs")) {
        ShellExecuteW(nullptr, L"open", L"Docs\\design", nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::GetContentRegionAvail().x >= 120.0f) ImGui::SameLine();
    if (ImGui::Button("Open UI Design")) {
        ShellExecuteW(nullptr, L"open", L"Docs\\design\\editor-ui-refactor.md", nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::EndChild();

    ImGui::BeginChild("LearnRepository", { 0.0f, 0.0f },
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextUnformatted("Source repository");
    DrawMutedWrappedText("Review source history and collaborate on the engine.");
    if (ImGui::Button("Repository")) {
        ShellExecuteW(nullptr, L"open", L"https://github.com/HasegawaJin/FBZZ_Engine", nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextDisabled("PROJECT CONTRACTS");
    ImGui::BulletText("Project metadata: .fbzz_proj");
    ImGui::BulletText("Generated public API: Include/<ProjectName>/ProjectAPI.hpp");
    ImGui::BulletText("Launch contract: FBZZEditor.exe --project <path>");
}

void HubApp::RenderSettingsPanel()
{
    ImGui::TextColored(ACCENT_COLOR, "SETTINGS");
    DrawMutedWrappedText("Configure how GameHub locates the engine and launches the Editor.");
    ImGui::Separator();

    ImGui::BeginChild("LaunchSettings", { 0.0f, 0.0f },
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextUnformatted("Launch and engine paths");
    DrawMutedWrappedText("Leave a field empty to use automatic discovery relative to GameHub.");
    ImGui::Dummy({ 0.0f, 8.0f });

    const bool stackPathControls = ImGui::GetContentRegionAvail().x < 460.0f;

    ImGui::TextUnformatted("Editor executable");
    ImGui::SetNextItemWidth(stackPathControls ? -1.0f : -92.0f);
    ImGui::InputTextWithHint("##EditorExe", "Automatic (same directory)",
                             m_editorExeBuffer.data(), m_editorExeBuffer.size());
    if (!stackPathControls) ImGui::SameLine();
    if (ImGui::Button("Auto##Editor", { 82.0f, 0.0f })) m_editorExeBuffer.fill('\0');

    ImGui::TextUnformatted("SDK root");
    ImGui::SetNextItemWidth(stackPathControls ? -1.0f : -178.0f);
    ImGui::InputTextWithHint("##EngineRoot", "Automatic discovery",
                             m_engineRootBuffer.data(), m_engineRootBuffer.size());
    if (!stackPathControls) ImGui::SameLine();
    if (ImGui::Button("Browse##Engine", { 82.0f, 0.0f })) {
        std::string selectedPath;
        if (SelectFolder(L"Select versioned FBZZ SDK root", selectedPath))
            strncpy_s(m_engineRootBuffer.data(), m_engineRootBuffer.size(),
                      selectedPath.c_str(), _TRUNCATE);
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto##Engine", { 82.0f, 0.0f })) m_engineRootBuffer.fill('\0');

    ImGui::Dummy({ 0.0f, 7.0f });
    const bool validEngineRoot = m_engineRootBuffer[0] == '\0' ||
        IsSdkRoot(Utf8ToPath(m_engineRootBuffer.data()));
    const char* engineStatus = m_engineRootBuffer[0] == '\0'
        ? "Automatic engine discovery is enabled"
        : (validEngineRoot ? "SDK path is valid" : "Selected folder is not a versioned FBZZ SDK root");
    DrawStatusPill(engineStatus,
                   validEngineRoot ? SUCCESS_COLOR : ERROR_COLOR);
    ImGui::EndChild();

    ImGui::BeginChild("AppearanceSettings", { 0.0f, 0.0f },
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::TextUnformatted("Appearance");
    DrawMutedWrappedText("Choose a low-contrast graphite surface or a deeper blue workspace.");
    const char* themes[] = { "Midnight", "Graphite" };
    ImGui::SetNextItemWidth((std::min)(220.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("Theme", &m_selectedThemeIndex, themes, 2)) {
        const char* theme = m_selectedThemeIndex == 0 ? "midnight" : "dark";
        ApplyHubTheme(theme);
    }
    ImGui::EndChild();

    const std::string configPath = engine_util::FileSystem::PathToUtf8(m_config.GetConfigPath());
    ImGui::TextDisabled("Config: %s",
        EllipsizeText(configPath, ImGui::GetContentRegionAvail().x).c_str());
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 218.0f));
    if (ImGui::Button("Reload", { 82.0f, 36.0f })) {
        m_config.Load();
        SyncSettingsBuffers();
        ApplyHubTheme(m_config.GetTheme());
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Settings", { 126.0f, 36.0f })) {
        const std::string engineRoot = m_engineRootBuffer.data();
        if (!engineRoot.empty() && !IsSdkRoot(Utf8ToPath(engineRoot))) {
            m_errorMessage = "The selected folder is not a versioned FBZZ SDK root.";
        } else {
            m_config.SetEditorExe(m_editorExeBuffer.data());
            m_config.SetSdkRoot(engineRoot);
            m_config.SetTheme(m_selectedThemeIndex == 0 ? "midnight" : "dark");
            if (!m_config.Save()) {
                m_errorMessage = "Failed to save GameHub settings.";
            } else {
                ApplyHubTheme(m_config.GetTheme());
            ApplySdkEnvironment(ResolveSdkRootForTemplate(m_config));
                m_templateManager.Refresh();
            }
        }
    }
}

void HubApp::AddExistingProject()
{
    std::string selectedPath;
    if (!SelectFolder(L"Select FBZZ project folder", selectedPath)) {
        return;
    }

    const std::filesystem::path root = Utf8ToPath(selectedPath);
    if (!engine_util::FileSystem::Exists(root / ".fbzz_proj")) {
        m_errorMessage = "The selected folder does not contain .fbzz_proj.";
        return;
    }

    if (!m_config.AddProject(ToStoredPath(root), CurrentTimestamp())) {
        m_errorMessage = "The selected project is already registered.";
        return;
    }

    m_config.Save();
    m_projectManager.LoadFromConfig(m_config);
}

void HubApp::OpenProject(const ProjectEntry& project)
{
    if (project.migrationRequired) {
        RequestMigration(project);
        return;
    }

    std::string error;
    if (ProcessLauncher::OpenInEditor(m_config, project.path, error)) {
        const std::string now = CurrentTimestamp();
        m_config.UpdateLastOpened(project.path, now);
        m_config.Save();
        m_reloadProjectsAfterRender = true;
    } else {
        m_errorMessage = error;
    }
}

void HubApp::RemoveProject(const ProjectEntry& project)
{
    m_pendingRemoveProject = project.path;
}

void HubApp::RevealProject(const ProjectEntry& project)
{
    const std::wstring path = engine_util::StringUtils::ToWide(project.path);
    ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void HubApp::RequestMigration(const ProjectEntry& project)
{
    m_pendingMigrationProject = project.path;
    m_pendingMigrationName = project.name;
    m_pendingMigrationIsMajor = ParseVersionMajor(project.engineVersion) != ParseVersionMajor(FBZZ_VERSION);
    m_showMigrationDialog = true;
}

bool HubApp::MatchesSearch(const ProjectEntry& project) const
{
    const std::string query = m_searchBuffer.data();
    if (query.empty()) {
        return true;
    }

    return engine_util::StringUtils::ContainsCI(project.name, query)
        || engine_util::StringUtils::ContainsCI(project.path, query);
}

void HubApp::RenderErrorModal()
{
    if (!m_errorMessage.empty()) {
        ImGui::OpenPopup("Error");
    }

    if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", m_errorMessage.c_str());
        if (ImGui::Button("OK", ImVec2(96.0f, 0.0f))) {
            m_errorMessage.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

std::string HubApp::CurrentTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);

    std::tm utc{};
    gmtime_s(&utc, &time);

    std::ostringstream ss;
    ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

} // namespace fbzz::hub
