// FBZZ Engine
// HubApp.hpp | fbzz::hub
// Hub UI の操作制御
#pragma once

#include "HubConfig.hpp"
#include "ProjectManager.hpp"
#include "TemplateManager.hpp"

#include <array>
#include <string>

namespace fbzz::hub {

class HubApp {
public:
    bool Init();
    void Shutdown();
    void Render();

private:
    enum class Panel {
        Projects,
        Learn,
        Settings
    };

    void RenderSidebar();
    void RenderProjectsPanel();
    void RenderProjectCard(const ProjectEntry& project);
    void RenderNewProjectDialog();
    void RenderLearnPanel();
    void RenderSettingsPanel();
    void RenderErrorModal();

    void AddExistingProject();
    void OpenProject(const ProjectEntry& project);
    void RemoveProject(const ProjectEntry& project);
    void RevealProject(const ProjectEntry& project);
    [[nodiscard]] bool MatchesSearch(const ProjectEntry& project) const;

    static std::string CurrentTimestamp();

    HubConfig m_config;
    ProjectManager m_projectManager;
    TemplateManager m_templateManager;
    Panel m_activePanel = Panel::Projects;
    std::string m_errorMessage;
    std::array<char, 128> m_searchBuffer{};
    std::array<char, 128> m_newProjectNameBuffer{};
    std::array<char, 260> m_newProjectDestinationBuffer{};
    int m_selectedTemplateIndex = 0;
    bool m_showNewProjectDialog = false;
    bool m_reloadProjectsAfterRender = false;
    std::string m_pendingRemoveProject;
};

} // namespace fbzz::hub
