/// @file    StandaloneApp.hpp
/// @brief   エディタ UI を持たないスタンドアロン (配布ゲーム) モードのライフサイクル管理。
/// @author  Hasegawa Jin
/// @date    2026-05-31
/// @note フレームの境界で必要な入力更新・イベント処理・計測は、IModule を呼ぶ Application::Run が担当する。
#pragma once
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/IModule.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

struct ImGuiContext;

namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; }

namespace fbzz::editor_launcher {

class StandaloneApp final : public core::IModule {
public:
    /// @pre 起動元の描画先・リソース・プロジェクト情報・設定は、このモジュールの終了まで有効であること。
    StandaloneApp(renderer::IRenderer& renderer,
                  renderer::IImGuiRenderer& imguiRenderer,
                  renderer::ResourceManager& resources,
                  const LaunchProject& project,
                  ProjectSettings& settings);

    [[nodiscard]] bool OnInit()         override;
    void               OnUpdate(float dt)     override;
    void               OnLateUpdate(float dt) override;
    void               OnRender()       override;
    void               OnShutdown()     override;

private:
    /// @brief 配布ゲームの診断を game.log に残す。
    struct FileLogSink final : core::ILogSink {
        std::ofstream file;
        void OnLog(const core::LogEntry& entry) override;
    };

    /// @note 起動元が所有する描画先。シーン描画とフレームの開始・終了に使う。
    renderer::IRenderer&             m_renderer;
    /// @note ゲーム画面にプロファイラーを重ねて表示するために借りる。
    renderer::IImGuiRenderer&        m_imguiRenderer;
    /// @note 起動元が所有するリソースを、シーンの読み込みと描画で共有する。
    renderer::ResourceManager&       m_resources;
    /// @note アセットの基準になるプロジェクトルートと、最初に開くシーンのパスを借りる。
    const LaunchProject&             m_project;
    /// @note 実行中のオプション変更をその場で描画・音声に反映するため、起動元の設定を参照する。
    ProjectSettings&                 m_settings;
    /// @note 実行中のシーンとゲームの更新状態をこのアプリの寿命で管理する。
    scene::ProjectRuntime            m_runtime;
    /// @note Logger に登録する出力先。終了時に登録を外すまでアドレスが有効である必要がある。
    FileLogSink                      m_logSink;
    /// @note 初期化時に作る UI コンテキスト。終了時は描画側を停止してから破棄する。
    ImGuiContext*                    m_imguiCtx     = nullptr;
    bool                             m_showProfiler = false;
};

} /// @note namespace fbzz::editor_launcher
