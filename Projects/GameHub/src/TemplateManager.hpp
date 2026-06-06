// FBZZ Engine
// TemplateManager.hpp | fbzz::hub
// Project template discovery and instantiation
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::hub {

struct TemplateInfo {
    std::string id;
    std::string displayName;
    std::string description;
    std::filesystem::path rootPath;
};

struct ProjectNameInfo {
    std::string name;
    std::string projectId;
    std::string cppNamespace;
    std::string targetName;
};

class TemplateManager {
public:
    void Refresh();

    [[nodiscard]] const std::vector<TemplateInfo>& GetTemplates() const { return m_templates; }
    [[nodiscard]] bool Instantiate(
        const TemplateInfo& templateInfo,
        const std::filesystem::path& destinationRoot,
        const ProjectNameInfo& nameInfo,
        const std::string& createdAt,
        const std::string& engineRoot,
        std::string& errorMessage) const;

    static ProjectNameInfo MakeProjectNameInfo(const std::string& displayName);
    static bool IsValidProjectNameInfo(const ProjectNameInfo& nameInfo);

private:
    static std::filesystem::path ResolveTemplatesRoot();
    static std::filesystem::path ResolveSourceTemplatesRoot();
    static void CollectTemplatesFromRoot(const std::filesystem::path& templatesRoot, std::vector<TemplateInfo>& templates);
    static bool ReadTemplateInfo(const std::filesystem::path& rootPath, TemplateInfo& outInfo);
    static std::string ApplyPlaceholders(
        const std::string& text,
        const ProjectNameInfo& nameInfo,
        const std::string& createdAt,
        const std::string& engineRoot);
    static bool CopyEngineAssets(
        const std::filesystem::path& engineRoot,
        const std::filesystem::path& projectRoot,
        std::string& errorMessage);
    static bool SyncCopiedScriptRegistrations(
        const std::filesystem::path& projectRoot,
        const ProjectNameInfo& nameInfo,
        std::string& errorMessage);

    std::vector<TemplateInfo> m_templates;
};

} // namespace fbzz::hub
