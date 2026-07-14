// FBZZ Engine
// MigrationManager.cpp | fbzz::hub
// Standalone project migration support
#include "MigrationManager.hpp"
#include <Engine/Util/FileSystem.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

std::string TimestampForPath()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);

    std::tm local{};
    localtime_s(&local, &time);

    std::ostringstream ss;
    ss << std::put_time(&local, "%Y-%m-%d_%H%M%S");
    return ss.str();
}

bool CopyIfExists(const std::filesystem::path& from, const std::filesystem::path& to, std::string& errorMessage)
{
    std::error_code ec;
    if (!engine_util::FileSystem::Exists(from)) {
        return true;
    }

    if (!engine_util::FileSystem::EnsureParentDirectory(to)) {
        errorMessage = "Failed to create backup directory.";
        return false;
    }

    if (std::filesystem::is_directory(from, ec)) {
        if (!engine_util::FileSystem::CopyDirectoryRecursive(from, to)) {
            errorMessage = "Failed to copy backup files.";
            return false;
        }
    } else {
        if (!engine_util::FileSystem::CopyFile(from, to)) {
            errorMessage = "Failed to copy backup files.";
            return false;
        }
    }
    return true;
}

bool EnsureTextFile(const std::filesystem::path& path, const std::string& text, std::string& errorMessage)
{
    if (engine_util::FileSystem::Exists(path)) {
        return true;
    }

    if (!engine_util::FileSystem::WriteText(path, text)) {
        errorMessage = "Failed to create a generated project file.";
        return false;
    }
    return true;
}

std::filesystem::path FindStandardTemplateFile(const std::filesystem::path& relative)
{
    const std::filesystem::path runtime =
        engine_util::FileSystem::GetExecutableDirectory() / "Templates" / "standard" / relative;
    if (engine_util::FileSystem::Exists(runtime)) return runtime;

    std::filesystem::path current = engine_util::FileSystem::GetCurrentDirectory();
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        const std::filesystem::path source =
            current / "Projects" / "GameHub" / "Templates" / "standard" / relative;
        if (engine_util::FileSystem::Exists(source)) return source;
        current = current.parent_path();
    }
    return {};
}

size_t FindTomlKey(const std::string& text, const std::string& key)
{
    size_t keyPos = 0;
    while ((keyPos = text.find(key, keyPos)) != std::string::npos) {
        const bool lineStart = keyPos == 0 || text[keyPos - 1] == '\n' || text[keyPos - 1] == '\r';
        const size_t after = keyPos + key.size();
        const bool keyEnd = after == text.size() || text[after] == ' ' || text[after] == '\t' || text[after] == '=';
        if (lineStart && keyEnd) return keyPos;
        keyPos = after;
    }
    return std::string::npos;
}

std::string ReadProjectString(const std::string& text, const std::string& key)
{
    const size_t keyPos = FindTomlKey(text, key);
    if (keyPos == std::string::npos) return {};
    const size_t equalsPos = text.find('=', keyPos + key.size());
    const size_t firstQuote = equalsPos == std::string::npos ? std::string::npos : text.find('"', equalsPos);
    const size_t secondQuote = firstQuote == std::string::npos ? std::string::npos : text.find('"', firstQuote + 1);
    if (firstQuote == std::string::npos || secondQuote == std::string::npos) return {};
    return text.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

void ReplaceAll(std::string& text, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool ReplaceTomlString(std::string& text, const std::string& key, const std::string& value)
{
    const size_t keyPos = FindTomlKey(text, key);
    if (keyPos == std::string::npos) return false;
    const size_t equalsPos = text.find('=', keyPos + key.size());
    const size_t lineEnd = text.find_first_of("\r\n", keyPos);
    if (equalsPos == std::string::npos || (lineEnd != std::string::npos && equalsPos > lineEnd)) return false;
    const size_t valueEnd = lineEnd == std::string::npos ? text.size() : lineEnd;
    text.replace(equalsPos + 1, valueEnd - equalsPos - 1, " \"" + value + "\"");
    return true;
}

std::string CurrentSdkRoot()
{
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, "FBZZ_SDK_ROOT");
    const std::string result = value && length > 1
        ? engine_util::FileSystem::NormalizePathSeparators(value)
        : "";
    free(value);
    return result;
}

} // namespace

bool MigrationManager::MigrateProject(const std::string& projectPath, std::string& errorMessage)
{
    const std::filesystem::path projectRoot = engine_util::FileSystem::PathFromUtf8(projectPath);
    if (!engine_util::FileSystem::Exists(projectRoot / ".fbzz_proj")) {
        errorMessage = "The project file was not found.";
        return false;
    }

    if (!BackupProjectFiles(projectRoot, errorMessage)) {
        return false;
    }

    if (!EnsureGeneratedLayout(projectRoot, errorMessage)) {
        return false;
    }

    return UpdateProjectVersion(projectRoot, errorMessage);
}

bool MigrationManager::BackupProjectFiles(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path backupRoot = projectRoot / "Backups" / (TimestampForPath() + "_before_" + FBZZ_VERSION);

    const std::vector<std::filesystem::path> targets = {
        ".fbzz_proj",
        "ProjectSettings",
        "CMakeLists.txt",
        "CMakePresets.json",
        "Include",
        "Src/GameMain.cpp"
    };

    for (const auto& target : targets) {
        if (!CopyIfExists(projectRoot / target, backupRoot / target, errorMessage)) {
            return false;
        }
    }

    return true;
}

bool MigrationManager::EnsureGeneratedLayout(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    if (!engine_util::FileSystem::EnsureDirectory(projectRoot / "Lib") ||
        !engine_util::FileSystem::EnsureDirectory(projectRoot / "Binaries") ||
        !engine_util::FileSystem::EnsureDirectory(projectRoot / "Build") ||
        !engine_util::FileSystem::EnsureDirectory(projectRoot / "ProjectSettings")) {
        errorMessage = "Failed to create generated project directories.";
        return false;
    }

    if (!EnsureTextFile(projectRoot / "Binaries/.gitignore", "*\n!.gitignore\n", errorMessage)) {
        return false;
    }

    if (!EnsureTextFile(projectRoot / "Build/.gitignore", "*\n!.gitignore\n", errorMessage)) {
        return false;
    }

    if (!EnsureTextFile(
        projectRoot / "ProjectSettings/ProjectSettings.toml",
        "[project]\ndefault_scene = \"Assets/Scenes/Main.scene\"\n\n[runtime]\nstart_scene = \"Assets/Scenes/Main.scene\"\n",
        errorMessage)) {
        return false;
    }

    if (!PatchCMakeLists(projectRoot, errorMessage))
        return false;

    return PatchCMakePresets(projectRoot, errorMessage);
}

bool MigrationManager::PatchCMakeLists(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path cmakePath = projectRoot / "CMakeLists.txt";
    if (!engine_util::FileSystem::Exists(cmakePath))
        return true;

    std::string text;
    if (!engine_util::FileSystem::ReadText(cmakePath, text)) {
        errorMessage = "Failed to read CMakeLists.txt.";
        return false;
    }

    if (text.find("find_package(FBZZ") != std::string::npos)
        return true;
    if (text.find("FBZZ_ENGINE_ROOT") == std::string::npos ||
        text.find("add_subdirectory") == std::string::npos) {
        errorMessage = "CMakeLists.txt is customized and cannot be migrated automatically. The backup was preserved.";
        return false;
    }

    const std::string sdkRoot = CurrentSdkRoot();
    if (sdkRoot.empty() || !engine_util::FileSystem::Exists(
            engine_util::FileSystem::PathFromUtf8(sdkRoot) / "fbzz-sdk.toml")) {
        errorMessage = "A valid FBZZ_SDK_ROOT is required before migration.";
        return false;
    }

    const std::filesystem::path templatePath = FindStandardTemplateFile("CMakeLists.txt");
    std::string projectText;
    std::string migrated;
    engine_util::FileSystem::ReadText(projectRoot / ".fbzz_proj", projectText);
    if (templatePath.empty()) {
        errorMessage = "Failed to resolve the shared-SDK CMake migration template.";
        return false;
    }
    engine_util::FileSystem::ReadText(templatePath, migrated);
    const std::string targetName = ReadProjectString(projectText, "target_name");
    if (migrated.empty() || targetName.empty()) {
        errorMessage = "Failed to read target_name or the shared-SDK CMake migration template.";
        return false;
    }
    ReplaceAll(migrated, "{{TARGET_NAME}}", targetName);
    ReplaceAll(migrated, "{{ENGINE_VERSION}}", FBZZ_VERSION);

    if (!engine_util::FileSystem::WriteText(cmakePath, migrated)) {
        errorMessage = "Failed to write patched CMakeLists.txt.";
        return false;
    }

    // WHY: 古い Build/VS は Engine ソースの add_subdirectory をキャッシュしているため削除する。
    //      次回 configure で IMPORTED target だけを持つ solution へ確実に切り替える。
    const std::filesystem::path buildVS = projectRoot / "Build" / "VS";
    if (engine_util::FileSystem::Exists(buildVS))
        engine_util::FileSystem::RemoveAll(buildVS);

    return true;
}

bool MigrationManager::PatchCMakePresets(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path presetsPath = projectRoot / "CMakePresets.json";
    if (!engine_util::FileSystem::Exists(presetsPath))
        return true;

    std::string text;
    if (!engine_util::FileSystem::ReadText(presetsPath, text)) {
        errorMessage = "Failed to read CMakePresets.json.";
        return false;
    }

    if (text.find("FBZZ_SDK_ROOT") != std::string::npos) return true;
    if (text.find("FBZZ_ENGINE_ROOT") == std::string::npos) {
        errorMessage = "CMakePresets.json is customized and cannot be migrated automatically.";
        return false;
    }

    const std::filesystem::path templatePath = FindStandardTemplateFile("CMakePresets.json");
    if (templatePath.empty() || !engine_util::FileSystem::ReadText(templatePath, text)) {
        errorMessage = "Failed to resolve the shared-SDK preset migration template.";
        return false;
    }

    if (!engine_util::FileSystem::WriteText(presetsPath, text)) {
        errorMessage = "Failed to write patched CMakePresets.json.";
        return false;
    }

    // WHY: CMakePresets.json の変更は既存の Build/VS には反映されないため削除して再生成させる。
    const std::filesystem::path buildVS = projectRoot / "Build" / "VS";
    if (engine_util::FileSystem::Exists(buildVS))
        engine_util::FileSystem::RemoveAll(buildVS);

    return true;
}

bool MigrationManager::UpdateProjectVersion(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path projectFile = projectRoot / ".fbzz_proj";
    std::string text;
    engine_util::FileSystem::ReadText(projectFile, text);
    if (text.empty()) {
        errorMessage = "Failed to read .fbzz_proj.";
        return false;
    }

    if (!ReplaceTomlString(text, "engine_version", FBZZ_VERSION)) {
        errorMessage = "project.engine_version was not found.";
        return false;
    }

    const std::string sdkRoot = CurrentSdkRoot();
    if (sdkRoot.empty()) {
        errorMessage = "FBZZ_SDK_ROOT is not set; select a versioned SDK in GameHub Settings.";
        return false;
    }
    if (ReplaceTomlString(text, "sdk_root", sdkRoot)) {
        // すでに新形式。
    } else if (ReplaceTomlString(text, "root", sdkRoot)) {
        const size_t rootKey = FindTomlKey(text, "root");
        text.replace(rootKey, std::string("root").size(), "sdk_root");
    } else {
        text += "\n[engine]\nsdk_root = \"" + sdkRoot + "\"\n";
    }

    if (!engine_util::FileSystem::WriteText(projectFile, text)) {
        errorMessage = "Failed to update .fbzz_proj.";
        return false;
    }

    return true;
}

} // namespace fbzz::hub
