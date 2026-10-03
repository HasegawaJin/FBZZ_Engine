/// @file    Application.cpp
/// @brief   Application シングルトンの初期化とメインループ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note Window / Renderer / Audio / SceneManager を所有し、エンジン全体の寿命を管理する。
/// @note sandbox 側で手動ループする場合も、初期化済みサブシステムの入口になる。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <timeapi.h>
#include "Engine/Core/Application.hpp"
#include "Engine/Asset/AssetStreaming.hpp"
#include "Engine/Asset/StreamedTextureResolver.hpp"
#include "Engine/Core/Concurrency/TaskSystem.hpp"
#include "Engine/Core/IModule.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Audio/AudioManager.hpp"
#include "Engine/Audio/XAudio2Device.hpp"
#include "Engine/Input/Input.hpp"
#include "Engine/Input/InputActionMap.hpp"
#include "Engine/Profiler/ProfileScope.hpp"
#include "Engine/Profiler/Profiler.hpp"
#include <Engine/Profiler/ProfilerFrame.hpp>
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RendererFactory.hpp"
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Engine/Util/EngineAssetPath.hpp>
#include "Engine/Util/SaveStore.hpp"
#include "Math/MathContract.hpp"

namespace fbzz::core {

namespace {

/// @brief 描画バックエンドを選ぶ。優先順位はコマンドライン (--renderer=) > 呼び出し側指定。
/// @note  DirectX 11 サポートは v1.0 で終了した (Docs/design/dx11-removal.md)。
/// @note --renderer=dx11 は起動を止めず DX12 へ倒すが、黙って倒すと設定が効いていると
/// @note 誤解されるため名指しで警告する。
renderer::RendererBackend SelectRendererBackend(renderer::RendererBackend fallback)
{
    const wchar_t* commandLine = GetCommandLineW();
    if (commandLine) {
        if (wcsstr(commandLine, L"--renderer=dx12")) return renderer::RendererBackend::DX12;
        if (wcsstr(commandLine, L"--renderer=dx11")) {
            FBZZ_LOG_WARN("Application: --renderer=dx11 は v1.0 でサポートを終了しました。"
                          "DirectX 12 で起動します (Docs/design/dx11-removal.md)");
            return renderer::RendererBackend::DX12;
        }
    }
    return fallback;
}

/// @brief ウィンドウタイトルに付ける描画バックエンドの識別サフィックス。
/// @note  バックエンドが 1 つになった後も残すのは、実行中のバイナリがどの API で
/// @note 動いているかをスクリーンショットだけで判別できるようにするため。
const wchar_t* BackendTitleTag(renderer::RendererBackend backend)
{
    switch (backend) {
    case renderer::RendererBackend::DX12: return L" [DirectX 12]";
    }
    return L" [Unknown Renderer]";
}

} /// @note namespace

Application& Application::Get() {
    static Application instance;
    return instance;
}

/// @note 既定パスを Application が決めるのは、ゲーム側が設定し忘れてもセーブと設定が
/// @note 同じファイルへ落ちない状態を最初から保証するため。ゲームは SetPath / SetSlot で
/// @note 好きな場所へ差し替えてよい。
Application::Application()
    : m_saveStore(std::make_unique<util::SaveStore>("Saves/save0.toml"))
    , m_configStore(std::make_unique<util::SaveStore>("Config/settings.toml"))
{
}

Application::~Application() = default;

bool Application::Init() {
    /// @note エディタは常にデフォルト設定 (1920x1080 ウィンドウ) で起動する。
    /// @note Standalone モードのみ ProjectSettings から取得した Config を渡す。
    return Init(Window::Config{});
}

bool Application::Init(const Window::Config& windowConfig,
                       renderer::RendererBackend preferredBackend) {
    FBZZ_LOG_INFO("Application::Init: 開始 (preferred=%s)", renderer::ToString(preferredBackend));
    timeBeginPeriod(1);
    renderer::SetShaderPathResolver(&util::ResolveEngineAssetPath);

    /// @note 数学の契約違反を Logger へ流す。Math は Engine に依存できないので、出力先はここで差す。
    /// @note 落とさないのは、ゼロ長ベクトルも特異行列も «ユーザーデータ» で普通に起き、
    /// @note abort すると未保存の作業ごとエディターが死ぬため (MathContract.hpp)。
    math::SetContractHandler([](const math::ContractViolation& v) {
        FBZZ_LOG_ERROR("[%s:%d] %s: %s (%s)",
                       v.file, v.line, v.function, v.message, v.expr);
    });

    /// @note フレームアロケータとメモリ統計はエンジン全体の診断基盤なので、
    /// @note Window / Renderer より先に初期化し、以後のサブシステムから参照できる状態にする。
    constexpr std::size_t FRAME_ALLOCATOR_CAPACITY = 8u * 1024u * 1024u;
    if (!m_memorySystem.Initialize(FRAME_ALLOCATOR_CAPACITY)) {
        FBZZ_LOG_ERROR("Application::Init: MemorySystem 初期化失敗");
        return false;
    }

    TaskSystem::Init();

    /// @note バックエンドを Window 生成前に確定させ、タイトルへ識別サフィックスを付ける。
    /// @note 同じ backend 値を後段の CreateRenderer にも渡し、選択規則を二重評価しない。
    /// @note preferredBackend (プロジェクト設定) を既定に、コマンドラインがあれば上書きする。
    const renderer::RendererBackend backend = SelectRendererBackend(preferredBackend);
    FBZZ_LOG_INFO("Application::Init: 描画バックエンド = %s", renderer::ToString(backend));
    Window::Config windowConfigTagged = windowConfig;
    windowConfigTagged.title += BackendTitleTag(backend);

    m_window = std::make_unique<Window>();
    if (!m_window->Initialize(windowConfigTagged)) {
        FBZZ_LOG_ERROR("Application::Init: ウィンドウ生成に失敗しました");
        return false;
    }
    FBZZ_LOG_INFO("Application::Init: ウィンドウ生成 OK (%ux%u)",
                  m_window->GetWidth(), m_window->GetHeight());

    input::Input::Init();

    /// @note Standalone はウィンドウ設定のため Init より先に ProjectSettings を読む。
    /// @note 読み込み済みの Submit / Cancel やゲーム固有アクションを既定値で消さない。
    input::InputActionMap::Initialize();

    /// @note バックエンド具象の選択と生成は RendererFactory に集約してある。
    /// @note 合成ルートである Application は RendererBackend を指定するだけで具象を直接知らない。
    FBZZ_LOG_INFO("Application::Init: レンダラー生成を開始します");
    auto rendererBundle = renderer::CreateRenderer(
        backend,
        m_window->GetHandle(), m_window->GetWidth(), m_window->GetHeight());
    if (!rendererBundle.renderer || !rendererBundle.imguiRenderer) {
        FBZZ_LOG_ERROR("Application::Init: レンダラー生成に失敗しました (backend=%s) — 起動を中止します",
                       renderer::ToString(backend));
        return false;
    }
    FBZZ_LOG_INFO("Application::Init: レンダラー生成 OK");

    m_renderer = std::move(rendererBundle.renderer);
    m_imguiRenderer = std::move(rendererBundle.imguiRenderer);

    /// @note WM_SIZE をレンダラーのスワップチェーン再構築へ橋渡しする。繋がないと
    /// @note m_width/m_height だけが更新され、バックバッファは起動時の寸法のまま残る。
    /// @note flip-model のスワップチェーンはサイズ不一致を DWM 側の引き伸ばしで吸収するため、
    /// @note エラーも警告も出ないまま描画全体がぼやけ続ける。
    /// @note レンダラー生成後でなければ m_renderer が空。Run() まで遅らせると Init 中に届く
    /// @note WM_SIZE (メニューバー設定やウィンドウ移動) を取りこぼすため、ここで登録する。
    /// @note PollEvents() は BeginFrame() の前に呼ばれるため、このコールバックは常にフレーム外で走る。
    /// @note リサイズドラッグ中の Win32 内部ループから再入した場合も同じ (DX12 側は m_frameOpen を見て保留する)。
    m_window->SetResizeCallback([this](uint32_t width, uint32_t height) {
        if (m_renderer)
            m_renderer->Resize(width, height);
    });


    /// @note AudioSourceComponent の要求を実 Voice へ変換できるよう、Application を音響の合成ルートにする。
    /// @note Editor と Standalone は同じ AudioManager を各 ProjectRuntime へ渡して利用する。
    m_audioDevice = std::make_unique<audio::XAudio2Device>();
    m_audioManager = std::make_unique<audio::AudioManager>(*m_audioDevice);
    if (!m_audioManager->Init()) {
        FBZZ_LOG_WARN("Application::Init: Audio初期化失敗 — サイレントモードで継続します");
        m_audioManager.reset();
        m_audioDevice.reset();
    } else {
        FBZZ_LOG_INFO("Application::Init: Audio初期化 OK");
    }

    m_sceneManager = std::make_unique<scene::SceneManager>();
    m_sceneManager->SetAudioManager(m_audioManager.get());

    FBZZ_LOG_INFO("Application started: %ux%u", m_window->GetWidth(), m_window->GetHeight());
    return true;
}

void Application::Shutdown() {
    /// @note 描画設定の実体を持っているのは Module 側 (ProjectSettings)。Application より先に
    /// @note 消えることがあるので、参照はここで必ず切っておく。
    m_activeRenderSettings = nullptr;
    renderer::SetShaderPathResolver(nullptr);
    /// @note SceneManager は AudioManager を raw pointer で参照するため、音響より先に参照と Scene を破棄する。
    if (m_sceneManager) m_sceneManager->SetAudioManager(nullptr);
    m_sceneManager.reset();
    if (m_audioManager) m_audioManager->Shutdown();
    m_audioManager.reset();
    m_audioDevice.reset();
    TaskSystem::Shutdown();
    m_imguiRenderer.reset();
    /// @note デバイスを破棄する前に ResourceManager 所有の全 GPU リソースを解放する。
    /// @note ResourceManager は呼び出し側のローカル変数として Application より長く生存するため、
    /// @note 先に renderer を破棄すると Shader 等が D3D の Live Object として報告される。
    if (renderer::ResourceManager* resources = renderer::ResourceManager::Active())
        resources->Reset();
    if (m_renderer)
        m_renderer->Shutdown();
    m_renderer.reset();
    if (m_window)
        m_window->Shutdown();
    m_memorySystem.Shutdown();
    FBZZ_LOG_INFO("Application shutdown (frames: %llu)", Time::frameCount);
    timeEndPeriod(1);
}

void Application::Run() {
    if (!Init()) return;

    while (m_isRunning) {
        Time::Tick();
        if (auto* resources = renderer::ResourceManager::Active()) resources->AdvanceFrame();
        m_memorySystem.BeginFrame();
        profiler::BeginApplicationProfileFrame();

        {
            FBZZ_PROFILE_SCOPE("Input::Update");
            input::Input::Update();
        }

        {
            FBZZ_PROFILE_SCOPE("Window::PollEvents");
            m_window->PollEvents();
        }

        /// @note PollEvents の後に置くのは、Input::Update() がフレーム先頭で前フレーム状態を
        /// @note 退避するだけで、キーボード/マウスの現在状態は PollEvents 内の Win32 メッセージで
        /// @note 更新されるため。その前で評価すると常に 1 フレーム古い入力を見ることになる。
        {
            FBZZ_PROFILE_SCOPE("InputActionMap::Update");
            /// @note 入力の平滑化はプレイヤーの操作感であり、スローモーション演出 (TimeScale)
            /// @note に引きずられて鈍くなるべきではないため unscaledDeltaTime を使う。
            input::InputActionMap::Update(Time::unscaledDeltaTime);
        }
        if (m_window->ShouldClose()) {
            profiler::EndApplicationProfileFrame();
            m_memorySystem.EndFrame();
            Quit();
            break;
        }

        {
            FBZZ_PROFILE_SCOPE("Renderer::Frame");
            m_renderer->BeginFrame();
            m_renderer->Clear({ 0.10f, 0.15f, 0.25f, 1.0f });
            m_renderer->EndFrame();
        }

        profiler::EndApplicationProfileFrame();
        m_memorySystem.EndFrame();
    }

    Shutdown();
}

void Application::Run(IModule& module) {
    /// @note Init() やシーンロードにかかった時間を最初のゲームフレームの DeltaTime に混ぜない。
    /// @note 先に Time::Tick() を呼んで時刻基準を作り、OnInit() 後の最初の Tick で実フレーム時間だけを得る。
    Time::Tick();

    if (!module.OnInit()) {
        module.OnShutdown();
        return;
    }

    while (m_isRunning) {
        Time::Tick();
        if (auto* resources = renderer::ResourceManager::Active()) resources->AdvanceFrame();
        m_memorySystem.BeginFrame();
        profiler::BeginApplicationProfileFrame();

        {
            FBZZ_PROFILE_SCOPE("Input::Update");
            input::Input::Update();
        }

        {
            FBZZ_PROFILE_SCOPE("Window::PollEvents");
            m_window->PollEvents();
        }

        /// @note 注入はアクション層の評価より前。後に置くと注入した入力がアクションへ届くのが 1 フレーム遅れる。
        module.OnInputPolled();

        /// @note PollEvents の後に置くのは、Input::Update() がフレーム先頭で前フレーム状態を
        /// @note 退避するだけで、キーボード/マウスの現在状態は PollEvents 内の Win32 メッセージで
        /// @note 更新されるため。その前で評価すると常に 1 フレーム古い入力を見ることになる。
        {
            FBZZ_PROFILE_SCOPE("InputActionMap::Update");
            /// @note 入力の平滑化はプレイヤーの操作感であり、スローモーション演出 (TimeScale)
            /// @note に引きずられて鈍くなるべきではないため unscaledDeltaTime を使う。
            input::InputActionMap::Update(Time::unscaledDeltaTime);
        }
        if (m_window->ShouldClose()) {
            /// @note BeginFrame() 済みの Profiler / MemorySystem を必ず対で閉じる。
            /// @note break 前に EndFrame() することで終了フレームでも診断状態を壊さない。
            profiler::EndApplicationProfileFrame();
            m_memorySystem.EndFrame();
            Quit();
            break;
        }

        /// @note 非同期アセットの完了回収と公開はフレーム境界で行う。Scene 更新と描画記録より前、
        /// @note レンダラーのフレーム外に置くので、このフレームの描画は公開済みの実体だけを見る。
        /// @see Docs/design/asset-streaming.md «責務とスレッド境界»
        {
            FBZZ_PROFILE_SCOPE("AssetStreamer::Pump");
            asset::AssetStreamer::Engine().Pump();
            /// @note 前のフレームで描画が引かなかったテクスチャの利用権を手放す。解放は台帳の猶予と予算が決める。
            asset::StreamedTextureResolver::Engine().EndFrame();
        }

        const float dt = Time::deltaTime;
        module.OnUpdate(dt);
        module.OnLateUpdate(dt);

        /// @note AudioSystem ではなくここで回すのは、AudioSystem が SimOnly で Edit モードでは
        /// @note 走らないため。回収を任せると、Editor のプレビュー再生ぶんが Play 開始まで解放されない。
        if (m_audioManager) m_audioManager->Update(dt);

        module.OnRender();

        profiler::EndApplicationProfileFrame();
        m_memorySystem.EndFrame();
    }

    module.OnShutdown();
}

void Application::Quit() {
    m_isRunning = false;
}

} /// @note namespace fbzz::core
