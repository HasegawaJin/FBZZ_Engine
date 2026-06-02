// FBZZ Engine
// ProjectResolver.hpp | fbzz::sandbox
// .fbzz_proj と ProjectSettings から Sandbox 起動対象を解決する
#pragma once

#include "LaunchProject.hpp"

#include <filesystem>
#include <string>

namespace fbzz::sandbox {

/// 起動時プロジェクトのパス解決とエラーメッセージ保持を担当する。
/// WHY: main.cpp に TOML 解析とフォールバック探索を置かず、起動分岐だけに集中させる。
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

} // namespace fbzz::sandbox
