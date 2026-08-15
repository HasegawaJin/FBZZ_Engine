// FBZZ Engine
// Application.cpp | fbzz::core
// Application シングルトンの初期化とメインループ
// Window / Renderer / Audio / SceneManager を所有し、エンジン全体の寿命を管理する。
// sandbox 側で手動ループする場合も、初期化済みサブシステムの入口になる。
#define NOMINMAX
#include <Windows.h>
#include <timeapi.h>
#include "Engine/Core/Application.hpp"
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
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RendererFactory.hpp"

namespace fbzz::core {

namespace {

// 描画バックエンドを選ぶ。優先順位: コマンドライン (--renderer=) > 呼び出し側指定 (fallback)。
// WHY: コマンドラインを最優先にすることで、プロジェクト設定が dx12 でも A/B 検証時に
//      既定はDX12。--renderer=dx11を指定した場合だけ互換バックエンドへ戻す。
renderer::RendererBackend SelectRendererBackend(renderer::RendererBackend fallback)
{
    const wchar_t* commandLine = GetCommandLineW();
    if (commandLine) {
        if (wcsstr(commandLine, L"--renderer=dx12")) return renderer::RendererBackend::DX12;
        if (wcsstr(commandLine, L"--renderer=dx11")) return renderer::RendererBackend::DX11;
    }
    return fallback;
}

// ウィンドウタイトルに付ける描画バックエンドの識別サフィックス。
// WHY: DX11 / DX12 のどちらで起動しているかをタイトルバーで一目で判別できるようにする
//      (A/B 検証時にどちらのウィンドウか取り違えないため)。
const wchar_t* BackendTitleTag(renderer::RendererBackend backend)
{
    switch (backend) {
    case renderer::RendererBackend::DX12: return L" [DirectX 12]";
    case renderer::RendererBackend::DX11: return L" [DirectX 11]";
    }
    return L" [Unknown Renderer]";
}

} // namespace

Application& Application::Get() {
    static Application instance;
    return instance;
}

Application::~Application() = default;

bool Application::Init() {
    // WHY: エディタは常にデフォルト設定 (1920x1080 ウィンドウ) で起動する。
    //      Standalone モードのみ ProjectSettings から取得した Config を渡す。
    return Init(Window::Config{});
}

bool Application::Init(const Window::Config& windowConfig,
                       renderer::RendererBackend preferredBackend) {
    FBZZ_LOG_INFO("Application::Init: 開始 (preferred=%s)", renderer::ToString(preferredBackend));
    timeBeginPeriod(1);

    // WHY: フレームアロケータとメモリ統計はエンジン全体の診断基盤なので、
    //      Window / Renderer より先に初期化し、以後のサブシステムから参照できる状態にする。
    constexpr std::size_t FRAME_ALLOCATOR_CAPACITY = 8u * 1024u * 1024u;
    if (!m_memorySystem.Initialize(FRAME_ALLOCATOR_CAPACITY)) {
        FBZZ_LOG_ERROR("Application::Init: MemorySystem 初期化失敗");
        return false;
    }

    TaskSystem::Init();

    // WHY: バックエンドを Window 生成前に確定させ、タイトルへ識別サフィックスを付ける。
    //      同じ backend 値を後段の CreateRenderer にも渡し、選択規則を二重評価しない。
    //      preferredBackend (プロジェクト設定) を既定に、コマンドラインがあれば上書きする。
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

    // WHY ここで既定バインドを入れるか:
    //   プロジェクト固有の .inputactions は「どのプロジェクトを開くか」が決まってから
    //   ProjectRuntime 側で読み込まれる。Application::Init の時点ではまだ確定していないため、
    //   まず既定バインドで動く状態を作っておく。設定ファイルがあれば後から上書きされる。
    //   これにより「プロジェクト未読込のエディタでも入力が完全に死なない」状態を保証する。
    input::InputActionMap::LoadDefaults();

    // WHY: バックエンド具象 (DX11 / DX12) の選択と生成は RendererFactory に集約する。
    //      合成ルートである Application は RendererBackend を指定するだけで具象を直接知らない。
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

    // WHAT: WM_SIZE をレンダラーのスワップチェーン再構築へ橋渡しする。
    // WHY:  これを繋がないと Window 側の m_width/m_height だけが更新され、バックバッファは
    //       起動時の寸法のまま取り残される。flip-model のスワップチェーンはサイズ不一致を
    //       DWM 側の引き伸ばしで吸収するため、エラーも警告も出ないまま画面全体 (3D だけでなく
    //       ImGui の UI と文字も) がぼやけ続ける。最大化した瞬間に発生し、元のサイズへ戻すまで治らない。
    // WHY ここで登録するか: レンダラー生成後でなければ m_renderer が空。逆に Run() まで遅らせると
    //       Init 中に届く WM_SIZE (メニューバー設定やウィンドウ移動) を取りこぼす。
    // NOTE: PollEvents() は BeginFrame() の前に呼ばれるため、このコールバックは常にフレーム外で走る。
    //       リサイズドラッグ中の Win32 内部ループから再入した場合も同じ (DX12 側は m_frameOpen を見て保留する)。
    m_window->SetResizeCallback([this](uint32_t width, uint32_t height) {
        if (m_renderer)
            m_renderer->Resize(width, height);
    });


    // AudioSourceComponentの要求を実Voiceへ変換できるよう、Applicationを音響の合成ルートにする。
    // EditorとStandaloneは同じAudioManagerを各ProjectRuntimeへ渡して利用する。
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
    // SceneManagerはAudioManagerをraw pointerで参照するため、音響より先に参照とSceneを破棄する。
    if (m_sceneManager) m_sceneManager->SetAudioManager(nullptr);
    m_sceneManager.reset();
    if (m_audioManager) m_audioManager->Shutdown();
    m_audioManager.reset();
    m_audioDevice.reset();
    TaskSystem::Shutdown();
    m_imguiRenderer.reset();
    // WHAT: デバイスを破棄する前に ResourceManager 所有の全 GPU リソースを解放する。
    // WHY: ResourceManager は呼び出し側のローカル変数として Application より長く生存するため、
    //      先に renderer を破棄すると Shader 等が DX11 Live Object として報告される。
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
        m_memorySystem.BeginFrame();
        profiler::Profiler::BeginFrame();

        {
            FBZZ_PROFILE_SCOPE("Input::Update");
            input::Input::Update();
        }

        {
            FBZZ_PROFILE_SCOPE("Window::PollEvents");
            m_window->PollEvents();
        }

        // WHY PollEvents の「後」か:
        //   Input::Update() はフレーム先頭で前フレーム状態を退避するだけで、
        //   キーボード/マウスの現在状態は PollEvents 内の Win32 メッセージで更新される。
        //   アクション層をその前で評価すると、常に 1 フレーム古い入力を見ることになる。
        {
            FBZZ_PROFILE_SCOPE("InputActionMap::Update");
            // WHY unscaledDeltaTime か: 入力の平滑化はプレイヤーの操作感であり、
            //      スローモーション演出 (TimeScale) に引きずられて鈍くなるべきではない。
            input::InputActionMap::Update(Time::unscaledDeltaTime);
        }
        if (m_window->ShouldClose()) {
            profiler::Profiler::EndFrame();
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

        profiler::Profiler::EndFrame();
        m_memorySystem.EndFrame();
    }

    Shutdown();
}

void Application::Run(IModule& module) {
    // WHY: Init() やシーンロードにかかった時間を最初のゲームフレームの DeltaTime に混ぜない。
    //      先に Time::Tick() を呼んで時刻基準を作り、OnInit() 後の最初の Tick で実フレーム時間だけを得る。
    Time::Tick();

    if (!module.OnInit()) {
        module.OnShutdown();
        return;
    }

    while (m_isRunning) {
        Time::Tick();
        m_memorySystem.BeginFrame();
        profiler::Profiler::BeginFrame();

        {
            FBZZ_PROFILE_SCOPE("Input::Update");
            input::Input::Update();
        }

        {
            FBZZ_PROFILE_SCOPE("Window::PollEvents");
            m_window->PollEvents();
        }

        // WHY PollEvents の「後」か:
        //   Input::Update() はフレーム先頭で前フレーム状態を退避するだけで、
        //   キーボード/マウスの現在状態は PollEvents 内の Win32 メッセージで更新される。
        //   アクション層をその前で評価すると、常に 1 フレーム古い入力を見ることになる。
        {
            FBZZ_PROFILE_SCOPE("InputActionMap::Update");
            // WHY unscaledDeltaTime か: 入力の平滑化はプレイヤーの操作感であり、
            //      スローモーション演出 (TimeScale) に引きずられて鈍くなるべきではない。
            input::InputActionMap::Update(Time::unscaledDeltaTime);
        }
        if (m_window->ShouldClose()) {
            // WHY: BeginFrame() 済みの Profiler / MemorySystem を必ず対で閉じる。
            //      break 前に EndFrame() することで終了フレームでも診断状態を壊さない。
            profiler::Profiler::EndFrame();
            m_memorySystem.EndFrame();
            Quit();
            break;
        }

        const float dt = Time::deltaTime;
        module.OnUpdate(dt);
        module.OnLateUpdate(dt);
        module.OnRender();

        profiler::Profiler::EndFrame();
        m_memorySystem.EndFrame();
    }

    module.OnShutdown();
}

void Application::Quit() {
    m_isRunning = false;
}

} // namespace fbzz::core
