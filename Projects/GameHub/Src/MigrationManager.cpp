// FBZZ Engine
// MigrationManager.cpp | fbzz::hub
// Standalone project migration support
#include "MigrationManager.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

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
    if (!std::filesystem::exists(from, ec)) {
        return true;
    }

    std::filesystem::create_directories(to.parent_path(), ec);
    if (ec) {
        errorMessage = "Failed to create backup directory.";
        return false;
    }

    if (std::filesystem::is_directory(from, ec)) {
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
    } else {
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    }

    if (ec) {
        errorMessage = "Failed to copy backup files.";
        return false;
    }
    return true;
}

bool EnsureTextFile(const std::filesystem::path& path, const std::string& text, std::string& errorMessage)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return true;
    }

    if (!WriteText(path, text)) {
        errorMessage = "Failed to create a generated project file.";
        return false;
    }
    return true;
}

} // namespace

bool MigrationManager::MigrateProject(const std::string& projectPath, std::string& errorMessage)
{
    const std::filesystem::path projectRoot(projectPath);
    std::error_code ec;
    if (!std::filesystem::exists(projectRoot / ".fbzz_proj", ec)) {
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
    std::error_code ec;
    std::filesystem::create_directories(projectRoot / "Lib", ec);
    std::filesystem::create_directories(projectRoot / "Binaries", ec);
    std::filesystem::create_directories(projectRoot / "Build", ec);
    std::filesystem::create_directories(projectRoot / "ProjectSettings", ec);
    if (ec) {
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
        "[project]\ndefault_scene = \"Assets/Scenes/Main.fbzz\"\n",
        errorMessage);
}

bool MigrationManager::UpdateProjectVersion(const std::filesystem::path& projectRoot, std::string& errorMessage)
{
    const std::filesystem::path projectFile = projectRoot / ".fbzz_proj";
    std::string text = ReadText(projectFile);
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

    if (!WriteText(projectFile, text)) {
        errorMessage = "Failed to update .fbzz_proj.";
        return false;
    }

    return true;
}

} // namespace fbzz::hub
