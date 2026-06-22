// FBZZ Engine
// MigrationManager.hpp | fbzz::hub
// Standalone project migration support
#pragma once

#include <filesystem>
#include <string>

namespace fbzz::hub {

class MigrationManager {
public:
    [[nodiscard]] static bool MigrateProject(const std::string& projectPath, std::string& errorMessage);

private:
    static bool BackupProjectFiles(const std::filesystem::path& projectRoot, std::string& errorMessage);
    static bool EnsureGeneratedLayout(const std::filesystem::path& projectRoot, std::string& errorMessage);
    static bool PatchCMakeLists(const std::filesystem::path& projectRoot, std::string& errorMessage);
    static bool PatchCMakePresets(const std::filesystem::path& projectRoot, std::string& errorMessage);
    static bool UpdateProjectVersion(const std::filesystem::path& projectRoot, std::string& errorMessage);
};

} // namespace fbzz::hub
