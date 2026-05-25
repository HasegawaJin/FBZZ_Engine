// FBZZ Engine
// HubApp.cpp | fbzz::hub
// Hub の ImGui UI 実装
#include "HubApp.hpp"
#include "ProcessLauncher.hpp"

#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fbzz::hub {

namespace {

constexpr float SIDEBAR_WIDTH = 160.0f;
constexpr float TOOLBAR_HEIGHT = 40.0f;
constexpr float CARD_HEIGHT = 72.0f;

bool IsProjectOpenRequested()
{
    return ImGui::IsItemClicked(0) && ImGui::IsMouseDoubleClicked(0);
}

} // namespace

bool HubApp::Init()
{
    m_config.Load();
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
    ImGui::BeginDisabled();
    ImGui::Button("New Project");
    ImGui::SameLine();
    ImGui::Button("Add Existing");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        m_config.Load();
        m_projectManager.LoadFromConfig(m_config);
    }
    ImGui::EndChild();

    ImGui::Separator();

    const auto& projects = m_projectManager.GetProjects();
    if (projects.empty()) {
        ImGui::TextDisabled("No projects registered.");
        ImGui::TextDisabled("Phase 2 will add New Project and Add Existing.");
        return;
    }

    ImGui::BeginChild("ProjectsList", ImVec2(0, 0), false);
    for (const auto& project : projects) {
        RenderProjectCard(project);
    }
    ImGui::EndChild();
}

void HubApp::RenderProjectCard(const ProjectEntry& project)
{
    ImGui::PushID(project.path.c_str());
    ImGui::BeginChild("Card", ImVec2(0, CARD_HEIGHT), true);

    if (!project.pathExists) {
        ImGui::BeginDisabled();
    }

    ImGui::TextUnformatted(project.name.c_str());
    if (IsProjectOpenRequested() && project.pathExists) {
        std::string error;
        if (ProcessLauncher::OpenInEditor(m_config, project.path, error)) {
            const std::string now = CurrentTimestamp();
            m_config.UpdateLastOpened(project.path, now);
            m_config.Save();
            m_projectManager.UpdateLastOpened(project.path, now);
        } else {
            m_errorMessage = error;
        }
    }

    ImGui::TextDisabled("%s", project.path.c_str());
    ImGui::TextDisabled("last: %s   engine: %s",
        project.lastOpened.empty() ? "-" : project.lastOpened.c_str(),
        project.engineVersion.empty() ? "-" : project.engineVersion.c_str());

    ImGui::SameLine();
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 88.0f));
    if (ImGui::Button("Open", ImVec2(72.0f, 0.0f))) {
        std::string error;
        if (ProcessLauncher::OpenInEditor(m_config, project.path, error)) {
            const std::string now = CurrentTimestamp();
            m_config.UpdateLastOpened(project.path, now);
            m_config.Save();
            m_projectManager.UpdateLastOpened(project.path, now);
        } else {
            m_errorMessage = error;
        }
    }

    if (!project.pathExists) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("パスが見つかりません");
        }
    }

    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::PopID();
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
