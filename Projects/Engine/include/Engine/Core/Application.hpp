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

    // デフォルト設定 (エディタモード) で初期化する。
    bool Init();
    // Standalone モード用: ウィンドウ設定を外部から指定して初期化する。
    // ProjectSettings を Application::Init() より前に読み込んでウィンドウを正しいサイズで生成するため、
    // Window::Config を受け取るオーバーロードを別途用意する。
    // preferredBackend: プロジェクト設定で指定された描画バックエンド。ただしコマンドライン
    //   (--renderer=dx11 / dx12) が指定されていればそちらを優先する。
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
    // Audio初期化に失敗した場合はnullptrを返し、描画・編集機能だけで起動を継続する。
    audio::AudioManager*      GetAudioManager() const { return m_audioManager.get(); }
    // 進行データ (セーブ枠ごと) と環境設定 (枠に依らず 1 本) のランタイム永続化。
    // WHY 2 本に分けるか: セーブ枠の切り替えはテーブルを丸ごと置き換える。同居させると
    //     「別のセーブをロードしたら音量設定が戻った」という原因の見えない不具合になる。
    util::SaveStore&          GetSaveStore()   const { return *m_saveStore; }
    util::SaveStore&          GetConfigStore() const { return *m_configStore; }

    // ScriptProxy (graphics) が触る「実行中の」描画設定。所有はしない。
    // WHY 所有しないか: 実体は ProjectSettings::render で、それを持つのが Editor か
    //     Standalone かで変わる。Application は composition root として参照だけ預かる。
    // NOTE: Editor は Play 中だけ登録すること。編集中に書き換えられると、そのまま
    //       ProjectSettings.toml へ保存されてしまう (EditorApp が終了時に保存する)。
    void SetActiveRenderSettings(renderer::RenderSettings* settings) { m_activeRenderSettings = settings; }
    [[nodiscard]] renderer::RenderSettings* GetActiveRenderSettings() const { return m_activeRenderSettings; }

    // このプロセスが Editor UI をホストしているか。EditorApp::Init が立てる。
    // WHY: display プロキシが変えるつもりの「ゲームの窓」は、Editor では Editor 自身の窓。
    //     Option 画面を持つゲームを Play するたびにエディタが全画面へ飛び、ウィンドウが
    //     ゲーム側の解像度へ縮む。Editor は常にウィンドウモードなので、窓に触る要求は
    //     ここを見て握り潰す (Standalone では素通り)。
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
    // WHY: AudioManagerはIAudioDeviceを非所有参照するため、Deviceを先に宣言して後から破棄する。
    std::unique_ptr<audio::IAudioDevice>      m_audioDevice;
    std::unique_ptr<audio::AudioManager>      m_audioManager;
    // WHY コンストラクタではなく Init で作らないか: ストアは Window / Renderer に
    //     依存しないので、Get() で取り出した直後から使える方が呼び出し側の順序制約が減る。
    std::unique_ptr<util::SaveStore>          m_saveStore;
    std::unique_ptr<util::SaveStore>          m_configStore;
    renderer::RenderSettings*                 m_activeRenderSettings = nullptr;
    bool                                      m_editorHosted = false;
    MemorySystem                         m_memorySystem;
};

} // namespace fbzz::core
