// FBZZ Engine
// HubConfig.cpp | fbzz::hub
// Hub settings TOML persistence
#include "HubConfig.hpp"

#include <Windows.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cstdlib>
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

bool WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    file << text;
    return static_cast<bool>(file);
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

std::string NormalizeProjectPath(const std::string& path)
{
    if (path.empty()) return {};

    std::error_code ec;
    const std::filesystem::path source(Utf8ToWide(path));
    const std::filesystem::path absolute = std::filesystem::absolute(source, ec);
    const std::filesystem::path normalized = ec ? source : absolute.lexically_normal();
    return WideToUtf8(normalized.wstring());
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
        base = std::filesystem::current_path();
    }

    free(appData);
    return base / "FBZZHub" / "hub_config.toml";
}

bool HubConfig::Load()
{
    m_configPath = ResolveConfigPath();
    std::filesystem::create_directories(m_configPath.parent_path());

    const std::string text = ReadText(m_configPath);
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
    std::filesystem::create_directories(m_configPath.empty() ? ResolveConfigPath().parent_path() : m_configPath.parent_path());
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
    return WriteText(path, ss.str());
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
