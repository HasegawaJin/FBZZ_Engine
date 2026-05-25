// FBZZ Engine
// HubApp.hpp | fbzz::hub
// Hub UI の操作制御
#pragma once

#include "HubConfig.hpp"
#include "ProjectManager.hpp"

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
    void RenderLearnPanel();
    void RenderSettingsPanel();
    void RenderErrorModal();

    static std::string CurrentTimestamp();

    HubConfig m_config;
    ProjectManager m_projectManager;
    Panel m_activePanel = Panel::Projects;
    std::string m_errorMessage;
};

} // namespace fbzz::hub
