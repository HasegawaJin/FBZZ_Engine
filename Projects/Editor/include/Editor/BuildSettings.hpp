// FBZZ Engine
// BuildSettings.hpp | fbzz::editor
// ゲームビルドのパッケージング設定
//
// WHAT: Unity の Build Settings に相当する構造体。
//       ビルドに含めるシーン、出力先、製品名、バージョンを保持する。
//
// WHY (永続化を持たない): 保存先は EditorSettings (Assets/EditorConfig/editor_settings.toml)。
//      エディターが覚えている他の状態と同じファイルに入れることで、
//      プロジェクトルートに設定ファイルが増えるのを避ける。
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
struct BuildSettings {
    std::vector<SceneEntry> scenes;
    // 出力先: プロジェクトルート相対を推奨。別 PC に持ち込んでも動くように。
    std::string outputDirectory = "Builds/MyGame";
    std::string productName     = "MyGame";
    std::string version         = "1.0.0";
    // true のとき game.manifest.toml に development = true を書き、dev 構成をビルドする。
    bool        developmentBuild = true;

    // ソースとエディター専用ファイルを配布物から外す。
    // WHY 既定で有効か: Assets には スクリプト原本 (.hpp) や EditorConfig が同居しており、
    //     配布物に入れる理由がない。ランタイムが読む .hlsl / .meta は除外対象に含めない。
    bool        stripEditorAssets = true;

    // enabled = true のシーンをインデックス順に返す
    std::vector<std::string> EnabledScenes() const;

    // 絶対パス / 相対パス両対応で出力先を解決する
    std::filesystem::path ResolveOutputPath(const std::string& projectRoot) const;

    // 旧形式 (<projectRoot>/BuildSettings.toml) を読み込む。EditorSettings に
    // [build] が無い場合の移行専用で、通常の保存経路では使わない。
    static bool LoadLegacyFile(const std::string& projectRoot, BuildSettings& out);
};

} // namespace fbzz::editor
