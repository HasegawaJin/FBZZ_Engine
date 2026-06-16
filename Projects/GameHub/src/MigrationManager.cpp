// FBZZ Engine
// MigrationManager.cpp | fbzz::hub
// Standalone project migration support
#include "MigrationManager.hpp"
#include <Engine/Util/FileSystem.hpp>

#include <chrono>
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

    return EnsureTextFile(
        projectRoot / "ProjectSettings/ProjectSettings.toml",
        "[project]\ndefault_scene = \"Assets/Scenes/Main.scene\"\n\n[runtime]\nstart_scene = \"Assets/Scenes/Main.scene\"\n",
        errorMessage);
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

    const std::string key = "engine_version";
    const size_t keyPos = text.find(key);
    if (keyPos == std::string::npos) {
        errorMessage = "project.engine_version was not found.";
        return false;
    }

    const size_t equalsPos = text.find('=', keyPos);
    const size_t lineEnd = text.find_first_of("\r\n", keyPos);
    if (equalsPos == std::string::npos || (lineEnd != std::string::npos && equalsPos > lineEnd)) {
        errorMessage = "project.engine_version is invalid.";
        return false;
    }

    const size_t valueEnd = lineEnd == std::string::npos ? text.size() : lineEnd;
    text.replace(equalsPos + 1, valueEnd - equalsPos - 1, " \"" FBZZ_VERSION "\"");

    if (!engine_util::FileSystem::WriteText(projectFile, text)) {
        errorMessage = "Failed to update .fbzz_proj.";
        return false;
    }

    return true;
}

} // namespace fbzz::hub
