/// @file    BuildSettings.hpp
/// @brief   ゲームビルドのパッケージング設定。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note 永続化は持たない。保存先は EditorSettings (`Assets/EditorConfig/editor_settings.toml`) に統一する。
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor {

/// @note ビルドに含めるシーン。プロジェクト内の絶対パスは開始前に相対パスへ正規化する。
struct SceneEntry {
    std::string path;
    bool        enabled = true;
};

/// @brief 配布ビルドの設定一式。
struct BuildSettings {
    std::vector<SceneEntry> scenes;
    /// @note 移動可能な設定にするためプロジェクトルート相対を推奨する。
    std::string outputDirectory = "Builds/MyGame";
    std::string productName     = "MyGame";
    std::string version         = "1.0.0";

    /// @note 空ならアイコンを差し替えない。設定は "Assets/..." 相対を推奨する。
    std::string iconPath;

    /// @note true は Development 構成と manifest の development = true を選ぶ。
    bool        developmentBuild = true;

    /// @brief ソースとエディター専用ファイルを配布物から外す。
    /// @note Assets にはスクリプト原本 (.hpp) や EditorConfig が同居し配布物に入れる理由がないため既定で有効。ランタイムが読む .hlsl / .meta は除外しない。
    bool        stripEditorAssets = true;

    /// @return 有効なシーンを設定順に返す。
    std::vector<std::string> EnabledScenes() const;

    /// @note 絶対パスを維持し、相対パスはプロジェクトルートに対して解決する。
    std::filesystem::path ResolveOutputPath(const std::string& projectRoot) const;

    /// @return 未設定なら空のパス。
    std::filesystem::path ResolveIconPath(const std::string& projectRoot) const;

    /// @brief 配布出力の置換先を読み取りだけで検査する。
    /// @note 実体を正規化し Windows ordinal ignore-case で project/保護フォルダーの重複を拒否する。
    /// @note 未作成・空フォルダー・前回の配布物だけを許す。検査エラーは拒否し、削除直前にも再検査する。
    /// @param outReason false のときに UI とビルドログへ出す理由
    /// @return 安全なら true
    bool ValidateOutputPath(const std::string& projectRoot, std::string& outReason) const;

    /// @note EditorSettings の [build] が無い場合だけ、旧 BuildSettings.toml から移行する。
    static bool LoadLegacyFile(const std::string& projectRoot, BuildSettings& out);
};

} /// @note namespace fbzz::editor
