/// @file    BuildSettings.cpp
/// @brief   BuildSettings のパス解決と旧形式ファイルの移行読み込み。
/// @author  Hasegawa Jin
/// @date    2026-05-31
#include <Editor/BuildSettings.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace fbzz::editor {

namespace {
// 旧形式でプロジェクトルートに置かれていた設定ファイル。
constexpr const char* LEGACY_BUILD_SETTINGS_FILE = "BuildSettings.toml";

// a が b と同じか、b の祖先か。実体の有無は問わず字面だけで判定する。
bool ContainsOrEquals(const std::filesystem::path& a, const std::filesystem::path& b)
{
    const std::filesystem::path rel =
        b.lexically_normal().lexically_relative(a.lexically_normal());
    if (rel.empty()) return false;   // 別ドライブ等、そもそも関係が無い
    return *rel.begin() != "..";
}

// 出力先と重なってはいけないプロジェクトのデータディレクトリ。
constexpr const char* kProtectedDirs[] = {
    "Assets", "Library", "ProjectSettings", "Src", "Include", "Binaries", "Build",
};
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

bool BuildSettings::ValidateOutputPath(const std::string& projectRoot, std::string& outReason) const
{
    outReason.clear();

    if (util::StringUtils::Trim(outputDirectory).empty()) {
        outReason = "Output Directory is empty — it would resolve to the project root.";
        return false;
    }
    if (projectRoot.empty()) {
        outReason = "No project is open.";
        return false;
    }

    const std::filesystem::path root =
        util::FileSystem::PathFromUtf8(projectRoot).lexically_normal();
    const std::filesystem::path out = ResolveOutputPath(projectRoot).lexically_normal();

    if (!out.has_relative_path()) {
        outReason = "Output Directory is a drive root: "
                  + util::FileSystem::PathToUtf8(out);
        return false;
    }
    if (ContainsOrEquals(out, root)) {
        outReason = "Output Directory contains the project itself ("
                  + util::FileSystem::PathToUtf8(out) + ") — committing the build would delete it.";
        return false;
    }
    for (const char* name : kProtectedDirs) {
        const std::filesystem::path dir = (root / name).lexically_normal();
        if (!ContainsOrEquals(dir, out) && !ContainsOrEquals(out, dir)) continue;
        outReason = std::string("Output Directory overlaps the project's ") + name + "/ folder.";
        return false;
    }

    // 既に中身があるフォルダは、前回の配布物 (game.manifest.toml を持つ) でない限り拒否する。
    // WHY: Browse... でデスクトップや素材置き場を選ぶと、コミット時の remove_all が
    //      そのフォルダを中身ごと消す。空フォルダと再ビルドだけを通す。
    std::error_code ec;
    if (std::filesystem::is_directory(out, ec)) {
        const bool isEmpty =
            std::filesystem::directory_iterator(out, ec) == std::filesystem::directory_iterator{};
        if (!ec && !isEmpty && !util::FileSystem::Exists(out / "game.manifest.toml")) {
            outReason = "Output Directory is not empty and is not a previous build "
                        "(no game.manifest.toml): "
                      + util::FileSystem::PathToUtf8(out)
                      + " — the build deletes it entirely. Pick an empty or dedicated folder.";
            return false;
        }
    }

    return true;
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
