// FBZZ Engine
// HubApp.cpp | fbzz::hub
// Hub の ImGui UI 実装
#include "HubApp.hpp"
#include "ProcessLauncher.hpp"

#include <Windows.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace fbzz::hub {

namespace {

constexpr float SIDEBAR_WIDTH = 160.0f;
constexpr float TOOLBAR_HEIGHT = 40.0f;
constexpr float CARD_HEIGHT = 104.0f;

bool IsProjectOpenRequested()
{
    return ImGui::IsItemClicked(0) && ImGui::IsMouseDoubleClicked(0);
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return std::filesystem::path(text).wstring();
    }

    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::filesystem::path(text).string();
    }

    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
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

    outPath = WideToUtf8(path);
    return true;
}

std::string ToLower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

} // namespace

bool HubApp::Init()
{
    m_config.Load();
    m_templateManager.Refresh();
    m_projectManager.LoadFromConfig(m_config);
    return true;
}

void HubApp::Shutdown()
{
    m_config.Save();
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

    ImGui::TextUnformatted(project.name.c_str());
    if (IsProjectOpenRequested() && project.pathExists) {
        OpenProject(project);
    }

    ImGui::TextDisabled("%s", project.path.c_str());
    ImGui::TextDisabled("last: %s   engine: %s",
        project.lastOpened.empty() ? "-" : project.lastOpened.c_str(),
        project.engineVersion.empty() ? "-" : project.engineVersion.c_str());

    if (!project.projFileValid || !project.layoutValid || !project.cmakeExists || !project.apiHeaderExists || !project.settingsExists) {
        ImGui::TextColored(ImVec4(1.0f, 0.74f, 0.24f, 1.0f), "Project layout warning");
    } else if (project.engineVersionMismatch) {
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f),
            project.migrationRequired ? "Migration recommended" : "Update available");
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
        ImGui::Separator();
        if (ImGui::MenuItem("Remove from List")) {
            RemoveProject(project);
        }
        ImGui::EndPopup();
    }

    if (!project.pathExists && ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("Project path was not found.");
    }

    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::PopID();
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
                std::filesystem::path(m_newProjectDestinationBuffer.data()),
                nameInfo,
                createdAt,
                error)) {
            const std::string projectPath = (std::filesystem::path(m_newProjectDestinationBuffer.data()) / nameInfo.targetName).string();
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

void HubApp::RenderLearnPanel()
{
    ImGui::TextUnformatted("Learn");
    ImGui::Separator();
    ImGui::TextDisabled("Documentation links will be added in Phase 3.");
}

void HubApp::RenderSettingsPanel()
{
    ImGui::TextUnformatted("Settings");
    ImGui::Separator();
    ImGui::Text("Config: %s", m_config.GetConfigPath().string().c_str());
    ImGui::Text("Editor: %s", m_config.GetEditorExe().empty() ? "(same directory)" : m_config.GetEditorExe().c_str());
    ImGui::Text("Engine: %s", m_config.GetEngineRoot().empty() ? "(auto)" : m_config.GetEngineRoot().c_str());
}

void HubApp::AddExistingProject()
{
    std::string selectedPath;
    if (!SelectFolder(L"Select FBZZ project folder", selectedPath)) {
        return;
    }

    const std::filesystem::path root(selectedPath);
    if (!std::filesystem::exists(root / ".fbzz_proj")) {
        m_errorMessage = "The selected folder does not contain .fbzz_proj.";
        return;
    }

    if (!m_config.AddProject(selectedPath, CurrentTimestamp())) {
        m_errorMessage = "The selected project is already registered.";
        return;
    }

    m_config.Save();
    m_projectManager.LoadFromConfig(m_config);
}

void HubApp::OpenProject(const ProjectEntry& project)
{
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
    const std::wstring path = Utf8ToWide(project.path);
    ShellExecuteW(nullptr, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool HubApp::MatchesSearch(const ProjectEntry& project) const
{
    const std::string query = ToLower(m_searchBuffer.data());
    if (query.empty()) {
        return true;
    }

    return ToLower(project.name).find(query) != std::string::npos
        || ToLower(project.path).find(query) != std::string::npos;
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
