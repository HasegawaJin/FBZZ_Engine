/// @file    Application.hpp
/// @brief   エンジンのエントリポイントとメインループ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Window / Renderer / Audio / SceneManager を一箇所で所有する Application シングルトン。
/// sandbox や editor は Get() から各サブシステムへアクセスする。
#pragma once
#include "Engine/Core/Memory/MemorySystem.hpp"
#include "Engine/Core/Window.hpp"
#include "Engine/Renderer/IImGuiRenderer.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/RendererBackend.hpp"
#include "Engine/Scene/SceneManager.hpp"
#include <cstdint>
#include <memory>

namespace fbzz::audio { class AudioManager; class IAudioDevice; }
namespace fbzz::util { class SaveStore; }
namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::core {

class IModule;

class Application {
public:
    static Application& Get();

    /// @brief デフォルト設定 (エディタモード) で初期化する。
    bool Init();
    /// @brief Standalone モード用: ウィンドウ設定を外部から指定して初期化する。
    /// @note ProjectSettings を Application::Init() より前に読み込んでウィンドウを正しいサイズで生成するため、Window::Config を受け取るオーバーロードを別途用意する。
    /// @param preferredBackend プロジェクト設定で指定された描画バックエンド。ただしコマンドライン (--renderer=dx11 / dx12) が指定されていればそちらを優先する。
    bool Init(const Window::Config& windowConfig,
              renderer::RendererBackend preferredBackend = renderer::RendererBackend::DX12);
    void Shutdown();
    void Run();
    void Run(IModule& module);
    void Quit();

    bool                    IsRunning()      const { return m_isRunning; }
    Window&                 GetWindow()      const { return *m_window; }
    uint32_t                GetWindowWidth() const { return m_window ? m_window->GetWidth() : 0u; }
    uint32_t                GetWindowHeight() const { return m_window ? m_window->GetHeight() : 0u; }
    renderer::IRenderer&      GetRenderer()      const { return *m_renderer; }
    renderer::IImGuiRenderer& GetImGuiRenderer() const { return *m_imguiRenderer; }
    scene::SceneManager&      GetSceneManager() const { return *m_sceneManager; }
    /// @brief Audio 初期化に失敗した場合は nullptr を返し、描画・編集機能だけで起動を継続する。
    audio::AudioManager*      GetAudioManager() const { return m_audioManager.get(); }
    /// @brief 進行データ (セーブ枠ごと) と環境設定 (枠に依らず 1 本) のランタイム永続化。
    /// @note 同居させると「別のセーブをロードしたら音量設定が戻った」という不具合になるため、セーブ枠を丸ごと差し替える方とは別に持つ。
    util::SaveStore&          GetSaveStore()   const { return *m_saveStore; }
    util::SaveStore&          GetConfigStore() const { return *m_configStore; }

    /// @brief ScriptProxy (graphics) が触る「実行中の」描画設定。所有はしない。
    /// @note Editor は Play 専用コピー、Standalone は解決済みランタイム設定を所有し、Application は非所有参照だけ預かる。
    /// @note Editor は Play 中だけ登録し、終了時に解除する。スクリプトの変更は編集用設定や品質アセットへ保存しない。
    void SetActiveRenderSettings(renderer::RenderSettings* settings) { m_activeRenderSettings = settings; }
    [[nodiscard]] renderer::RenderSettings* GetActiveRenderSettings() const { return m_activeRenderSettings; }

    /// @brief このプロセスが Editor UI をホストしているか。EditorApp::Init が立てる。
    /// @note Option 画面を持つゲームを Play するたびエディタが全画面へ飛びゲーム側の解像度へ縮むのを防ぐため、Editor は常にウィンドウモードとし窓に触る要求はここを見て握り潰す (Standalone では素通り)。
    void SetEditorHosted(bool hosted) { m_editorHosted = hosted; }
    [[nodiscard]] bool IsEditorHosted() const { return m_editorHosted; }
    MemorySystem&           GetMemorySystem() { return m_memorySystem; }
    const MemorySystem&     GetMemorySystem() const { return m_memorySystem; }

private:
    Application();
    ~Application();

    bool m_isRunning = true;
    std::unique_ptr<Window>                   m_window;
    std::unique_ptr<renderer::IRenderer>      m_renderer;
    std::unique_ptr<renderer::IImGuiRenderer> m_imguiRenderer;
    std::unique_ptr<scene::SceneManager>      m_sceneManager;
    /// @note AudioManager は IAudioDevice を非所有参照するため、Device を先に宣言して後から破棄する。
    std::unique_ptr<audio::IAudioDevice>      m_audioDevice;
    std::unique_ptr<audio::AudioManager>      m_audioManager;
    /// @note ストアは Window / Renderer に依存しないため、コンストラクタで作る。Get() で取り出した直後から使えると呼び出し側の順序制約が減る。
    std::unique_ptr<util::SaveStore>          m_saveStore;
    std::unique_ptr<util::SaveStore>          m_configStore;
    renderer::RenderSettings*                 m_activeRenderSettings = nullptr;
    bool                                      m_editorHosted = false;
    MemorySystem                         m_memorySystem;
};

} // namespace fbzz::core
