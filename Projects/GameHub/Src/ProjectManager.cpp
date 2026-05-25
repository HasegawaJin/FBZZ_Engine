// FBZZ Engine
// ProjectManager.cpp | fbzz::hub
// .fbzz_proj を読み取って Hub 表示用情報に変換する
#include "ProjectManager.hpp"

#include <toml++/toml.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace fbzz::hub {

namespace {

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool ExistsRelative(const std::filesystem::path& root, const std::string& relativePath)
{
    if (relativePath.empty()) return false;
    return std::filesystem::exists(root / std::filesystem::path(relativePath));
}

bool HasStandardLayout(const std::filesystem::path& root)
{
    return std::filesystem::exists(root / "Assets")
        && std::filesystem::exists(root / "Src")
        && std::filesystem::exists(root / "Include");
}

} // namespace

void ProjectManager::LoadFromConfig(const HubConfig& config)
{
    m_projects.clear();
    for (const auto& project : config.GetProjects()) {
        m_projects.push_back(BuildEntry(project));
    }

    SortProjects(m_projects);
}

void ProjectManager::UpdateLastOpened(const std::string& path, const std::string& lastOpened)
{
    for (auto& project : m_projects) {
        if (project.path == path) {
            project.lastOpened = lastOpened;
            SortProjects(m_projects);
            return;
        }
    }
}

ProjectEntry ProjectManager::BuildEntry(const ConfigProject& configProject)
{
    ProjectEntry entry;
    entry.path = configProject.path;
    entry.lastOpened = configProject.lastOpened;

    const std::filesystem::path root(configProject.path);
    entry.pathExists = std::filesystem::exists(root);
    if (!entry.pathExists) {
        entry.name = "(不明)";
        return entry;
    }

    const std::filesystem::path projectFile = root / ".fbzz_proj";
    const std::string text = ReadText(projectFile);
    if (text.empty()) {
        entry.name = "(不明)";
        return entry;
    }

    auto result = toml::parse(text);
    if (!result) {
        entry.name = "(不明)";
        return entry;
    }

    auto& table = result.table();
    entry.name = table["project"]["name"].value_or(std::string{ "(不明)" });
    entry.projectId = table["project"]["project_id"].value_or(std::string{});
    entry.engineVersion = table["project"]["engine_version"].value_or(std::string{ "-" });

    const std::string settingsPath = table["project"]["settings_path"].value_or(std::string{});
    const std::string apiHeader = table["project"]["public_api_header"].value_or(std::string{});

    entry.projFileValid = entry.name != "(不明)" && entry.engineVersion != "-";
    entry.settingsExists = ExistsRelative(root, settingsPath);
    entry.apiHeaderExists = ExistsRelative(root, apiHeader);
    entry.cmakeExists = std::filesystem::exists(root / "CMakeLists.txt");
    entry.layoutValid = HasStandardLayout(root);

    return entry;
}

void ProjectManager::SortProjects(std::vector<ProjectEntry>& projects)
{
    std::stable_sort(projects.begin(), projects.end(), [](const ProjectEntry& a, const ProjectEntry& b) {
        return a.lastOpened > b.lastOpened;
    });
}

} // namespace fbzz::hub
