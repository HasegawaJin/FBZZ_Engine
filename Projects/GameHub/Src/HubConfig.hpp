// FBZZ Engine
// HubConfig.hpp | fbzz::hub
// hub_config.toml の読み書き
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::hub {

struct ConfigProject {
    std::string path;
    std::string lastOpened;
};

class HubConfig {
public:
    bool Load();
    bool Save() const;

    [[nodiscard]] const std::string& GetEditorExe() const { return m_editorExe; }
    [[nodiscard]] const std::string& GetEngineRoot() const { return m_engineRoot; }
    [[nodiscard]] const std::string& GetTheme() const { return m_theme; }
    [[nodiscard]] const std::vector<ConfigProject>& GetProjects() const { return m_projects; }
    [[nodiscard]] const std::filesystem::path& GetConfigPath() const { return m_configPath; }

    void SetEditorExe(const std::string& editorExe) { m_editorExe = editorExe; }
    void SetEngineRoot(const std::string& engineRoot) { m_engineRoot = engineRoot; }
    void SetTheme(const std::string& theme) { m_theme = theme; }

    void SetProjects(std::vector<ConfigProject> projects);
    void UpdateLastOpened(const std::string& path, const std::string& lastOpened);

private:
    static std::filesystem::path ResolveConfigPath();

    std::filesystem::path m_configPath;
    std::string m_editorExe;
    std::string m_engineRoot;
    std::string m_theme = "dark";
    std::vector<ConfigProject> m_projects;
};

} // namespace fbzz::hub
