// FBZZ Engine
// HubConfig.cpp | fbzz::hub
// Hub settings TOML persistence
#include "HubConfig.hpp"
#include <Engine/Util/FileSystem.hpp>

#include <toml++/toml.hpp>
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

std::string NormalizeProjectPath(const std::string& path)
{
    if (path.empty()) return {};
    return engine_util::FileSystem::PathToUtf8(
        engine_util::FileSystem::MakeAbsolute(engine_util::FileSystem::PathFromUtf8(path)));
}

} // namespace

std::filesystem::path HubConfig::ResolveConfigPath()
{
    char* appData = nullptr;
    size_t length = 0;
    _dupenv_s(&appData, &length, "APPDATA");

    std::filesystem::path base;
    if (appData && length > 0) {
        base = appData;
    } else {
        base = engine_util::FileSystem::GetCurrentDirectory();
    }

    free(appData);
    return base / "FBZZHub" / "hub_config.toml";
}

bool HubConfig::Load()
{
    m_configPath = ResolveConfigPath();
    engine_util::FileSystem::EnsureDirectory(m_configPath.parent_path());

    std::string text;
    engine_util::FileSystem::ReadText(m_configPath, text);
    if (text.empty()) {
        return Save();
    }

    auto result = toml::parse(text);
    if (!result) {
        return false;
    }

    auto& table = result.table();
    m_editorExe = table["hub"]["editor_exe"].value_or(std::string{});
    m_engineRoot = table["hub"]["engine_root"].value_or(std::string{});
    m_theme = table["hub"]["theme"].value_or(std::string{ "dark" });

    m_projects.clear();
    if (auto* projects = table["projects"].as_array()) {
        for (auto& node : *projects) {
            auto* project = node.as_table();
            if (!project) continue;

            ConfigProject entry;
            entry.path = NormalizeProjectPath((*project)["path"].value_or(std::string{}));
            entry.lastOpened = (*project)["last_opened"].value_or(std::string{});
            if (!entry.path.empty()) {
                m_projects.push_back(std::move(entry));
            }
        }
    }

    return true;
}

bool HubConfig::Save() const
{
    const std::filesystem::path path = m_configPath.empty() ? ResolveConfigPath() : m_configPath;

    toml::table hub;
    hub.insert("editor_exe", m_editorExe);
    hub.insert("engine_root", m_engineRoot);
    hub.insert("theme", m_theme);

    toml::array projects;
    for (const auto& project : m_projects) {
        toml::table item;
        item.insert("path", project.path);
        item.insert("last_opened", project.lastOpened);
        projects.push_back(std::move(item));
    }

    toml::table root;
    root.insert("hub", std::move(hub));
    root.insert("projects", std::move(projects));

    std::ostringstream ss;
    ss << root;
    return engine_util::FileSystem::WriteText(path, ss.str());
}

void HubConfig::SetProjects(std::vector<ConfigProject> projects)
{
    m_projects = std::move(projects);
}

bool HubConfig::AddProject(const std::string& path, const std::string& lastOpened)
{
    const std::string normalizedPath = NormalizeProjectPath(path);
    if (normalizedPath.empty() || ContainsProject(normalizedPath)) {
        return false;
    }

    m_projects.push_back({ normalizedPath, lastOpened });
    return true;
}

bool HubConfig::RemoveProject(const std::string& path)
{
    const std::string normalizedPath = NormalizeProjectPath(path);
    const auto oldSize = m_projects.size();
    m_projects.erase(
        std::remove_if(m_projects.begin(), m_projects.end(), [&normalizedPath](const ConfigProject& project) {
            return project.path == normalizedPath;
        }),
        m_projects.end());
    return m_projects.size() != oldSize;
}

bool HubConfig::ContainsProject(const std::string& path) const
{
    const std::string normalizedPath = NormalizeProjectPath(path);
    return std::any_of(m_projects.begin(), m_projects.end(), [&normalizedPath](const ConfigProject& project) {
        return project.path == normalizedPath;
    });
}

void HubConfig::UpdateLastOpened(const std::string& path, const std::string& lastOpened)
{
    const std::string normalizedPath = NormalizeProjectPath(path);
    for (auto& project : m_projects) {
        if (project.path == normalizedPath) {
            project.lastOpened = lastOpened;
            return;
        }
    }

    m_projects.push_back({ normalizedPath, lastOpened });
}

} // namespace fbzz::hub
