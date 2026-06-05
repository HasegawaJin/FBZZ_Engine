// FBZZ Engine
// TemplateManager.cpp | fbzz::hub
// Project template discovery and instantiation
#include "TemplateManager.hpp"

#include "Util/HubUtil.hpp"

#include <Windows.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>

namespace fbzz::hub {

namespace {

namespace util = fbzz::hub::util;

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
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    file << text;
    return static_cast<bool>(file);
}

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

bool IsTextTemplate(const std::filesystem::path& path)
{
    const std::string extension = path.extension().string();
    return extension == ".tmpl"
        || extension == ".fbzz_proj"
        || extension == ".txt"
        || extension == ".toml"
        || extension == ".json"
        || extension == ".hpp"
        || extension == ".cpp"
        || extension == ".h"
        || extension == ".c"
        || extension == ".md"
        || extension == ".gitignore"
        || extension.empty();
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool InsertAfterMarker(const std::filesystem::path& path, const std::string& marker, const std::string& insertion)
{
    std::string text = ReadText(path);
    if (text.empty() || text.find(insertion) != std::string::npos)
        return true;

    size_t markerPos = std::string::npos;
    size_t lineStart = 0;
    while (lineStart < text.size()) {
        const size_t lineEnd = text.find('\n', lineStart);
        const size_t nextLine = (lineEnd == std::string::npos) ? text.size() : lineEnd + 1;
        const std::string_view line(text.data() + lineStart, nextLine - lineStart);
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            const std::string_view trimmed = line.substr(first);
            const std::string expectedMarkerLine = "// " + marker;
            if (trimmed.starts_with(std::string_view(expectedMarkerLine))) {
                markerPos = lineStart + first;
                break;
            }
        }
        if (lineEnd == std::string::npos) break;
        lineStart = nextLine;
    }
    if (markerPos == std::string::npos) return false;

    const size_t lineEnd = text.find('\n', markerPos);
    size_t insertPos = (lineEnd == std::string::npos) ? text.size() : lineEnd + 1;
    if (marker == "@@FBZZ_SCRIPT_ENTRIES_BEGIN") {
        const size_t nextLineEnd = text.find('\n', insertPos);
        const std::string_view nextLine(
            text.data() + insertPos,
            (nextLineEnd == std::string::npos ? text.size() : nextLineEnd) - insertPos);
        if (nextLine.find("static const std::vector<ScriptEntry> entries = {") != std::string_view::npos)
            insertPos = (nextLineEnd == std::string::npos) ? text.size() : nextLineEnd + 1;
    }
    text.insert(insertPos, insertion + "\n");
    return WriteText(path, text);
}

} // namespace

void TemplateManager::Refresh()
{
    m_templates.clear();

    CollectTemplatesFromRoot(ResolveSourceTemplatesRoot(), m_templates);
    CollectTemplatesFromRoot(util::GetExecutableDirectory() / "Templates", m_templates);

    std::sort(m_templates.begin(), m_templates.end(), [](const TemplateInfo& a, const TemplateInfo& b) {
        return a.displayName < b.displayName;
    });
}

bool TemplateManager::Instantiate(
    const TemplateInfo& templateInfo,
    const std::filesystem::path& destinationRoot,
    const ProjectNameInfo& nameInfo,
    const std::string& createdAt,
    const std::string& engineRoot,
    std::string& errorMessage) const
{
    if (!IsValidProjectNameInfo(nameInfo)) {
        errorMessage = "Project name must contain ASCII letters or digits.";
        return false;
    }

    const std::filesystem::path projectRoot = destinationRoot / nameInfo.targetName;
    std::error_code ec;
    if (std::filesystem::exists(projectRoot, ec)) {
        errorMessage = "Destination project folder already exists.";
        return false;
    }

    std::filesystem::create_directories(projectRoot, ec);
    if (ec) {
        errorMessage = "Failed to create destination project folder.";
        return false;
    }

    std::filesystem::recursive_directory_iterator iter(templateInfo.rootPath, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (iter != end) {
        if (ec) {
            errorMessage = "Failed to enumerate template files.";
            return false;
        }

        const std::filesystem::directory_entry entry = *iter;
        iter.increment(ec);

        const std::filesystem::path relativePath = std::filesystem::relative(entry.path(), templateInfo.rootPath, ec);
        if (ec || relativePath == "template.toml") {
            continue;
        }

        std::filesystem::path outputRelative;
        for (const auto& part : relativePath) {
            outputRelative /= ApplyPlaceholders(part.string(), nameInfo, createdAt, engineRoot);
        }

        std::filesystem::path outputPath = projectRoot / outputRelative;
        std::error_code entryEc;
        if (entry.is_directory(entryEc)) {
            std::filesystem::create_directories(outputPath, ec);
            if (ec) {
                errorMessage = "Failed to create a project directory.";
                return false;
            }
            continue;
        }

        entryEc.clear();
        if (!entry.is_regular_file(entryEc)) {
            continue;
        }

        std::string outputFileName = outputPath.filename().string();
        if (outputFileName.size() > 5 && outputFileName.substr(outputFileName.size() - 5) == ".tmpl") {
            outputFileName.resize(outputFileName.size() - 5);
            outputPath = outputPath.parent_path() / outputFileName;
        }

        if (IsTextTemplate(entry.path())) {
            const std::string text = ReadText(entry.path());
            if (!WriteText(outputPath, ApplyPlaceholders(text, nameInfo, createdAt, engineRoot))) {
                errorMessage = "Failed to write a project file.";
                return false;
            }
        } else {
            std::filesystem::create_directories(outputPath.parent_path(), ec);
            std::filesystem::copy_file(entry.path(), outputPath, std::filesystem::copy_options::none, ec);
            if (ec) {
                errorMessage = "Failed to copy a project asset.";
                return false;
            }
        }
    }

    if (!engineRoot.empty()) {
        if (!CopyEngineAssets(engineRoot, projectRoot, errorMessage)) {
            return false;
        }
    }

    if (!SyncCopiedScriptRegistrations(projectRoot, nameInfo, errorMessage)) {
        return false;
    }

    return true;
}

bool TemplateManager::CopyEngineAssets(
    const std::filesystem::path& engineRoot,
    const std::filesystem::path& projectRoot,
    std::string& errorMessage)
{
    const std::filesystem::path src = engineRoot / "Assets";
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return true;
    }

    std::filesystem::recursive_directory_iterator iter(src, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (iter != end) {
        if (ec) {
            errorMessage = "Failed to enumerate engine assets.";
            return false;
        }

        const std::filesystem::directory_entry entry = *iter;
        iter.increment(ec);

        std::error_code entryEc;
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), src, entryEc);
        if (entryEc) {
            continue;
        }

        const std::filesystem::path dest = projectRoot / "Assets" / relative;

        if (entry.is_directory(entryEc)) {
            std::filesystem::create_directories(dest, ec);
            continue;
        }

        if (!entry.is_regular_file(entryEc)) {
            continue;
        }

        std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::skip_existing, ec);
    }

    return true;
}

bool TemplateManager::SyncCopiedScriptRegistrations(
    const std::filesystem::path& projectRoot,
    const ProjectNameInfo& nameInfo,
    std::string& errorMessage)
{
    const std::filesystem::path scriptsDir = projectRoot / "Assets" / "Scripts";
    std::error_code ec;
    if (!std::filesystem::exists(scriptsDir, ec)) {
        return true;
    }

    const std::filesystem::path dllCppPath =
        projectRoot / "Src" / (nameInfo.targetName + "ScriptsDll.cpp");
    const std::filesystem::path staticCppPath = projectRoot / "Src" / "GameMain.cpp";

    if (!std::filesystem::exists(dllCppPath, ec) || !std::filesystem::exists(staticCppPath, ec)) {
        errorMessage = "Failed to find generated script registration files.";
        return false;
    }

    const std::regex classRegex(R"(class\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*public\s+Script)");
    for (const auto& entry : std::filesystem::directory_iterator(scriptsDir, ec)) {
        if (ec) {
            errorMessage = "Failed to enumerate copied script headers.";
            return false;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".hpp") {
            continue;
        }

        const std::string headerText = ReadText(entry.path());
        std::smatch match;
        if (!std::regex_search(headerText, match, classRegex)) {
            continue;
        }

        const std::string className = match[1].str();
        const std::string includeLine = "#include \"Scripts/" + entry.path().filename().string() + "\"";
        const std::string dllEntry =
            "        { ::sandbox::" + className + "::TYPE_NAME,"
            " []() { return std::make_unique<::sandbox::" + className + ">(); } },";
        const std::string staticEntry = "FBZZ_REGISTER_SCRIPT(::sandbox::" + className + ")";

        // WHY: standard テンプレートはエンジン側 Assets/Scripts をコピーするため、
        //      ScriptCodeGen を経由しない既存スクリプトも DLL / Standalone の両方へ登録する必要がある。
        if (!InsertAfterMarker(dllCppPath, "@@FBZZ_SCRIPT_INCLUDES_BEGIN", includeLine) ||
            !InsertAfterMarker(dllCppPath, "@@FBZZ_SCRIPT_ENTRIES_BEGIN", dllEntry) ||
            !InsertAfterMarker(staticCppPath, "@@FBZZ_SCRIPT_INCLUDES_BEGIN", includeLine) ||
            !InsertAfterMarker(staticCppPath, "@@FBZZ_SCRIPT_ENTRIES_BEGIN", staticEntry)) {
            errorMessage = "Failed to update copied script registrations.";
            return false;
        }
    }

    return true;
}

ProjectNameInfo TemplateManager::MakeProjectNameInfo(const std::string& displayName)
{
    ProjectNameInfo info;
    info.name = displayName;

    bool nextUpper = true;
    bool lastWasUnderscore = false;
    for (char ch : displayName) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            const char lower = static_cast<char>(std::tolower(c));
            info.projectId.push_back(lower);
            info.cppNamespace.push_back(lower);
            info.targetName.push_back(nextUpper ? static_cast<char>(std::toupper(c)) : ch);
            nextUpper = false;
            lastWasUnderscore = false;
        } else {
            if (!info.projectId.empty() && !lastWasUnderscore) {
                info.projectId.push_back('_');
                info.cppNamespace.push_back('_');
                lastWasUnderscore = true;
            }
            nextUpper = true;
        }
    }

    while (!info.projectId.empty() && info.projectId.back() == '_') {
        info.projectId.pop_back();
        info.cppNamespace.pop_back();
    }

    return info;
}

bool TemplateManager::IsValidProjectNameInfo(const ProjectNameInfo& nameInfo)
{
    if (nameInfo.name.empty()
        || nameInfo.projectId.empty()
        || nameInfo.cppNamespace.empty()
        || nameInfo.targetName.empty()) {
        return false;
    }

    if (!std::islower(static_cast<unsigned char>(nameInfo.projectId.front()))) {
        return false;
    }

    return std::all_of(nameInfo.projectId.begin(), nameInfo.projectId.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    });
}

std::filesystem::path TemplateManager::ResolveTemplatesRoot()
{
    std::error_code ec;
    const std::filesystem::path runtimeRoot = util::GetExecutableDirectory() / "Templates";
    if (std::filesystem::exists(runtimeRoot, ec)) {
        return runtimeRoot;
    }

    return std::filesystem::current_path() / "Projects" / "GameHub" / "Templates";
}

std::filesystem::path TemplateManager::ResolveSourceTemplatesRoot()
{
    std::error_code ec;
    std::vector<std::filesystem::path> starts;
    starts.push_back(std::filesystem::current_path(ec));
    starts.push_back(util::GetExecutableDirectory());

    for (std::filesystem::path current : starts) {
        for (int i = 0; i < 8 && !current.empty(); ++i) {
            const std::filesystem::path candidate = current / "Projects" / "GameHub" / "Templates";
            if (Exists(candidate)) {
                return candidate;
            }
            current = current.parent_path();
        }
    }

    return {};
}

void TemplateManager::CollectTemplatesFromRoot(const std::filesystem::path& templatesRoot, std::vector<TemplateInfo>& templates)
{
    if (templatesRoot.empty() || !Exists(templatesRoot)) {
        return;
    }

    std::error_code ec;
    std::filesystem::directory_iterator iter(templatesRoot, ec);
    const std::filesystem::directory_iterator end;
    while (iter != end) {
        if (ec) {
            return;
        }

        const std::filesystem::directory_entry entry = *iter;
        iter.increment(ec);

        std::error_code entryEc;
        if (!entry.is_directory(entryEc)) {
            continue;
        }

        TemplateInfo info;
        if (!ReadTemplateInfo(entry.path(), info)) {
            continue;
        }

        const bool alreadyAdded = std::any_of(templates.begin(), templates.end(), [&info](const TemplateInfo& item) {
            return item.id == info.id;
        });
        if (!alreadyAdded) {
            templates.push_back(std::move(info));
        }
    }
}

bool TemplateManager::ReadTemplateInfo(const std::filesystem::path& rootPath, TemplateInfo& outInfo)
{
    const std::string text = ReadText(rootPath / "template.toml");
    if (text.empty()) {
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        return false;
    }

    auto& table = result.table();
    outInfo.id = table["template"]["id"].value_or(rootPath.filename().string());
    outInfo.displayName = table["template"]["display_name"].value_or(outInfo.id);
    outInfo.description = table["template"]["description"].value_or(std::string{});
    outInfo.rootPath = rootPath;
    return !outInfo.id.empty();
}

std::string TemplateManager::ApplyPlaceholders(
    const std::string& text,
    const ProjectNameInfo& nameInfo,
    const std::string& createdAt,
    const std::string& engineRoot)
{
    std::string result = text;
    result = ReplaceAll(result, "{{PROJECT_NAME}}", nameInfo.name);
    result = ReplaceAll(result, "{{PROJECT_ID}}", nameInfo.projectId);
    result = ReplaceAll(result, "{{CPP_NAMESPACE}}", nameInfo.cppNamespace);
    result = ReplaceAll(result, "{{TARGET_NAME}}", nameInfo.targetName);
    result = ReplaceAll(result, "{{ENGINE_VERSION}}", FBZZ_VERSION);
    result = ReplaceAll(result, "{{CREATED_AT}}", createdAt);
    result = ReplaceAll(result, "{{SETTINGS_PATH}}", "ProjectSettings/ProjectSettings.toml");
    result = ReplaceAll(result, "{{API_ROOT}}", "Include/");
    result = ReplaceAll(result, "{{PUBLIC_API_HEADER}}", "Include/" + nameInfo.targetName + "/ProjectAPI.hpp");
    result = ReplaceAll(result, "{{SCRIPT_ROOT}}", "Src/Scripts/");
    result = ReplaceAll(result, "{{LIBRARY_ROOT}}", "Lib/");
    result = ReplaceAll(result, "{{BINARY_ROOT}}", "Binaries/");
    result = ReplaceAll(result, "{{BUILD_ROOT}}", "Build/");
    result = ReplaceAll(result, "{{ENGINE_ROOT}}", engineRoot);
    return result;
}

} // namespace fbzz::hub
