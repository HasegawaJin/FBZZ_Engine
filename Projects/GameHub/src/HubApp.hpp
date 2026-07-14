// FBZZ Engine
// HubApp.hpp | fbzz::hub
// Hub UI の操作制御
#pragma once

#include "HubConfig.hpp"
#include "ProjectManager.hpp"
#include "TemplateManager.hpp"
#include "ThumbnailCache.hpp"

#include <array>
#include <string>

namespace fbzz::renderer {
class ResourceManager;
class IImGuiRenderer;
} // namespace fbzz::renderer

namespace fbzz::hub {

class HubApp {
public:
    // GPU リソース生成用 ResourceManager と ImGui 表示 ID 解決用 IImGuiRenderer を受け取る。
    // WHY: サムネイル表示のために DX11 デバイスを直接受けていたが、抽象層経由に切り替える。
    bool Init(fbzz::renderer::ResourceManager& resources, fbzz::renderer::IImGuiRenderer& imgui);
    void Shutdown();
    void Render();

private:
    enum class Panel {
        Projects,
        Learn,
        Settings
    };

    // プロジェクト一覧で選択できる安定ソートの基準を表す。
    enum class ProjectSort {
        Recent,
        Name,
        EngineVersion
    };

    void RenderSidebar();
    void RenderProjectsPanel();
    void RenderProjectCard(const ProjectEntry& project);
    void RenderProjectThumbnail(const ProjectEntry& project);
    void RenderNewProjectDialog();
    void RenderMigrationDialog();
    void RenderLearnPanel();
    void RenderSettingsPanel();
    void RenderErrorModal();
    void PrepareNewProjectDialog();
    void SyncSettingsBuffers();

    void AddExistingProject();
    void OpenProject(const ProjectEntry& project);
    void RemoveProject(const ProjectEntry& project);
    void RevealProject(const ProjectEntry& project);
    void RequestMigration(const ProjectEntry& project);
    [[nodiscard]] bool MatchesSearch(const ProjectEntry& project) const;

    static std::string CurrentTimestamp();

    HubConfig m_config;
    ProjectManager m_projectManager;
    TemplateManager m_templateManager;
    ThumbnailCache m_thumbnailCache;
    Panel m_activePanel = Panel::Projects;
    std::string m_errorMessage;
    std::array<char, 128> m_searchBuffer{};
    std::array<char, 128> m_newProjectNameBuffer{};
    std::array<char, 260> m_newProjectDestinationBuffer{};
    std::array<char, 260> m_editorExeBuffer{};
    std::array<char, 260> m_engineRootBuffer{};
    int m_selectedTemplateIndex = 0;
    int m_selectedThemeIndex = 0;
    ProjectSort m_projectSort = ProjectSort::Recent;
    bool m_sortAscending = false;
    bool m_showNewProjectDialog = false;
    bool m_reloadProjectsAfterRender = false;
    std::string m_pendingRemoveProject;
    std::string m_pendingMigrationProject;
    std::string m_pendingMigrationName;
    std::string m_failedMigrationProject;
    bool m_pendingMigrationIsMajor = false;
    bool m_showMigrationDialog = false;
};

} // namespace fbzz::hub
