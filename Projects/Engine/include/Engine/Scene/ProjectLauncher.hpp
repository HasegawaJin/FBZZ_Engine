// FBZZ Engine
// ProjectLauncher.hpp | fbzz::scene
// ProjectRuntime内部で使う設定適用・シーン登録ユーティリティクラス。
//
// WHY: Sandbox / EditorLauncher / GameHub テンプレートが
//      「ProjectSettings 適用 → シーン登録」という同一の起動フローを
//      それぞれ実装していた。Engine 側に集約することで
//   1. テンプレートを薄くし、ユーザーが触るべきコードを最小化する。
//   2. 起動手順の変更を 1 か所に閉じ込める。
#pragma once
#include <Engine/ProjectSettings.hpp>
#include <filesystem>

namespace fbzz::physics    { class World; }
namespace fbzz::renderer   { class ResourceManager; }
namespace fbzz::scene      { class SceneManager; }
namespace fbzz::scene      { struct UISystemContext; }

namespace fbzz::scene {

class ProjectLauncher {
public:
    // NOTE: Editor/Standaloneのホストから個別に呼ばず、ProjectRuntime経由で使用する。
    // WHY: SceneManagerとPhysics Worldの組み合わせを呼び出し側が選べると実行状態が分裂するため。
    // ── 設定適用 ──────────────────────────────────────────────────────────────

    /// ProjectSettings を Physics / SceneManager / UI に一括反映する。
    /// ApplyPhysicsSettings + ApplyUISettings(primary) + SetPhysicsHz の統合。
    /// @param primaryUICtx 最初に設定を反映する UISystemContext (nullptr 可)
    static void ApplySettings(physics::World& world,
                               SceneManager& sceneManager,
                               const ProjectSettings& settings,
                               UISystemContext* primaryUICtx = nullptr);

    /// 追加の UISystemContext に設定を反映する。
    /// WHY: Editor は gameViewport / sceneViewport の 2 コンテキストを持つため、
    ///      2 回目以降の適用はこちらを使う。
    static void ApplyAdditionalUIContext(const ProjectSettings& settings,
                                          UISystemContext& ctx);

    // ── シーン登録 ────────────────────────────────────────────────────────────

    /// scenesDir 以下の全 .scene ファイルを SceneManager に名前で登録する。
    /// WHY: EditorModule / GameHub テンプレートに重複していた同一のループを集約する。
    ///      Play 中の LoadScene 解決には起動時登録が必要。ファイルが存在しない場合は何もしない。
    static void RegisterScenesFromDirectory(SceneManager& sceneManager,
                                             const std::filesystem::path& scenesDir,
                                             renderer::ResourceManager& resources);
};

} // namespace fbzz::scene
