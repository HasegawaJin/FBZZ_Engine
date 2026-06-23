// FBZZ Engine
// BuildSettings.hpp | fbzz::editor
// ゲームビルドのパッケージング設定
//
// WHAT: Unity の Build Settings に相当する構造体。
//       ビルドに含めるシーン、出力先、製品名、バージョンを保持する。
//       プロジェクトルートの BuildSettings.toml にシリアライズする。
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor {

// ビルドに含めるシーンの 1 エントリ。
// path はプロジェクトルートからの相対パスを推奨するが、絶対パスも許容する。
struct SceneEntry {
    std::string path;
    bool        enabled = true;
};

// Build Settings ウィンドウで編集する設定一式。
// Save / Load は bool で成否を返し、例外は使わない。
struct BuildSettings {
    std::vector<SceneEntry> scenes;
    // 出力先: プロジェクトルート相対を推奨。別 PC に持ち込んでも動くように。
    std::string outputDirectory = "Builds/MyGame";
    std::string productName     = "MyGame";
    std::string version         = "1.0.0";
    // true のとき StandaloneApp がログを Verbose に設定する (game.manifest.toml に書き出す)
    bool        developmentBuild = true;

    bool Save(const std::string& projectRoot) const;
    bool Load(const std::string& projectRoot);

    // enabled = true のシーンをインデックス順に返す
    std::vector<std::string> EnabledScenes() const;

    // 絶対パス / 相対パス両対応で出力先を解決する
    std::filesystem::path ResolveOutputPath(const std::string& projectRoot) const;
};

} // namespace fbzz::editor
