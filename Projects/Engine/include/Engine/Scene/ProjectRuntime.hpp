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

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::scene {

/// SceneManager・Physics・Game UIを一つの実行単位として所有し、共通順序で駆動する。
/// WHY: Moduleごとにこれらを個別所有すると、LoadSceneを受けたManagerとUpdateするManagerが
///      分離するなど、Editor PlayとStandaloneの挙動差が発生するため。
class ProjectRuntime final {
public:
    /// ProjectSettingsをPhysics、Scheduler、Game UIへ適用する。
    void ApplySettings(const ProjectSettings& settings);

    /// EditorのScene Viewなど、Game UI以外のUI Contextへ同じ設定を適用する。
    void ApplyAdditionalUIContext(const ProjectSettings& settings, UISystemContext& context);

    /// プロジェクト配下の全Sceneを共通SceneManagerへ登録する。
    void RegisterScenes(const std::filesystem::path& projectRoot,
                        renderer::ResourceManager& resources);

    /// Editor所有Sceneを実行対象としてバインドする。nullptrでowned Sceneへ戻す。
    void BindExternalScene(Scene* scene);

    /// Play中のLoadSceneでManagerが所有したSceneを破棄し、保留中の遷移要求も捨てる。
    /// EditorがStopで編集Sceneへ戻るときに呼ぶ。
    void ReleaseOwnedScene();

    /// 遷移で切り替わった実行中Sceneの登録名。遷移していなければ空。
    [[nodiscard]] const std::string& ActiveSceneName() const { return m_sceneManager.ActiveSceneName(); }

    /// 登録済みScene名への遷移を要求する。
    void LoadScene(const std::string& sceneName);

    /// 開始Sceneファイルのstemを登録名として遷移を要求する。
    void LoadScene(const std::filesystem::path& sceneFile);

    /// 共通のSimulation設定を適用してSystemSchedulerを更新する。
    /// @param simulating このフレームでシミュレーションを進めるか (Pause 中は false)。
    /// @param playing    Play セッションが続いているか。Pause 中も true。
    ///                   既定の true は Standalone (常に Play セッション) を指す。
    ///                   Editor は Pause を simulating=false / playing=true で表す。
    void Update(float dt,
                const ProjectSettings& settings,
                bool simulating,
                bool singleStep = false,
                bool playing = true);

    /// Update後のLateUpdateフェーズを駆動する。
    void LateUpdate(float dt);

    /// Play終了時に接触キャッシュを含むPhysics Worldを再生成する。
    void ResetPhysics(const ProjectSettings& settings);

    /// ScriptProxyが同じSceneManagerとViewportを参照するようRuntime Contextを有効化する。
    void ActivateScriptRuntime(renderer::IRenderer& renderer,
                               uint32_t viewportWidth,
                               uint32_t viewportHeight);

    /// Game ViewportのリサイズをScriptProxyへ反映する。
    void UpdateScriptViewport(uint32_t viewportWidth, uint32_t viewportHeight);

    /// ScriptProxyの参照を解除し、外部Sceneバインドを外す。
    void Shutdown();

    [[nodiscard]] SceneManager& GetSceneManager() { return m_sceneManager; }
    [[nodiscard]] const SceneManager& GetSceneManager() const { return m_sceneManager; }
    [[nodiscard]] physics::World& GetPhysicsWorld() { return m_physicsWorld; }
    [[nodiscard]] UISystemContext& GetGameUIContext() { return m_gameUICtx; }
    [[nodiscard]] Scene* GetActiveScene() { return m_sceneManager.GetActive(); }

private:
    SceneManager     m_sceneManager;
    physics::World   m_physicsWorld;
    UISystemContext  m_gameUICtx;
    ScriptRuntime    m_scriptRuntime;
};

} // namespace fbzz::scene
