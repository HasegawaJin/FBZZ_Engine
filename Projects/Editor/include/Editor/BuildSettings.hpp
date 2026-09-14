/// @file    BuildSettings.hpp
/// @brief   ゲームビルドのパッケージング設定。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// WHAT: Unity の Build Settings に相当する構造体。
/// ビルドに含めるシーン、出力先、製品名、バージョンを保持する。
///
/// WHY (永続化を持たない): 保存先は EditorSettings (Assets/EditorConfig/editor_settings.toml)。
/// エディターが覚えている他の状態と同じファイルに入れることで、
/// プロジェクトルートに設定ファイルが増えるのを避ける。
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

    // 出力先が配布物の書き出し先として安全か判定する。
    //
    // WHY 必要か: コミットは出力先を remove_all してから _tmp を rename する。
    //     空欄のままだと ResolveOutputPath がプロジェクトルートを返し、ビルドを
    //     押した瞬間にプロジェクトごと消える。Browse... で作業フォルダを選んでも同じ。
    // @param outReason false のときに UI とビルドログへ出す理由
    // @ret 安全なら true
    bool ValidateOutputPath(const std::string& projectRoot, std::string& outReason) const;

    // 旧形式 (<projectRoot>/BuildSettings.toml) を読み込む。EditorSettings に
    // [build] が無い場合の移行専用で、通常の保存経路では使わない。
    static bool LoadLegacyFile(const std::string& projectRoot, BuildSettings& out);
};

} // namespace fbzz::editor
