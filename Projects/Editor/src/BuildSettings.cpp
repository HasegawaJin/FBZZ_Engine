/// @file    BuildSettings.cpp
/// @brief   BuildSettings のパス解決と旧形式ファイルの移行読み込み。
/// @author  Hasegawa Jin
/// @date    2026-05-31
#include <Editor/BuildSettings.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace fbzz::editor {

namespace {
/// @note 旧形式でプロジェクトルートに置かれていた設定ファイル。
constexpr const char* LEGACY_BUILD_SETTINGS_FILE = "BuildSettings.toml";

/// @note Windows の ordinal ignore-case でパス構成要素を比較し、名前の前方一致を包含扱いしない。
/// @see https://learn.microsoft.com/windows/win32/api/stringapiset/nf-stringapiset-comparestringordinal CompareStringOrdinal の非言語的比較。
bool ContainsOrEquals(const std::filesystem::path& a, const std::filesystem::path& b)
{
    auto left = a.begin();
    auto right = b.begin();
    for (; left != a.end(); ++left, ++right) {
        if (right == b.end() || CompareStringOrdinal(left->c_str(), -1, right->c_str(), -1, TRUE) != CSTR_EQUAL)
            return false;
    }
    return true;
}

/// @note 既存のリンク先と未作成の末尾を正規化し、検査不能を許可へ変換しない。
/// @see https://learn.microsoft.com/cpp/standard-library/filesystem-functions#weakly_canonical 未作成パスの既存部分を正規化する契約。
bool CanonicalPath(const std::filesystem::path& input, std::filesystem::path& result, std::string& reason)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(input, error);
    if (!error) result = std::filesystem::weakly_canonical(absolute, error).lexically_normal();
    if (error || result.empty()) {
        reason = "Output path could not be resolved: " + error.message();
        return false;
    }
    while (result.has_relative_path() && result.filename().empty()) result = result.parent_path();
    return true;
}

/// @note 出力先と重なってはいけないプロジェクトのデータディレクトリ。
constexpr const char* kProtectedDirs[] = {
    "Assets", "Library", "ProjectSettings", "Src", "Include", "Binaries", "Build",
};
} /// @note namespace

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
    /// @note 絶対/相対パスどちらも受け付け、保存時は相対パスを優先する (別 PC へ移しても動くように)。
    std::filesystem::path out = util::FileSystem::PathFromUtf8(outputDirectory);
    if (out.is_absolute()) return out;
    return util::FileSystem::PathFromUtf8(projectRoot) / out;
}

std::filesystem::path BuildSettings::ResolveIconPath(const std::string& projectRoot) const
{
    if (iconPath.empty()) return {};
    std::filesystem::path icon = util::FileSystem::PathFromUtf8(iconPath);
    if (icon.is_absolute()) return icon;
    return util::FileSystem::PathFromUtf8(projectRoot) / icon;
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
    if (outputDirectory.find('\0') != std::string::npos || projectRoot.find('\0') != std::string::npos) {
        outReason = "Output Directory and project root must not contain NUL.";
        return false;
    }

    std::filesystem::path root;
    std::filesystem::path out;
    if (!CanonicalPath(util::FileSystem::PathFromUtf8(projectRoot), root, outReason)
        || !CanonicalPath(ResolveOutputPath(projectRoot), out, outReason)) return false;
    std::error_code error;
    const bool rootExists = std::filesystem::is_directory(root, error);
    if (error || !rootExists) {
        outReason = "Project root is not a readable directory.";
        return false;
    }

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
        std::filesystem::path dir;
        if (!CanonicalPath(root / name, dir, outReason)) return false;
        if (!ContainsOrEquals(dir, out) && !ContainsOrEquals(out, dir)) continue;
        outReason = std::string("Output Directory overlaps the project's ") + name + "/ folder.";
        return false;
    }

    /// @note 既に中身があるフォルダは、前回の配布物 (game.manifest.toml を持つ) でない限り拒否する。
    /// @note 検査できない既存フォルダーや通常ファイルは、削除対象として許可しない。
    const std::filesystem::file_status status = std::filesystem::status(out, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        outReason = "Output Directory could not be inspected: " + error.message();
        return false;
    }
    if (status.type() == std::filesystem::file_type::not_found || error == std::errc::no_such_file_or_directory) {
        std::filesystem::path parent = out.parent_path();
        while (!parent.empty()) {
            error.clear();
            const std::filesystem::file_status parentStatus = std::filesystem::status(parent, error);
            if (error && error != std::errc::no_such_file_or_directory) {
                outReason = "Output parent could not be inspected: " + error.message();
                return false;
            }
            if (std::filesystem::exists(parentStatus)) {
                if (std::filesystem::is_directory(parentStatus)) return true;
                outReason = "Output parent exists and is not a directory.";
                return false;
            }
            const std::filesystem::path next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
        outReason = "Output Directory has no existing directory ancestor.";
        return false;
    }
    if (!std::filesystem::is_directory(status)) {
        outReason = "Output Directory exists and is not a directory.";
        return false;
    }
    error.clear();
    const bool isEmpty = std::filesystem::is_empty(out, error);
    if (error) {
        outReason = "Output Directory could not be enumerated: " + error.message();
        return false;
    }
    if (!isEmpty) {
        const bool previousBuild = std::filesystem::is_regular_file(out / "game.manifest.toml", error);
        if (error && error != std::errc::no_such_file_or_directory) {
            outReason = "Previous package manifest could not be inspected: " + error.message();
            return false;
        }
        if (!previousBuild) {
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

} /// @note namespace fbzz::editor
