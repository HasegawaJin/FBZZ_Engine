// FBZZ Engine
// TemplateManager.cpp | fbzz::hub
// Project template discovery and instantiation
#include "TemplateManager.hpp"
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

std::string ReadText(const std::filesystem::path& path)
{
    std::string text;
    engine_util::FileSystem::ReadText(path, text);
    return text;
}

bool WriteText(const std::filesystem::path& path, const std::string& text)
{
    return engine_util::FileSystem::WriteText(path, text);
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
    const std::string extension = engine_util::FileSystem::PathToUtf8(path.extension());
    return extension == ".tmpl"
        || extension == ".fbzz_proj"
        || extension == ".txt"
        || extension == ".toml"
        || extension == ".json"
        || extension == ".hpp"
        || extension == ".cpp"
        || extension == ".h"
        || extension == ".c"
        || extension == ".inl"  // ScriptList.inl 等のテンプレートファイルを対象に含める
        || extension == ".md"
        || extension == ".gitignore"
        || extension.empty();
}

bool Exists(const std::filesystem::path& path)
{
    return engine_util::FileSystem::Exists(path);
}

bool ReplaceMarkerBlock(const std::filesystem::path& path,
                        const std::string& beginMarker,
                        const std::string& endMarker,
                        const std::vector<std::string>& generatedLines)
{
    std::string text = ReadText(path);
    if (text.empty()) return false;

    const std::string beginText = "// " + beginMarker;
    const std::string endText = "// " + endMarker;
    const size_t beginMarkerPos = text.find(beginText);
    if (beginMarkerPos == std::string::npos) return false;
    const size_t beginLineEnd = text.find('\n', beginMarkerPos);
    if (beginLineEnd == std::string::npos) return false;

    const size_t generatedBegin = beginLineEnd + 1;
    const size_t endMarkerPos = text.find(endText, generatedBegin);
    if (endMarkerPos == std::string::npos) return false;

    std::string generatedText;
    for (const auto& line : generatedLines)
        generatedText += line + "\n";

    const std::string next = text.substr(0, generatedBegin) +
                             generatedText +
                             text.substr(endMarkerPos);
    if (next == text) return true;
    return WriteText(path, next);
}

std::string ParseNamespaceForScript(const std::string& headerText)
{
    std::istringstream input(headerText);
    std::string line;
    while (std::getline(input, line)) {
        const size_t namespacePos = line.find("namespace ");
        if (namespacePos == std::string::npos) continue;
        const size_t begin = namespacePos + std::string_view("namespace ").size();
        size_t end = line.find_first_of(" {", begin);
        if (end == std::string::npos) end = line.size();
        if (end > begin) return line.substr(begin, end - begin);
    }
    return "sandbox";
}

} // namespace

void TemplateManager::Refresh()
{
    m_templates.clear();

    CollectTemplatesFromRoot(ResolveSourceTemplatesRoot(), m_templates);
    CollectTemplatesFromRoot(engine_util::FileSystem::GetExecutableDirectory() / "Templates", m_templates);

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
    if (engine_util::FileSystem::Exists(projectRoot)) {
        errorMessage = "Destination project folder already exists.";
        return false;
    }

    if (!engine_util::FileSystem::EnsureDirectory(projectRoot)) {
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
            outputRelative /= ApplyPlaceholders(engine_util::FileSystem::PathToUtf8(part), nameInfo, createdAt, engineRoot);
        }

        std::filesystem::path outputPath = projectRoot / outputRelative;
        std::error_code entryEc;
        if (entry.is_directory(entryEc)) {
            if (!engine_util::FileSystem::EnsureDirectory(outputPath)) {
                errorMessage = "Failed to create a project directory.";
                return false;
            }
            continue;
        }

        entryEc.clear();
        if (!entry.is_regular_file(entryEc)) {
            continue;
        }

        std::string outputFileName = engine_util::FileSystem::PathToUtf8(outputPath.filename());
        if (engine_util::StringUtils::EndsWith(outputFileName, ".tmpl")) {
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
            if (!engine_util::FileSystem::CopyFile(entry.path(), outputPath, false)) {
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
    if (!engine_util::FileSystem::Exists(src)) {
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
            engine_util::FileSystem::EnsureDirectory(dest);
            continue;
        }

        if (!entry.is_regular_file(entryEc)) {
            continue;
        }

        if (!engine_util::FileSystem::Exists(dest)) {
            engine_util::FileSystem::CopyFile(entry.path(), dest, false);
        }
    }

    return true;
}

bool TemplateManager::SyncCopiedScriptRegistrations(
    const std::filesystem::path& projectRoot,
    const ProjectNameInfo& nameInfo,
    std::string& errorMessage)
{
    const std::filesystem::path assetsDir = projectRoot / "Assets";
    const std::filesystem::path scriptsDir = assetsDir / "Scripts";
    std::error_code ec;
    if (!engine_util::FileSystem::Exists(scriptsDir)) {
        return true;
    }

    const std::filesystem::path dllCppPath =
        projectRoot / "Src" / (nameInfo.targetName + "ScriptsDll.cpp");
    const std::filesystem::path staticCppPath = projectRoot / "Src" / "GameMain.cpp";

    if (!engine_util::FileSystem::Exists(dllCppPath) || !engine_util::FileSystem::Exists(staticCppPath)) {
        errorMessage = "Failed to find generated script registration files.";
        return false;
    }

    const std::filesystem::path scriptListPath = scriptsDir / "ScriptList.inl";
    if (!Exists(scriptListPath)) {
        errorMessage = "Failed to find generated ScriptList.inl.";
        return false;
    }

    std::vector<std::pair<std::string, std::string>> scripts;
    const std::regex scriptRegex(R"(FBZZ_SCRIPT\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(assetsDir, ec)) {
        if (ec) {
            errorMessage = "Failed to enumerate copied script headers.";
            return false;
        }
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".hpp") {
            continue;
        }

        if (entry.path().filename() == "ScriptList.inl" ||
            engine_util::FileSystem::PathToUtf8(entry.path().filename()).ends_with(".generated.hpp")) {
            continue;
        }

        const std::string headerText = ReadText(entry.path());
        const std::string namespaceName = ParseNamespaceForScript(headerText);
        const std::string includePath = engine_util::FileSystem::NormalizePathSeparators(
            engine_util::FileSystem::PathToUtf8(engine_util::FileSystem::RelativePath(entry.path(), assetsDir)));
        for (std::sregex_iterator it(headerText.begin(), headerText.end(), scriptRegex), end; it != end; ++it) {
            scripts.emplace_back(
                includePath,
                "FBZZ_SCRIPT_ENTRY(" + namespaceName + ", " + (*it)[1].str() + ")");
        }
    }

    std::sort(scripts.begin(), scripts.end());

    std::vector<std::string> includeLines;
    std::vector<std::string> entryLines;
    std::string lastHeader;
    for (const auto& [headerName, entryLine] : scripts) {
        if (headerName != lastHeader) {
            includeLines.push_back("#include \"" + headerName + "\"");
            lastHeader = headerName;
        }
        entryLines.push_back(entryLine);
    }

    // WHY: standard/empty テンプレートはエンジン側 Assets をコピーするため、
    //      ScriptCodeGen を経由しない既存スクリプトも DLL / Standalone の両方へ登録する必要がある。
    if (!ReplaceMarkerBlock(dllCppPath, "@@FBZZ_SCRIPT_INCLUDES_BEGIN", "@@FBZZ_SCRIPT_INCLUDES_END", includeLines) ||
        !ReplaceMarkerBlock(staticCppPath, "@@FBZZ_SCRIPT_INCLUDES_BEGIN", "@@FBZZ_SCRIPT_INCLUDES_END", includeLines) ||
        !ReplaceMarkerBlock(scriptListPath, "@@FBZZ_SCRIPT_ENTRIES_BEGIN", "@@FBZZ_SCRIPT_ENTRIES_END", entryLines)) {
        errorMessage = "Failed to update copied script registrations.";
        return false;
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
    const std::filesystem::path runtimeRoot = engine_util::FileSystem::GetExecutableDirectory() / "Templates";
    if (engine_util::FileSystem::Exists(runtimeRoot)) {
        return runtimeRoot;
    }

    return engine_util::FileSystem::GetCurrentDirectory() / "Projects" / "GameHub" / "Templates";
}

std::filesystem::path TemplateManager::ResolveSourceTemplatesRoot()
{
    std::vector<std::filesystem::path> starts;
    starts.push_back(engine_util::FileSystem::GetCurrentDirectory());
    starts.push_back(engine_util::FileSystem::GetExecutableDirectory());

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
    outInfo.id = table["template"]["id"].value_or(engine_util::FileSystem::PathToUtf8(rootPath.filename()));
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
    // WHY: 外部プロジェクトは Engine ソースではなく版別 SDK を参照する。
    //      引数名は API 互換のため維持し、テンプレート上の新しい語彙へ割り当てる。
    result = ReplaceAll(result, "{{SDK_ROOT}}", engineRoot);
    result = ReplaceAll(result, "{{ENGINE_ROOT}}", engineRoot); // 旧カスタムテンプレート互換
    return result;
}

} // namespace fbzz::hub
