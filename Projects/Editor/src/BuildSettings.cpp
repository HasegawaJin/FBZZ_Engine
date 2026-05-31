// FBZZ Engine
// BuildSettings.cpp | fbzz::editor
// BuildSettings の TOML シリアライズ実装
//
// WHAT: BuildSettings.toml を読み書きする。
//       [[scenes]] は toml++ の配列テーブル形式を使う。
#include <Editor/BuildSettings.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <filesystem>
#include <sstream>
#include <string>

namespace fbzz::editor {

static constexpr const char* BUILD_SETTINGS_FILE = "BuildSettings.toml";

bool BuildSettings::Save(const std::string& projectRoot) const
{
    toml::array scenesArr;
    for (const auto& scene : scenes) {
        toml::table entry;
        entry.insert("path",    scene.path);
        entry.insert("enabled", scene.enabled);
        scenesArr.push_back(std::move(entry));
    }

    toml::table buildTbl;
    buildTbl.insert("product_name", productName);
    buildTbl.insert("version",      version);
    buildTbl.insert("output_dir",   outputDirectory);
    buildTbl.insert("development",  developmentBuild);

    toml::table root;
    root.insert("build",  std::move(buildTbl));
    root.insert("scenes", std::move(scenesArr));

    std::ostringstream ss;
    ss << root;

    const std::string path = projectRoot + "/" + BUILD_SETTINGS_FILE;
    if (!util::FileSystem::WriteText(path, ss.str())) {
        FBZZ_LOG_ERROR("BuildSettings: 保存失敗: %s", path.c_str());
        return false;
    }
    return true;
}

bool BuildSettings::Load(const std::string& projectRoot)
{
    const std::string path = projectRoot + "/" + BUILD_SETTINGS_FILE;
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) {
        // ファイルがない場合はデフォルト値を維持する (初回は正常)
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("BuildSettings: パース失敗: %s", path.c_str());
        return false;
    }
    auto& tbl = result.table();

    if (auto* buildTbl = tbl["build"].as_table()) {
        productName     = (*buildTbl)["product_name"].value_or(productName);
        version         = (*buildTbl)["version"].value_or(version);
        outputDirectory = (*buildTbl)["output_dir"].value_or(outputDirectory);
        developmentBuild = (*buildTbl)["development"].value_or(developmentBuild);
    }

    scenes.clear();
    if (auto* scenesArr = tbl["scenes"].as_array()) {
        for (auto& elem : *scenesArr) {
            auto* entryTbl = elem.as_table();
            if (!entryTbl) continue;
            SceneEntry entry;
            entry.path    = (*entryTbl)["path"].value_or(std::string{});
            entry.enabled = (*entryTbl)["enabled"].value_or(true);
            if (!entry.path.empty())
                scenes.push_back(std::move(entry));
        }
    }

    return true;
}

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
    std::filesystem::path out(outputDirectory);
    if (out.is_absolute()) return out;
    return std::filesystem::path(projectRoot) / out;
}

} // namespace fbzz::editor
