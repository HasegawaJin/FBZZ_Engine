// FBZZ Engine
// ProjectResolver.cpp | fbzz
// .fbzz_proj と ProjectSettings から起動対象を解決する実装
#include <Engine/ProjectResolver.hpp>

#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <toml++/toml.hpp>

namespace fbzz {

namespace {

std::filesystem::path ReadTomlRelativePath(const toml::table& table, const char* tableName, const char* key)
{
    const std::string value = table[tableName][key].value_or(std::string{});
    return value.empty() ? std::filesystem::path{} : std::filesystem::path(util::StringUtils::ToWide(value));
}

bool IsTemplatePlaceholder(const std::filesystem::path& path)
{
    const std::wstring value = path.wstring();
    return value.size() >= 4 && value.rfind(L"{{", 0) == 0;
}

// 絶対パスが root 直下に存在しない場合 (プロジェクト移動後) に
// パスの末尾からサフィックスを順に試して root 相対でファイルを探す。
std::filesystem::path ResolvePathUnderRoot(const std::filesystem::path& filePath,
                                           const std::filesystem::path& root)
{
    if (!filePath.is_absolute())
        return util::FileSystem::MakeAbsolute(root / filePath);

    if (util::FileSystem::Exists(filePath))
        return filePath;

    // WHY: 移動後プロジェクトの .fbzz_proj に絶対パスが残っている場合でも、末尾成分を root 配下へ寄せて復旧する。
    std::filesystem::path suffix;
    std::filesystem::path p = filePath;
    while (p.has_relative_path()) {
        const auto component = p.filename();
        suffix = suffix.empty() ? component : (component / suffix);
        p = p.parent_path();
        const auto candidate = util::FileSystem::MakeAbsolute(root / suffix);
        if (util::FileSystem::Exists(candidate))
            return candidate;
    }
    return util::FileSystem::MakeAbsolute(root / filePath);
}

} // namespace

bool ProjectResolver::Resolve(const std::filesystem::path& projectPath)
{
    m_project = {};
    m_errorMessage.clear();

    m_project.root = util::FileSystem::MakeAbsolute(projectPath);
    if (m_project.root.empty()) {
        m_errorMessage = L"Project path was not specified.\n\n--project <path>";
        return false;
    }
    if (!util::FileSystem::Exists(m_project.root)) {
        m_errorMessage = L"Project folder was not found.\n\n" + m_project.root.wstring();
        return false;
    }

    m_project.projectFile = m_project.root / L".fbzz_proj";
    std::string projectText;
    if (!util::FileSystem::ReadText(m_project.projectFile, projectText)) {
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
    const std::filesystem::path scriptsDllRel = ReadTomlRelativePath(projectTable, "project", "scripts_dll");
    if (IsTemplatePlaceholder(settingsPath)) {
        settingsPath = L"ProjectSettings/ProjectSettings.toml";
    }
    if (settingsPath.empty()) {
        m_errorMessage = L".fbzz_proj does not define project.settings_path.";
        return false;
    }

    m_project.settingsFile = ResolvePathUnderRoot(settingsPath, m_project.root);
    if (!util::FileSystem::Exists(m_project.settingsFile)) {
        m_errorMessage = L"ProjectSettings file was not found.\n\n" + m_project.settingsFile.wstring();
        return false;
    }

    std::string settingsText;
    if (!util::FileSystem::ReadText(m_project.settingsFile, settingsText)) {
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
    if (!util::FileSystem::Exists(m_project.sceneFile)) {
        m_errorMessage = L"Start scene file was not found.\n\n" + m_project.sceneFile.wstring();
        return false;
    }

    // scripts_dll は省略可。指定がある場合のみ解決する。
    // WHY: 開発環境では .fbzz_proj に scripts_dll を書かず exe 隣の DLL を使うケースがある。
    //      配布ビルドでは BuildPipeline が必ず書き出すため、ここでは存在チェックをしない。
    if (!scriptsDllRel.empty())
        m_project.scriptsDll = ResolvePathUnderRoot(scriptsDllRel, m_project.root);

    return true;
}

} // namespace fbzz
