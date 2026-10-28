/// @file    ProjectRuntime.hpp
/// @brief   Editor PlayとStandaloneで共有するプロジェクト実行状態と更新パイプライン。
/// @author  Hasegawa Jin
/// @date    2026-06-22
#pragma once

#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Physics/World.hpp>

#include <filesystem>
#include <cstdint>
#include <string>
#include <array>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::scene {

/// @brief SceneManager・Physics・Game UI を一つの実行単位として所有し、共通順序で駆動する。
/// @note Module ごとに個別所有すると、LoadScene を受ける Manager と Update する Manager が分離し、Editor Play と Standalone で挙動差が出る。
class ProjectRuntime final {
public:
    ~ProjectRuntime();
    /// @note ProjectSettingsをPhysics、Scheduler、Game UIへ適用する。
    void ApplySettings(const ProjectSettings& settings);

    /// @note EditorのScene Viewなど、Game UI以外のUI Contextへ同じ設定を適用する。
    void ApplyAdditionalUIContext(const ProjectSettings& settings, UISystemContext& context);

    /// @note プロジェクト配下の全Sceneを共通SceneManagerへ登録する。
    void RegisterScenes(const std::filesystem::path& projectRoot,
                        renderer::ResourceManager& resources);

    /// @note Editor所有Sceneを実行対象としてバインドする。nullptrでowned Sceneへ戻す。
    void BindExternalScene(Scene* scene);

    /// @note Play中のLoadSceneでManagerが所有したSceneを破棄し、保留中の遷移要求も捨てる。
    /// @note EditorがStopで編集Sceneへ戻るときに呼ぶ。
    void ReleaseOwnedScene();

    /// @note 遷移で切り替わった実行中Sceneの登録名。遷移していなければ空。
    [[nodiscard]] const std::string& ActiveSceneName() const { return m_sceneManager.ActiveSceneName(); }

    /// @note 登録済みScene名への遷移を要求する。
    void LoadScene(const std::string& sceneName);

    /// @note 開始Sceneファイルのstemを登録名として遷移を要求する。
    void LoadScene(const std::filesystem::path& sceneFile);

    /// @note 共通のSimulation設定を適用してSystemSchedulerを更新する。
    /// @param simulating このフレームでシミュレーションを進めるか (Pause 中は false)。
    /// @param playing    Play セッションが続いているか。Pause 中も true。
    /// @note 既定の true は Standalone (常に Play セッション) を指す。
    /// @note Editor は Pause を simulating=false / playing=true で表す。
    void Update(float dt,
                const ProjectSettings& settings,
                bool simulating,
                bool singleStep = false,
                bool playing = true);

    /// @note Update後のLateUpdateフェーズを駆動する。
    void LateUpdate(float dt);

    /// @note Play終了時に接触キャッシュを含むPhysics Worldを再生成する。
    void ResetPhysics(const ProjectSettings& settings);

    /// @note ScriptProxyが同じSceneManagerとViewportを参照するようRuntime Contextを有効化する。
    void ActivateScriptRuntime(renderer::IRenderer& renderer,
                               uint32_t viewportWidth,
                               uint32_t viewportHeight);
    /// @note Headless hosts can install the same Scene/quit ownership without a renderer.
    void ActivateScriptRuntime(uint32_t viewportWidth, uint32_t viewportHeight);

    /// @note Game ViewportのリサイズをScriptProxyへ反映する。
    void UpdateScriptViewport(uint32_t viewportWidth, uint32_t viewportHeight);
    /// @note 非所有 host hook の登録/解除。次の Activate でも保持し、Shutdown で解除する。
    void SetScriptQuitHandler(ScriptRuntime::QuitHandler handler, void* context);

    /// @note ScriptProxyの参照を解除し、外部Sceneバインドを外す。
    void Shutdown();

    [[nodiscard]] SceneManager& GetSceneManager() { return m_sceneManager; }
    [[nodiscard]] const SceneManager& GetSceneManager() const { return m_sceneManager; }
    [[nodiscard]] physics::World& GetPhysicsWorld() { return m_physicsWorld; }
    [[nodiscard]] UISystemContext& GetGameUIContext() { return m_gameUICtx; }
    [[nodiscard]] Scene* GetActiveScene() { return m_sceneManager.GetActive(); }

private:
    struct DisplayIntent { uint64_t scene = 0; uint32_t first = 0, second = 0; bool pending = false; };
    /// @note callback の同期処理中には Window/renderer を resize しない。次 frame に exact Scene を再照合する。
    void ApplyDisplayIntents();
    void BindDisplayHandler();
    std::array<DisplayIntent, 3> m_displayIntents{};
    SceneManager     m_sceneManager;
    physics::World   m_physicsWorld;
    UISystemContext  m_gameUICtx;
    ScriptRuntime    m_scriptRuntime;
};

} // namespace fbzz::scene
