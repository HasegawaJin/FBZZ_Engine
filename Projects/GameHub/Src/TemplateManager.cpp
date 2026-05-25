// FBZZ Engine
// TemplateManager.cpp | fbzz::hub
// Project template discovery and instantiation
#include "TemplateManager.hpp"

#include <Windows.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
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
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    file << text;
    return static_cast<bool>(file);
}

std::filesystem::path GetExecutableDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
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

} // namespace

void TemplateManager::Refresh()
{
    m_templates.clear();

    const std::filesystem::path templatesRoot = ResolveTemplatesRoot();
    std::error_code ec;
    if (!std::filesystem::exists(templatesRoot, ec)) {
        return;
    }

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
        if (ReadTemplateInfo(entry.path(), info)) {
            m_templates.push_back(std::move(info));
        }
    }

    std::sort(m_templates.begin(), m_templates.end(), [](const TemplateInfo& a, const TemplateInfo& b) {
        return a.displayName < b.displayName;
    });
}

bool TemplateManager::Instantiate(
    const TemplateInfo& templateInfo,
    const std::filesystem::path& destinationRoot,
    const ProjectNameInfo& nameInfo,
    const std::string& createdAt,
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
            outputRelative /= ApplyPlaceholders(part.string(), nameInfo, createdAt);
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
            if (!WriteText(outputPath, ApplyPlaceholders(text, nameInfo, createdAt))) {
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
    const std::filesystem::path runtimeRoot = GetExecutableDirectory() / "Templates";
    if (std::filesystem::exists(runtimeRoot, ec)) {
        return runtimeRoot;
    }

    return std::filesystem::current_path() / "Projects" / "GameHub" / "Templates";
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
    const std::string& createdAt)
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
    result = ReplaceAll(result, "{{ENGINE_ROOT}}", "");
    return result;
}

} // namespace fbzz::hub
