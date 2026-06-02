// FBZZ Engine
// ProjectResolver.cpp | fbzz::sandbox
// .fbzz_proj と ProjectSettings から Sandbox 起動対象を解決する
#include "ProjectResolver.hpp"

#include "Util/FileUtil.hpp"
#include "Util/PathUtil.hpp"

#include <toml++/toml.hpp>

namespace fbzz::sandbox {
namespace {

std::filesystem::path ReadTomlRelativePath(const toml::table& table, const char* tableName, const char* key)
{
    const std::string value = table[tableName][key].value_or(std::string{});
    return value.empty() ? std::filesystem::path{} : std::filesystem::path(util::Utf8ToWide(value));
}

bool IsTemplatePlaceholder(const std::filesystem::path& path)
{
    const std::wstring value = path.wstring();
    return value.size() >= 4 && value.rfind(L"{{", 0) == 0;
}

std::filesystem::path ResolvePathUnderRoot(const std::filesystem::path& filePath,
                                           const std::filesystem::path& root)
{
    if (!filePath.is_absolute())
        return util::MakeAbsolute(root / filePath);

    if (util::Exists(filePath))
        return filePath;

    // WHY: 移動後プロジェクトの .fbzz_proj に絶対パスが残っている場合でも、末尾成分を root 配下へ寄せて復旧する。
    std::filesystem::path suffix;
    std::filesystem::path p = filePath;
    while (p.has_relative_path()) {
        const auto component = p.filename();
        suffix = suffix.empty() ? component : (component / suffix);
        p = p.parent_path();
        const auto candidate = util::MakeAbsolute(root / suffix);
        if (util::Exists(candidate))
            return candidate;
    }
    return util::MakeAbsolute(root / filePath);
}

} // namespace

bool ProjectResolver::Resolve(const std::filesystem::path& projectPath)
{
    m_project = {};
    m_errorMessage.clear();

    m_project.root = util::MakeAbsolute(projectPath);
    if (m_project.root.empty()) {
        m_errorMessage = L"Project path was not specified and the Sandbox default project was not found.\n\nsandbox.exe --project <path>";
        return false;
    }
    if (!util::Exists(m_project.root)) {
        m_errorMessage = L"Project folder was not found.\n\n" + m_project.root.wstring();
        return false;
    }

    m_project.projectFile = m_project.root / L".fbzz_proj";
    const std::string projectText = util::ReadText(m_project.projectFile);
    if (projectText.empty()) {
        m_errorMessage = L".fbzz_proj could not be read.\n\n" + m_project.projectFile.wstring();
        return false;
    }

    toml::parse_result projectResult = toml::parse(projectText);
    if (!projectResult) {
        m_errorMessage = L".fbzz_proj could not be parsed.\n\n" + m_project.projectFile.wstring();
        return false;
    }

    const toml::table& projectTable = projectResult.table();
    std::filesystem::path settingsPath = ReadTomlRelativePath(projectTable, "project", "settings_path");
    const std::filesystem::path defaultScene = ReadTomlRelativePath(projectTable, "project", "default_scene");
    if (IsTemplatePlaceholder(settingsPath)) {
        settingsPath = L"ProjectSettings/ProjectSettings.toml";
    }
    if (settingsPath.empty()) {
        m_errorMessage = L".fbzz_proj does not define project.settings_path.";
        return false;
    }

    m_project.settingsFile = ResolvePathUnderRoot(settingsPath, m_project.root);
    if (!util::Exists(m_project.settingsFile)) {
        m_errorMessage = L"ProjectSettings file was not found.\n\n" + m_project.settingsFile.wstring();
        return false;
    }

    const std::string settingsText = util::ReadText(m_project.settingsFile);
    if (settingsText.empty()) {
        m_errorMessage = L"ProjectSettings file could not be read.\n\n" + m_project.settingsFile.wstring();
        return false;
    }

    toml::parse_result settingsResult = toml::parse(settingsText);
    if (!settingsResult) {
        m_errorMessage = L"ProjectSettings file could not be parsed.\n\n" + m_project.settingsFile.wstring();
        return false;
    }

    const toml::table& settingsTable = settingsResult.table();
    std::filesystem::path scenePath = ReadTomlRelativePath(settingsTable, "runtime", "start_scene");
    if (scenePath.empty()) {
        scenePath = defaultScene;
    }
    if (scenePath.empty()) {
        m_errorMessage = L"Project does not define a start scene.";
        return false;
    }

    m_project.sceneFile = ResolvePathUnderRoot(scenePath, m_project.root);
    if (!util::Exists(m_project.sceneFile)) {
        m_errorMessage = L"Start scene file was not found.\n\n" + m_project.sceneFile.wstring();
        return false;
    }

    return true;
}

} // namespace fbzz::sandbox
