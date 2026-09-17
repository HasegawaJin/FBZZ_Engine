/// @file    ProjectLauncher.hpp
/// @brief   ProjectRuntime内部で使う設定適用・シーン登録ユーティリティクラス。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// @note Sandbox / EditorLauncher / GameHub テンプレートが個別実装していた
///       「ProjectSettings 適用→シーン登録」の起動フローを Engine 側へ集約する。
/// @note テンプレートを薄くし、起動手順の変更を 1 か所に閉じ込める。
#pragma once
#include <Engine/ProjectSettings.hpp>
#include <filesystem>

namespace fbzz::physics    { class World; }
namespace fbzz::renderer   { class ResourceManager; }
namespace fbzz::scene      { class SceneManager; }
namespace fbzz::scene      { struct UISystemContext; }

namespace fbzz::scene {

/// @brief 設定適用とシーン登録をまとめたユーティリティ。ProjectRuntime 経由でのみ使うこと。
/// @note SceneManager と Physics World の組み合わせを呼び出し側が選べると実行状態が分裂するため。
class ProjectLauncher {
public:
    /// @name 設定適用
    /// @{

    /// @brief ProjectSettings を Physics / SceneManager / UI に一括反映する。
    /// @note ApplyPhysicsSettings + ApplyUISettings(primary) + SetPhysicsHz の統合。
    /// @param primaryUICtx 最初に設定を反映する UISystemContext (nullptr 可)
    static void ApplySettings(physics::World& world,
                               SceneManager& sceneManager,
                               const ProjectSettings& settings,
                               UISystemContext* primaryUICtx = nullptr);

    /// @brief 追加の UISystemContext に設定を反映する。
    /// @note Editor は gameViewport / sceneViewport の 2 コンテキストを持つため、2 回目以降の適用に使う。
    static void ApplyAdditionalUIContext(const ProjectSettings& settings,
                                          UISystemContext& ctx);
    /// @}

    /// @name シーン登録
    /// @{

    /// @brief scenesDir 以下の全 .scene ファイルを SceneManager に名前で登録する。
    /// @note EditorModule / GameHub テンプレートに重複していた同一のループを集約する。Play 中の LoadScene 解決には起動時登録が要る。
    /// @note ファイルが存在しない場合は何もしない。
    static void RegisterScenesFromDirectory(SceneManager& sceneManager,
                                             const std::filesystem::path& scenesDir,
                                             renderer::ResourceManager& resources);
    /// @}
};

} // namespace fbzz::scene
