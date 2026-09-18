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

/// ビルドに含めるシーンの 1 エントリ。
/// path はプロジェクトルートからの相対パスを推奨するが、絶対パスも許容する。
struct SceneEntry {
    std::string path;
    bool        enabled = true;
};

/// Build Settings ウィンドウで編集する設定一式。
struct BuildSettings {
    std::vector<SceneEntry> scenes;
    /// 出力先: プロジェクトルート相対を推奨。別 PC に持ち込んでも動くように。
    std::string outputDirectory = "Builds/MyGame";
    std::string productName     = "MyGame";
    std::string version         = "1.0.0";

    /// exe に焼き込むアイコン画像 (PNG / JPEG / TGA / BMP / ICO)。"Assets/..." 相対。
    /// 空なら差し替えず、Windows の既定アイコンのまま出る。
    std::string iconPath;

    /// true のとき game.manifest.toml に development = true を書き、dev 構成をビルドする。
    bool        developmentBuild = true;

    /// ソースとエディター専用ファイルを配布物から外す。
    /// @note Assets にはスクリプト原本 (.hpp) や EditorConfig が同居し配布物に入れる理由がないため既定で有効。ランタイムが読む .hlsl / .meta は除外しない。
    bool        stripEditorAssets = true;

    /// enabled = true のシーンをインデックス順に返す
    std::vector<std::string> EnabledScenes() const;

    /// 絶対パス / 相対パス両対応で出力先を解決する
    std::filesystem::path ResolveOutputPath(const std::string& projectRoot) const;

    /// アイコン画像のディスク上のパス。未設定なら空を返す。
    std::filesystem::path ResolveIconPath(const std::string& projectRoot) const;

    /// 出力先が配布物の書き出し先として安全か判定する。
    /// @note コミットは出力先を remove_all してから _tmp を rename する。空欄だと ResolveOutputPath がプロジェクトルートを返し、ビルドでプロジェクトごと消える。
    /// @param outReason false のときに UI とビルドログへ出す理由
    /// @return 安全なら true
    bool ValidateOutputPath(const std::string& projectRoot, std::string& outReason) const;

    /// 旧形式 (`<projectRoot>`/BuildSettings.toml) を読み込む。EditorSettings に
    /// [build] が無い場合の移行専用で、通常の保存経路では使わない。
    static bool LoadLegacyFile(const std::string& projectRoot, BuildSettings& out);
};

} // namespace fbzz::editor
