// FBZZ Engine
// StandaloneApp.cpp | fbzz::editor_launcher
// スタンドアロンモードのゲームループ実装 (IModule)
//
// WHY: IModule を継承することで Application::Run() に乗せる。
//      これにより Profiler::BeginFrame/EndFrame・MemorySystem・
//      Input::Update・PollEvents などフレーム境界処理がエンジン側で統一される。
#include "StandaloneApp.hpp"
#include <Editor/Util/SceneSerializer.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Profiler/Profiler.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <imgui.h>
#include <chrono>
#include <ctime>

namespace fbzz::editor_launcher {

void StandaloneApp::FileLogSink::OnLog(const core::LogEntry& entry)
{
    if (!file.is_open()) return;
    const char* prefix = "";
    switch (entry.level) {
    case core::LogLevel::DEBUG:     prefix = "[DEBUG] "; break;
    case core::LogLevel::INFO:      prefix = "[INFO]  "; break;
    case core::LogLevel::WARNING:   prefix = "[WARN]  "; break;
    case core::LogLevel::LOG_ERROR: prefix = "[ERROR] "; break;
    }
    file << prefix << entry.message << '\n';
    file.flush();
}

StandaloneApp::StandaloneApp(renderer::IRenderer& renderer,
                             renderer::ResourceManager& resources,
                             const LaunchProject& project,
                             const ProjectSettings& settings)
    : m_renderer(renderer)
    , m_resources(resources)
    , m_project(project)
    , m_settings(settings)
{}

bool StandaloneApp::OnInit()
{
    // WHY: Standalone は WIN32 サブシステムのためコンソールがなく printf が見えない。
    //      OutputDebugString はデバッガがないと見えない。
    //      game.log を exe 隣に生成し、Debug・Release 両方でログを残す。
    const std::filesystem::path logPath =
        util::FileSystem::GetExecutableDirectory() / L"game.log";
    m_logSink.file.open(logPath, std::ios::out | std::ios::trunc);
    if (m_logSink.file.is_open()) {
        // 起動時刻をヘッダとして書く
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        char timeBuf[64] = {};
        ctime_s(timeBuf, sizeof(timeBuf), &t);
        m_logSink.file << "=== FBZZ Standalone Log === " << timeBuf;
        core::Logger::AddSink(&m_logSink);
    }

    m_scene        = std::make_unique<scene::Scene>();
    m_physicsWorld = std::make_unique<physics::World>();

    m_physicsWorld->SetGravity(m_settings.physics.gravity);
    m_physicsWorld->SetSubsteps(m_settings.physics.substeps);
    scene::UISystemSetDefaultFontPath(m_settings.ui.defaultFontPath);

    // WHY: スクリプト DLL はシーンロードより前にロードしなければならない。
    //      SceneSerializer がシーン内の ScriptComponent を復元する際に
    //      ScriptFactory からファクトリ関数を引くため、DLL が未ロードだと
    //      スクリプトインスタンスが生成されず OnUpdate() に到達できない。
    //
    // DLL パスの解決優先順位:
    //   1. .fbzz_proj の scripts_dll フィールド (BuildPipeline が配布物へ記録)
    //   2. exe 隣の SandboxScripts.dll (Sandbox 開発環境フォールバック)
    std::filesystem::path scriptsDllPath = m_project.scriptsDll;
    if (scriptsDllPath.empty() || !std::filesystem::exists(scriptsDllPath)) {
        scriptsDllPath = util::FileSystem::GetExecutableDirectory() / L"SandboxScripts.dll";
    }
    if (std::filesystem::exists(scriptsDllPath)) {
        m_scriptDll.Load(scriptsDllPath);
        FBZZ_LOG_INFO("StandaloneApp: scripts DLL loaded: %ls (%d types)",
            scriptsDllPath.wstring().c_str(),
            static_cast<int>(scene::ScriptFactory::RegisteredTypeNames().size()));
    } else {
        FBZZ_LOG_WARN("StandaloneApp: scripts DLL not found: %ls — no scripts will run",
            scriptsDllPath.wstring().c_str());
    }

    if (!editor::SceneSerializer::Load(*m_scene, m_project.sceneFile.string())) {
        FBZZ_LOG_ERROR("StandaloneApp: シーンのロードに失敗: %s", m_project.sceneFile.string().c_str());
        return false;
    }
    FBZZ_LOG_INFO("StandaloneApp: シーンロード完了: %s", m_project.sceneFile.string().c_str());

    // プロファイラオーバーレイ用 ImGui を初期化する。
    // WHY: StandaloneApp は EditorApp を使わないため ImGui コンテキストが存在しない。
    //      Release ビルドでもプロファイラデータを確認できるよう独立したコンテキストを作成する。
    m_imguiCtx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // ini を書かない (スタンドアロンゲームのドキュメントを汚さない)
    m_renderer.ImGuiInit(core::Application::Get().GetWindow().GetHandle());

    return true;
}

void StandaloneApp::OnUpdate(float dt)
{
    // F3 でプロファイラオーバーレイをトグルする
    if (input::Input::KeyDown(input::KeyCode::F3))
        m_showProfiler = !m_showProfiler;

    scene::Script::SetPhysicsWorld(m_physicsWorld.get());
    scene::ScriptSystem(*m_scene, dt);
    scene::TransformSystem(*m_scene);

    // WHY: 物理シミュレーションはフレームレートに依存しないよう固定タイムステップで動かす。
    const int   physicsHz = m_settings.physics.hz < 1 ? 60 : m_settings.physics.hz;
    const float fixedDt   = 1.0f / static_cast<float>(physicsHz);
    m_physicsAccumulator += dt;
    if (m_physicsAccumulator > fixedDt * 8.0f) m_physicsAccumulator = fixedDt * 8.0f;
    while (m_physicsAccumulator >= fixedDt) {
        scene::PhysicsSystem(*m_scene, *m_physicsWorld, fixedDt);
        m_physicsAccumulator -= fixedDt;
    }

    scene::TransformSystem(*m_scene);
}

void StandaloneApp::OnLateUpdate(float dt)
{
    scene::LateScriptSystem(*m_scene, dt);
    scene::TransformSystem(*m_scene);
    scene::AnimatorSystem(*m_scene, m_resources, dt);
    scene::IKSystem(*m_scene, *m_physicsWorld, m_resources, dt);
}

void StandaloneApp::OnRender()
{
    auto& app = core::Application::Get();

    m_renderer.BeginFrame();
    m_renderer.SetRenderTarget({}, m_resources);
    m_renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

    const auto [w, h] = std::make_pair(app.GetWindow().GetWidth(), app.GetWindow().GetHeight());
    const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : 1.0f;
    const renderer::Camera gameCamera = ResolveGameCamera(aspect);

    scene::RenderSystemUIOptions uiOptions{};
    uiOptions.enabled            = true;
    uiOptions.viewportWidth      = static_cast<float>(w);
    uiOptions.viewportHeight     = static_cast<float>(h);
    uiOptions.mouseInCanvasSpace = input::Input::MousePosition();
    uiOptions.mousePressed       = input::Input::MouseButton(0);
    uiOptions.targetView         = scene::UIRenderTargetView::GameViewport;
    scene::RenderSystem(*m_scene, m_renderer, m_resources, gameCamera, {}, &m_settings.render,
                        fbzz::Layer::Everything, &uiOptions);

    // ── プロファイラオーバーレイ (F3 で表示) ────────────────────────────────
    m_renderer.ImGuiNewFrame();
    ImGui::NewFrame();

    if (m_showProfiler) {
        const float dt = core::Time::DeltaTime();
        const float fps = (dt > 0.0f) ? (1.0f / dt) : 0.0f;

        ImGui::SetNextWindowPos({ 8.0f, 8.0f }, ImGuiCond_Always);
        ImGui::SetNextWindowSize({ 360.0f, 0.0f }, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("##ProfOverlay", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("FPS: %.1f  (%.2f ms)", fps, dt * 1000.0f);
        ImGui::Separator();

        const auto& records = profiler::Profiler::GetLastFrameRecords();
        double totalMs = 0.0;
        for (const auto& r : records) totalMs += r.elapsedMs;
        ImGui::Text("Total: %.3f ms", totalMs);
        ImGui::Spacing();

        // カテゴリ別集計
        struct CatSum { const char* name; double ms; };
        CatSum cats[] = {
            { "Input",   0.0 }, { "Scene",  0.0 }, { "Physics", 0.0 },
            { "Render",  0.0 }, { "Memory", 0.0 }, { "Other",   0.0 },
        };
        for (const auto& r : records) {
            auto add = [&](int idx) { cats[idx].ms += r.elapsedMs; };
            std::string_view n = r.name;
            if (n.find("Input") != std::string_view::npos)                      add(0);
            else if (n.find("Script") != std::string_view::npos ||
                     n.find("Transform") != std::string_view::npos ||
                     n.find("Animator") != std::string_view::npos)              add(1);
            else if (n.find("Physics") != std::string_view::npos)               add(2);
            else if (n.find("Render") != std::string_view::npos ||
                     n.find("Renderer") != std::string_view::npos)              add(3);
            else if (n.find("Memory") != std::string_view::npos)                add(4);
            else                                                                  add(5);
        }
        for (const auto& c : cats) {
            if (c.ms > 0.0)
                ImGui::Text("  %-10s %.3f ms", c.name, c.ms);
        }

        ImGui::End();
    }

    ImGui::Render();
    m_renderer.ImGuiRenderDrawData();

    m_renderer.EndFrame();
}

void StandaloneApp::OnShutdown()
{
    // WHY: FreeLibrary より先に Scene を破棄しないと、DLL 内の仮想デストラクタが
    //      解放済みコードを呼んでアクセス違反になる。
    m_scriptDll.Unload(m_scene.get());
    m_scene.reset();
    m_physicsWorld.reset();

    if (m_imguiCtx) {
        m_renderer.ImGuiShutdown();
        ImGui::DestroyContext(m_imguiCtx);
        m_imguiCtx = nullptr;
    }

    core::Logger::RemoveSink(&m_logSink);
}

renderer::Camera StandaloneApp::ResolveGameCamera(float aspectRatio) const
{
    for (auto& go : m_scene->GameObjects()) {
        auto* cam = go.GetComponent<scene::CameraComponent>();
        if (!go.activeSelf() || !cam || !cam->enabled || !cam->isMain) continue;

        renderer::Camera result;
        result.m_position = go.transform.position;
        result.m_rotation = go.transform.rotation;
        result.m_fovY     = cam->fovY;
        result.m_near     = cam->nearZ;
        result.m_far      = cam->farZ;
        result.m_aspect   = aspectRatio;
        return result;
    }

    renderer::Camera fallback;
    fallback.m_aspect = aspectRatio;
    return fallback;
}

} // namespace fbzz::editor_launcher
