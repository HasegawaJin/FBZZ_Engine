// FBZZ Engine
// StandaloneApp.cpp | fbzz::editor_launcher
// スタンドアロンモードのゲームループ実装
//
// WHAT: エディタ UI なしで直接バックバッファへ描画するシンプルなゲームループ。
//       物理・アニメーション・UI システムを毎フレーム更新し、
//       シーン内の isMain カメラを使って描画する。
#include "StandaloneApp.hpp"
#include <Editor/Util/SceneSerializer.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>

namespace fbzz::editor_launcher {

bool StandaloneApp::Init(renderer::IRenderer& /*renderer*/,
                         renderer::ResourceManager& /*resources*/,
                         const std::filesystem::path& /*assetRoot*/,
                         const std::filesystem::path& sceneFile,
                         const ProjectSettings& settings)
{
    m_settings     = settings;
    m_scene        = std::make_unique<scene::Scene>();
    m_physicsWorld = std::make_unique<physics::World>();

    // 物理・UI 設定を ProjectSettings から適用する
    m_physicsWorld->SetGravity(settings.physics.gravity);
    m_physicsWorld->SetSubsteps(settings.physics.substeps);
    scene::UISystemSetDefaultFontPath(settings.ui.defaultFontPath);

    // シーンをロードする
    // WHY: editor::SceneSerializer はエディタが保存する .fbzz 形式を読み書きする。
    //      Standalone は同じ形式のシーンをそのまま使うため engine 側ではなく editor 側を呼ぶ。
    if (!editor::SceneSerializer::Load(*m_scene, sceneFile.string())) {
        FBZZ_LOG_ERROR("StandaloneApp: シーンのロードに失敗: %s", sceneFile.string().c_str());
        return false;
    }

    FBZZ_LOG_INFO("StandaloneApp: シーンロード完了: %s", sceneFile.string().c_str());
    return true;
}

void StandaloneApp::RunLoop(renderer::IRenderer& renderer,
                            renderer::ResourceManager& resources)
{
    auto& app = core::Application::Get();

    // WHY: warmup フレームの時間 (シーンロード等) を DeltaTime に混入させない。
    core::Time::Tick();

    while (app.IsRunning()) {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) {
            app.Quit();
            break;
        }

        const float dt = core::Time::DeltaTime();

        // --- 物理更新 (固定ステップ) ---
        // WHY: 物理シミュレーションはフレームレートに依存しないよう固定タイムステップで動かす。
        //      蓄積時間が過大になると追いつけなくなるため、上限 (fixedDt * 8 フレーム分) でクランプする。
        const int   physicsHz = m_settings.physics.hz < 1 ? 60 : m_settings.physics.hz;
        const float fixedDt   = 1.0f / static_cast<float>(physicsHz);
        m_physicsAccumulator += dt;
        const float maxAccum  = fixedDt * 8.0f;
        if (m_physicsAccumulator > maxAccum) m_physicsAccumulator = maxAccum;
        while (m_physicsAccumulator >= fixedDt) {
            scene::PhysicsSystem(*m_scene, *m_physicsWorld, fixedDt);
            m_physicsAccumulator -= fixedDt;
        }

        scene::TransformSystem(*m_scene);
        scene::AnimatorSystem(*m_scene, resources, dt);

        // --- 描画 (バックバッファへ直接描画) ---
        // WHY: Standalone モードではオフスクリーン RT を使わず、
        //      バックバッファへ直接描画することでエディタ用 RT の生成コストをゼロにする。
        renderer.BeginFrame();
        renderer.SetRenderTarget({}, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

        const auto [w, h] = std::make_pair(app.GetWindow().GetWidth(), app.GetWindow().GetHeight());
        const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : 1.0f;
        const renderer::Camera gameCamera = ResolveGameCamera(aspect);

        scene::RenderSystemUIOptions uiOptions{};
        uiOptions.enabled = true;
        uiOptions.viewportWidth = static_cast<float>(w);
        uiOptions.viewportHeight = static_cast<float>(h);
        uiOptions.mouseInCanvasSpace = input::Input::MousePosition();
        uiOptions.mousePressed = input::Input::MouseButton(0);
        uiOptions.targetView = scene::UIRenderTargetView::GameViewport;
        scene::RenderSystem(*m_scene, renderer, resources, gameCamera, {}, &m_settings.render,
                            fbzz::Layer::Everything, &uiOptions);

        renderer.EndFrame();
    }
}

void StandaloneApp::Shutdown()
{
    m_scene.reset();
    m_physicsWorld.reset();
}

renderer::Camera StandaloneApp::ResolveGameCamera(float aspectRatio) const
{
    // WHY: Standalone モードでは EditorCamera が存在しないため、
    //      シーン内の isMain フラグを持つ CameraComponent を唯一のゲームカメラとして使う。
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

    // カメラが見つからない場合のフォールバック: 原点・正面向き・デフォルト FOV
    renderer::Camera fallback;
    fallback.m_aspect = aspectRatio;
    return fallback;
}

} // namespace fbzz::editor_launcher
