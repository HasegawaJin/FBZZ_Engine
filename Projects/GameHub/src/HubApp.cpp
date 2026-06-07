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
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

constexpr float SIDEBAR_WIDTH = 160.0f;
constexpr float TOOLBAR_HEIGHT = 40.0f;
constexpr float CARD_HEIGHT = 116.0f;
constexpr float THUMBNAIL_SIZE = 72.0f;

bool IsProjectOpenRequested()
{
    return ImGui::IsItemClicked(0) && ImGui::IsMouseDoubleClicked(0);
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

bool IsEngineRoot(const std::filesystem::path& path)
{
    return engine_util::FileSystem::Exists(path / "CMakeLists.txt")
        && engine_util::FileSystem::Exists(path / "Projects" / "Engine")
        && engine_util::FileSystem::Exists(path / "Projects" / "Math");
}

std::string ResolveEngineRootForTemplate(const HubConfig& config)
{
    if (!config.GetEngineRoot().empty()) {
        return engine_util::FileSystem::PathToUtf8(engine_util::FileSystem::PathFromUtf8(config.GetEngineRoot()));
    }

    std::filesystem::path current = engine_util::FileSystem::GetCurrentDirectory();
    if (IsEngineRoot(current)) {
        return engine_util::FileSystem::PathToUtf8(current);
    }

    current = engine_util::FileSystem::GetExecutableDirectory();
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        if (IsEngineRoot(current)) {
            return engine_util::FileSystem::PathToUtf8(current);
        }
        current = current.parent_path();
    }

    return {};
}

void ApplyEngineEnvironment(const std::string& engineRoot)
{
    if (engineRoot.empty()) {
        return;
    }

    const std::wstring rootW = engine_util::StringUtils::ToWide(engineRoot);
    SetEnvironmentVariableW(L"FBZZ_ENGINE_ROOT", rootW.c_str());

    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegSetValueExW(
            key,
            L"FBZZ_ENGINE_ROOT",
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

bool HubApp::Init(ID3D11Device* device)
{
    m_thumbnailCache.Init(device);
    m_config.Load();
    ApplyEngineEnvironment(ResolveEngineRootForTemplate(m_config));
    m_templateManager.Refresh();
    m_projectManager.LoadFromConfig(m_config);
    return true;
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

    ImGui::Begin("FBZZ Hub", nullptr, flags);

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
    ImGui::BeginChild("Sidebar", ImVec2(SIDEBAR_WIDTH, 0), true);
    ImGui::TextUnformatted("FBZZ Hub");
    ImGui::TextDisabled("v0.1.0");
    ImGui::Separator();

    if (ImGui::Selectable("Projects", m_activePanel == Panel::Projects)) {
        m_activePanel = Panel::Projects;
    }
    if (ImGui::Selectable("Learn", m_activePanel == Panel::Learn)) {
        m_activePanel = Panel::Learn;
    }
    if (ImGui::Selectable("Settings", m_activePanel == Panel::Settings)) {
        m_activePanel = Panel::Settings;
    }

    ImGui::EndChild();
}

void HubApp::RenderProjectsPanel()
{
    ImGui::BeginChild("Toolbar", ImVec2(0, TOOLBAR_HEIGHT), false);
    if (ImGui::Button("New Project")) {
        m_templateManager.Refresh();
        m_newProjectNameBuffer.fill('\0');
        m_newProjectDestinationBuffer.fill('\0');
        m_selectedTemplateIndex = 0;
        m_showNewProjectDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Existing")) {
        AddExistingProject();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        m_config.Load();
        m_templateManager.Refresh();
        m_projectManager.LoadFromConfig(m_config);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Search", m_searchBuffer.data(), m_searchBuffer.size());
    ImGui::EndChild();

    ImGui::Separator();

    if (m_reloadProjectsAfterRender) {
        m_projectManager.LoadFromConfig(m_config);
        m_reloadProjectsAfterRender = false;
    }

    const auto& projects = m_projectManager.GetProjects();
    if (projects.empty()) {
        ImGui::TextDisabled("No projects registered.");
        return;
    }

    ImGui::BeginChild("ProjectsList", ImVec2(0, 0), false);
    for (const auto& project : projects) {
        if (MatchesSearch(project)) {
            RenderProjectCard(project);
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
    ImGui::BeginChild("Card", ImVec2(0, CARD_HEIGHT), true);

    RenderProjectThumbnail(project);
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::TextUnformatted(project.name.c_str());
    if (IsProjectOpenRequested() && project.pathExists) {
        OpenProject(project);
    }

    ImGui::TextDisabled("%s", project.path.c_str());
    ImGui::TextDisabled("last: %s   engine: %s",
        project.lastOpened.empty() ? "-" : project.lastOpened.c_str(),
        project.engineVersion.empty() ? "-" : project.engineVersion.c_str());

    if (project.path == m_failedMigrationProject) {
        ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.32f, 1.0f), "Migration failed");
    } else if (!project.projFileValid || !project.layoutValid || !project.cmakeExists || !project.apiHeaderExists || !project.settingsExists) {
        ImGui::TextColored(ImVec4(1.0f, 0.74f, 0.24f, 1.0f), "Project layout warning");
    } else if (project.engineVersionMismatch) {
        const char* label = project.migrationRequired ? "Migration recommended" : "Update available";
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "%s", label);
        if (project.migrationRequired) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Migrate")) {
                RequestMigration(project);
            }
        }
    } else {
        ImGui::TextDisabled("Ready");
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 88.0f));
    ImGui::BeginDisabled(!project.pathExists);
    if (ImGui::Button("Open", ImVec2(72.0f, 0.0f))) {
        OpenProject(project);
    }
    ImGui::EndDisabled();

    if (ImGui::BeginPopupContextWindow("ProjectMenu", ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Open", nullptr, false, project.pathExists)) {
            OpenProject(project);
        }
        if (ImGui::MenuItem("Reveal in Explorer", nullptr, false, project.pathExists)) {
            RevealProject(project);
        }
        if (ImGui::MenuItem("Migrate", nullptr, false, project.pathExists && project.migrationRequired)) {
            RequestMigration(project);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove from List")) {
            RemoveProject(project);
        }
        ImGui::EndPopup();
    }

    if (!project.pathExists && ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("Project path was not found.");
    }

    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::PopID();
}

void HubApp::RenderProjectThumbnail(const ProjectEntry& project)
{
    const ImVec2 size(THUMBNAIL_SIZE, THUMBNAIL_SIZE);
    const ThumbnailTexture* texture = project.thumbnailExists
        ? m_thumbnailCache.GetOrLoad(project.thumbnailPath)
        : nullptr;

    if (texture && texture->shaderResourceView) {
        ImGui::Image(
            texture->shaderResourceView.Get(),
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

    if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const auto& templates = m_templateManager.GetTemplates();
    if (templates.empty()) {
        ImGui::TextDisabled("No templates found.");
    } else {
        if (m_selectedTemplateIndex < 0 || m_selectedTemplateIndex >= static_cast<int>(templates.size())) {
            m_selectedTemplateIndex = 0;
        }

        const char* currentTemplate = templates[static_cast<size_t>(m_selectedTemplateIndex)].displayName.c_str();
        if (ImGui::BeginCombo("Template", currentTemplate)) {
            for (int i = 0; i < static_cast<int>(templates.size()); ++i) {
                const bool selected = i == m_selectedTemplateIndex;
                if (ImGui::Selectable(templates[static_cast<size_t>(i)].displayName.c_str(), selected)) {
                    m_selectedTemplateIndex = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("%s", templates[static_cast<size_t>(m_selectedTemplateIndex)].description.c_str());
    }

    ImGui::InputText("Name", m_newProjectNameBuffer.data(), m_newProjectNameBuffer.size());
    ImGui::InputText("Location", m_newProjectDestinationBuffer.data(), m_newProjectDestinationBuffer.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse")) {
        std::string selectedPath;
        if (SelectFolder(L"Select project parent folder", selectedPath)) {
            strncpy_s(m_newProjectDestinationBuffer.data(), m_newProjectDestinationBuffer.size(), selectedPath.c_str(), _TRUNCATE);
        }
    }

    const ProjectNameInfo nameInfo = TemplateManager::MakeProjectNameInfo(m_newProjectNameBuffer.data());
    const bool canCreate = !templates.empty()
        && TemplateManager::IsValidProjectNameInfo(nameInfo)
        && m_newProjectDestinationBuffer[0] != '\0';

    if (!canCreate) {
        ImGui::TextColored(ImVec4(1.0f, 0.74f, 0.24f, 1.0f), "Enter an ASCII project name and destination.");
    }

    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button("Create", ImVec2(96.0f, 0.0f))) {
        std::string error;
        const auto& selectedTemplate = templates[static_cast<size_t>(m_selectedTemplateIndex)];
        const std::string createdAt = CurrentTimestamp();
        if (m_templateManager.Instantiate(
                selectedTemplate,
                Utf8ToPath(m_newProjectDestinationBuffer.data()),
                nameInfo,
                createdAt,
                ResolveEngineRootForTemplate(m_config),
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
    if (ImGui::Button("Cancel", ImVec2(96.0f, 0.0f))) {
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
    ImGui::TextUnformatted("Learn");
    ImGui::Separator();
    if (ImGui::Button("Design")) {
        ShellExecuteW(nullptr, L"open", L"docs\\Design.md", nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Hub Design")) {
        ShellExecuteW(nullptr, L"open", L"docs\\hub\\Design.md", nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Repository")) {
        ShellExecuteW(nullptr, L"open", L"https://github.com/", nullptr, nullptr, SW_SHOWNORMAL);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("References");
    ImGui::BulletText("Project metadata: .fbzz_proj");
    ImGui::BulletText("Generated public API: Include/<ProjectName>/ProjectAPI.hpp");
    ImGui::BulletText("Launch contract: FBZZEditor.exe --project <path>");
}

void HubApp::RenderSettingsPanel()
{
    ImGui::TextUnformatted("Settings");
    ImGui::Separator();
    ImGui::Text("Config: %s", engine_util::FileSystem::PathToUtf8(m_config.GetConfigPath()).c_str());
    ImGui::Text("Editor: %s", m_config.GetEditorExe().empty() ? "(same directory)" : m_config.GetEditorExe().c_str());
    ImGui::Text("Engine: %s", m_config.GetEngineRoot().empty() ? "(auto)" : m_config.GetEngineRoot().c_str());
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
