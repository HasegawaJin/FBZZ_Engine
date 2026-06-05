// FBZZ Engine
// ProjectResolver.hpp | fbzz
// .fbzz_proj と ProjectSettings から起動対象を解決するユーティリティ
//
// WHY: Sandbox と EditorLauncher の両方が同じ「プロジェクトパス → 設定ファイル → 開始シーン」解決を必要とする。
//      重複を避けるため Engine に集約し、どちらの起動バイナリからも参照できるようにする。
#pragma once

#include <filesystem>
#include <string>

namespace fbzz {

/// 起動時に解決済みのプロジェクトパスを保持する値型。
struct LaunchProject {
    std::filesystem::path root;
    std::filesystem::path projectFile;
    std::filesystem::path settingsFile;
    std::filesystem::path sceneFile;
    std::filesystem::path scriptsDll;  // スクリプト DLL の絶対パス (省略可: 開発環境フォールバック)
};

/// .fbzz_proj / ProjectSettings / 開始シーンのパス解決とエラーメッセージ保持を担当する。
class ProjectResolver {
public:
    /// 指定パスから .fbzz_proj / ProjectSettings / start_scene を解決する。
    [[nodiscard]] bool Resolve(const std::filesystem::path& projectPath);

    /// 解決済みプロジェクト情報を返す。
    [[nodiscard]] const LaunchProject& Get() const { return m_project; }

    /// Resolve() 失敗時に MessageBox へ表示する wide string を返す。
    [[nodiscard]] const std::wstring& ErrorMessage() const { return m_errorMessage; }

private:
    LaunchProject m_project;
    std::wstring  m_errorMessage;
};

} // namespace fbzz
