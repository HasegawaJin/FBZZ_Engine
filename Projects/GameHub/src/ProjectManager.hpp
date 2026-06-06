// FBZZ Engine
// ProjectManager.hpp | fbzz::hub
// プロジェクト一覧の読み込みと検証
#pragma once

#include "HubConfig.hpp"
#include "ProjectEntry.hpp"

#include <vector>

namespace fbzz::hub {

class ProjectManager {
public:
    void LoadFromConfig(const HubConfig& config);
    void UpdateLastOpened(const std::string& path, const std::string& lastOpened);

    [[nodiscard]] const std::vector<ProjectEntry>& GetProjects() const { return m_projects; }

private:
    static ProjectEntry BuildEntry(const ConfigProject& configProject);
    static void SortProjects(std::vector<ProjectEntry>& projects);

    std::vector<ProjectEntry> m_projects;
};

} // namespace fbzz::hub
