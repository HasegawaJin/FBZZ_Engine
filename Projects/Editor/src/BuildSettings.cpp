// FBZZ Engine
// BuildSettings.cpp | fbzz::editor
// BuildSettings のパス解決と旧形式ファイルの移行読み込み
#include <Editor/BuildSettings.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>

#include <filesystem>
#include <string>

namespace fbzz::editor {

namespace {
// 旧形式でプロジェクトルートに置かれていた設定ファイル。
constexpr const char* LEGACY_BUILD_SETTINGS_FILE = "BuildSettings.toml";
} // namespace

std::vector<std::string> BuildSettings::EnabledScenes() const
{
    std::vector<std::string> result;
    for (const auto& scene : scenes)
        if (scene.enabled)
            result.push_back(scene.path);
    return result;
}

std::filesystem::path BuildSettings::ResolveOutputPath(const std::string& projectRoot) const
{
    // WHY: 絶対パスと相対パスの両方を受け付けるが、保存時は相対パスを優先する。
    //      プロジェクトを別 PC に持っていっても動くように。
    std::filesystem::path out = util::FileSystem::PathFromUtf8(outputDirectory);
    if (out.is_absolute()) return out;
    return util::FileSystem::PathFromUtf8(projectRoot) / out;
}

bool BuildSettings::LoadLegacyFile(const std::string& projectRoot, BuildSettings& out)
{
    if (projectRoot.empty()) return false;

    const std::string path = projectRoot + "/" + LEGACY_BUILD_SETTINGS_FILE;
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return false;

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("BuildSettings: failed to parse legacy file: %s", path.c_str());
        return false;
    }

    auto& tbl = result.table();
    if (auto* buildTbl = tbl["build"].as_table()) {
        out.productName      = (*buildTbl)["product_name"].value_or(out.productName);
        out.version          = (*buildTbl)["version"].value_or(out.version);
        out.outputDirectory  = (*buildTbl)["output_dir"].value_or(out.outputDirectory);
        out.developmentBuild = (*buildTbl)["development"].value_or(out.developmentBuild);
    }

    out.scenes.clear();
    if (auto* scenesArr = tbl["scenes"].as_array()) {
        for (auto& elem : *scenesArr) {
            auto* entryTbl = elem.as_table();
            if (!entryTbl) continue;
            SceneEntry entry;
            entry.path    = (*entryTbl)["path"].value_or(std::string{});
            entry.enabled = (*entryTbl)["enabled"].value_or(true);
            if (!entry.path.empty())
                out.scenes.push_back(std::move(entry));
        }
    }

    FBZZ_LOG_INFO("BuildSettings: migrated legacy %s into editor_settings.toml",
                  LEGACY_BUILD_SETTINGS_FILE);
    return true;
}

} // namespace fbzz::editor
