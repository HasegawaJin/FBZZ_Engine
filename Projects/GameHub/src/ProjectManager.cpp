// FBZZ Engine
// ProjectManager.cpp | fbzz::hub
// .fbzz_proj validation for Hub project cards
#include "ProjectManager.hpp"
#include <Engine/Util/FileSystem.hpp>

#include <toml++/toml.hpp>
#include <algorithm>
#include <filesystem>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

bool ExistsRelative(const std::filesystem::path& root, const std::string& relativePath)
{
    if (relativePath.empty()) return false;
    return engine_util::FileSystem::Exists(root / engine_util::FileSystem::PathFromUtf8(relativePath));
}

bool HasStandardLayout(const std::filesystem::path& root)
{
    return engine_util::FileSystem::Exists(root / "Assets")
        && engine_util::FileSystem::Exists(root / "Src")
        && engine_util::FileSystem::Exists(root / "Include");
}

bool HasGeneratedRoots(const std::filesystem::path& root)
{
    return engine_util::FileSystem::Exists(root / "Lib")
        && engine_util::FileSystem::Exists(root / "Binaries")
        && engine_util::FileSystem::Exists(root / "Build");
}

std::string FindThumbnailPath(const std::filesystem::path& root)
{
    const std::filesystem::path thumbnail = root / "thumbnail.png";
    if (engine_util::FileSystem::Exists(thumbnail)) {
        return engine_util::FileSystem::PathToUtf8(thumbnail);
    }

    const std::filesystem::path assetThumbnail = root / "Assets" / "thumbnail.png";
    if (engine_util::FileSystem::Exists(assetThumbnail)) {
        return engine_util::FileSystem::PathToUtf8(assetThumbnail);
    }

    return {};
}

bool HasStringField(toml::table& table, const std::string& tableName, const std::string& key)
{
    return table[tableName][key].is_string();
}

bool HasRequiredProjectFields(toml::table& table)
{
    return HasStringField(table, "project", "name")
        && HasStringField(table, "project", "project_id")
        && HasStringField(table, "project", "cpp_namespace")
        && HasStringField(table, "project", "target_name")
        && HasStringField(table, "project", "engine_version")
        && HasStringField(table, "project", "created_at")
        && HasStringField(table, "project", "default_scene")
        && HasStringField(table, "project", "asset_root")
        && HasStringField(table, "project", "settings_path")
        && HasStringField(table, "project", "api_root")
        && HasStringField(table, "project", "public_api_header")
        && HasStringField(table, "project", "script_root")
        && HasStringField(table, "project", "library_root")
        && HasStringField(table, "project", "binary_root")
        && HasStringField(table, "project", "build_root")
        && HasStringField(table, "engine", "root");
}

int ParseVersionPart(const std::string& version, size_t& offset)
{
    int value = 0;
    while (offset < version.size() && version[offset] >= '0' && version[offset] <= '9') {
        value = value * 10 + (version[offset] - '0');
        ++offset;
    }
    if (offset < version.size() && version[offset] == '.') {
        ++offset;
    }
    return value;
}

bool NeedsMigration(const std::string& projectVersion)
{
    if (projectVersion.empty() || projectVersion == "-") {
        return false;
    }

    size_t projectOffset = 0;
    size_t currentOffset = 0;
    const int projectMajor = ParseVersionPart(projectVersion, projectOffset);
    const int currentMajor = ParseVersionPart(FBZZ_VERSION, currentOffset);
    const int projectMinor = ParseVersionPart(projectVersion, projectOffset);
    const int currentMinor = ParseVersionPart(FBZZ_VERSION, currentOffset);
    return projectMajor != currentMajor || projectMinor != currentMinor;
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

    const std::filesystem::path root = engine_util::FileSystem::PathFromUtf8(configProject.path);
    entry.pathExists = engine_util::FileSystem::Exists(root);
    if (!entry.pathExists) {
        entry.name = "(Unknown)";
        return entry;
    }

    const std::filesystem::path projectFile = root / ".fbzz_proj";
    std::string text;
    engine_util::FileSystem::ReadText(projectFile, text);
    if (text.empty()) {
        entry.name = "(Unknown)";
        return entry;
    }

    auto result = toml::parse(text);
    if (!result) {
        entry.name = "(Unknown)";
        return entry;
    }

    auto& table = result.table();
    entry.name = table["project"]["name"].value_or(std::string{ "(Unknown)" });
    entry.projectId = table["project"]["project_id"].value_or(std::string{});
    entry.engineVersion = table["project"]["engine_version"].value_or(std::string{ "-" });

    const std::string settingsPath = table["project"]["settings_path"].value_or(std::string{});
    const std::string apiHeader = table["project"]["public_api_header"].value_or(std::string{});

    entry.projFileValid = HasRequiredProjectFields(table);
    entry.settingsExists = ExistsRelative(root, settingsPath);
    entry.apiHeaderExists = ExistsRelative(root, apiHeader);
    entry.cmakeExists = engine_util::FileSystem::Exists(root / "CMakeLists.txt");
    entry.layoutValid = HasStandardLayout(root);
    entry.generatedRootsExist = HasGeneratedRoots(root);
    entry.thumbnailPath = FindThumbnailPath(root);
    entry.thumbnailExists = !entry.thumbnailPath.empty();
    entry.engineVersionMismatch = entry.engineVersion != FBZZ_VERSION && entry.engineVersion != "-";
    entry.migrationRequired = NeedsMigration(entry.engineVersion);

    return entry;
}

void ProjectManager::SortProjects(std::vector<ProjectEntry>& projects)
{
    std::stable_sort(projects.begin(), projects.end(), [](const ProjectEntry& a, const ProjectEntry& b) {
        return a.lastOpened > b.lastOpened;
    });
}

} // namespace fbzz::hub
