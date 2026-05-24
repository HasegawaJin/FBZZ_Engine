// FBZZ Engine
// main.cpp | sandbox
// UISystem verification scene with 3D overlay test
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Scene/Systems/UIAnimatorSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Editor/EditorApp.hpp>
#include <Math/Vector4.hpp>
#include <memory>

using namespace fbzz;
namespace {

std::shared_ptr<renderer::Material> CreateMaterial(renderer::ResourceManager& resources,
                                                   const math::Vector4& color)
{
    auto material = std::make_shared<renderer::Material>();
    material->shader = resources.LoadShader("assets/shaders/Material/Phong.hlsl");
    material->shaderPath = "assets/shaders/Material/Phong.hlsl";
    material->params.albedo = color;
    material->params.roughness = 0.65f;
    material->Init(resources);
    return material;
}

scene::GameObject& MakeButton(scene::Scene& scene,
                               scene::GameObject& parent,
                               const char* name,
                               math::Vector2 pos,
                               math::Vector2 sz,
                               math::Vector4 baseColor)
{
    auto& go = scene.CreateGameObject(name);
    go.SetParent(parent);
    go.transform.localPosition = { pos.x, pos.y, 0.0f };
    go.transform.localScale    = { sz.x,  sz.y,  1.0f };

    scene::UIImage img{};
    img.color    = baseColor;
    go.AddComponent<scene::UIImage>(img);

    scene::UIButton btn{};
    btn.normalColor  = { 1.0f, 1.0f, 1.0f, 1.0f };
    btn.hoverColor   = { 1.25f, 1.25f, 1.25f, 1.0f };
    btn.pressedColor = { 0.6f,  0.6f,  0.6f,  1.0f };
    go.AddComponent<scene::UIButton>(btn);

    return go;
}

scene::GameObject& MakePanel(scene::Scene& scene,
                              scene::GameObject& parent,
                              const char* name,
                              math::Vector2 pos,
                              math::Vector2 sz,
                              math::Vector4 color)
{
    auto& go = scene.CreateGameObject(name);
    go.SetParent(parent);
    go.transform.localPosition = { pos.x, pos.y, 0.0f };
    go.transform.localScale    = { sz.x,  sz.y,  1.0f };

    scene::UIImage img{};
    img.color    = color;
    go.AddComponent<scene::UIImage>(img);

    return go;
}

scene::GameObject& MakeText(scene::Scene& scene,
                             scene::GameObject& parent,
                             const char* name,
                             const char* value,
                             math::Vector2 pos,
                             float fontSize,
                             math::Vector4 color)
{
    auto& go = scene.CreateGameObject(name);
    go.SetParent(parent);
    go.transform.localPosition = { pos.x, pos.y, 0.0f };

    scene::UIText text{};
    text.text = value;
    text.fontSize = fontSize;
    text.color = color;
    go.AddComponent<scene::UIText>(text);

    return go;
}

scene::UICanvas* FindFirstRootCanvas(scene::Scene& scene)
{
    for (scene::GameObject* root : scene.GetRootGameObjects())
    {
        if (!root || !root->activeSelf()) continue;
        auto* canvas = root->GetComponent<scene::UICanvas>();
        if (canvas && canvas->enabled) return canvas;
    }
    return nullptr;
}

} // namespace

int main()
{
    auto& app = core::Application::Get();
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    renderer::ResourceManager resources(renderer);
    asset::AssetManager::Init(resources);

    editor::EditorApp editorApp;
    editorApp.Init(renderer, resources, app.GetWindow());

    auto scene = std::make_unique<scene::Scene>();
    editorApp.GetContext().activeScene = scene.get();

    // 3D scene used to verify UI overlay composition.
    auto& mainCamera = scene->CreateGameObject("MainCamera");
    mainCamera.transform.localPosition = { 0.0f, 0.0f, -5.0f };
    mainCamera.AddComponent<scene::CameraComponent>({});

    auto& light = scene->CreateGameObject("DirectionalLight");
    light.transform.localPosition = { 0.0f, 8.0f, -6.0f };
    light.transform.localRotation = math::Quaternion::LookRotation({ 0.35f, -0.75f, 0.55f });
    light.AddComponent<scene::LightComponent>({});

    auto cubeMesh = renderer::PrimitiveMesh::Cube(resources);
    auto cubeMaterial = CreateMaterial(resources, { 0.15f, 0.65f, 1.0f, 1.0f });
    auto& cube = scene->CreateGameObject("UIOverlayTestCube");
    cube.transform.localPosition = { 0.0f, 0.0f, 3.0f };
    cube.transform.localRotation = math::Quaternion::FromEuler({ 0.3f, 0.5f, 0.0f });
    cube.transform.localScale = { 1.4f, 1.4f, 1.4f };
    scene::MeshRenderer cubeRenderer{};
    cubeRenderer.mesh = cubeMesh;
    cubeRenderer.meshPath = "primitive:cube";
    cube.AddComponent<scene::MeshRenderer>(cubeRenderer);

    scene::MaterialComponent cubeMat{};
    cubeMat.material = cubeMaterial;
    cubeMat.shaderPath = cubeMaterial->shaderPath;
    cube.AddComponent<scene::MaterialComponent>(cubeMat);

    // UI scene: root canvas plus colored panels/buttons.
    auto& canvasGO = scene->CreateGameObject("Canvas");
    {
        scene::UICanvas canvas{};
        canvas.canvasWidth = 1920.0f;
        canvas.canvasHeight = 1080.0f;
        canvasGO.AddComponent<scene::UICanvas>(canvas);
    }

    MakePanel(*scene, canvasGO, "BG_Panel",
              { 560.0f, 180.0f }, { 800.0f, 720.0f },
              { 0.12f, 0.12f, 0.16f, 0.95f });
    MakePanel(*scene, canvasGO, "Header",
              { 560.0f, 180.0f }, { 800.0f, 90.0f },
              { 0.18f, 0.32f, 0.72f, 1.0f });
    MakePanel(*scene, canvasGO, "Header_Accent",
              { 560.0f, 270.0f }, { 800.0f, 4.0f },
              { 0.3f, 0.7f, 1.0f, 1.0f });
    MakeText(*scene, canvasGO, "Title_Text", "FBZZ UI TEXT OK",
             { 610.0f, 205.0f }, 42.0f,
             { 0.95f, 0.98f, 1.0f, 1.0f });

    MakeButton(*scene, canvasGO, "Btn_Start",
               { 680.0f, 330.0f }, { 560.0f, 90.0f },
               { 0.22f, 0.65f, 0.28f, 1.0f });
    MakeText(*scene, canvasGO, "Btn_Start_Text", "START",
             { 845.0f, 357.0f }, 34.0f,
             { 0.98f, 1.0f, 0.98f, 1.0f });
    MakeButton(*scene, canvasGO, "Btn_Options",
               { 680.0f, 450.0f }, { 560.0f, 90.0f },
               { 0.38f, 0.38f, 0.75f, 1.0f });
    MakeText(*scene, canvasGO, "Btn_Options_Text", "OPTIONS",
             { 805.0f, 477.0f }, 34.0f,
             { 0.98f, 0.98f, 1.0f, 1.0f });
    MakeButton(*scene, canvasGO, "Btn_Exit",
               { 680.0f, 570.0f }, { 560.0f, 90.0f },
               { 0.72f, 0.22f, 0.22f, 1.0f });
    MakeText(*scene, canvasGO, "Btn_Exit_Text", "EXIT",
             { 870.0f, 597.0f }, 34.0f,
             { 1.0f, 0.95f, 0.95f, 1.0f });

    MakePanel(*scene, canvasGO, "Footer_Line",
              { 560.0f, 840.0f }, { 800.0f, 2.0f },
              { 0.4f, 0.4f, 0.45f, 1.0f });
    MakePanel(*scene, canvasGO, "Footer",
              { 560.0f, 842.0f }, { 800.0f, 58.0f },
              { 0.08f, 0.08f, 0.1f, 1.0f });
    MakeText(*scene, canvasGO, "Footer_Text", "TEXT RENDER CHECK",
             { 615.0f, 860.0f }, 24.0f,
             { 0.65f, 0.85f, 1.0f, 1.0f });
    MakePanel(*scene, canvasGO, "Status_Indicator",
              { 20.0f, 20.0f }, { 220.0f, 50.0f },
              { 0.9f, 0.55f, 0.1f, 1.0f });
    MakePanel(*scene, canvasGO, "Dot_1",
              { 1680.0f, 20.0f }, { 18.0f, 18.0f },
              { 0.3f, 0.8f, 1.0f, 1.0f });
    MakePanel(*scene, canvasGO, "Dot_2",
              { 1710.0f, 20.0f }, { 18.0f, 18.0f },
              { 0.3f, 0.8f, 1.0f, 0.6f });
    MakePanel(*scene, canvasGO, "Dot_3",
              { 1740.0f, 20.0f }, { 18.0f, 18.0f },
              { 0.3f, 0.8f, 1.0f, 0.3f });

    // --- Phase 2 test: UILayoutGroup (Horizontal) ---
    // A dark container at the bottom-left with 4 color chips auto-arranged by UILayoutGroup.
    {
        auto& layoutGO = scene->CreateGameObject("LayoutGroup_Test");
        layoutGO.SetParent(canvasGO);
        layoutGO.transform.localPosition = { 20.0f, 700.0f, 0.0f };
        layoutGO.transform.localScale    = { 420.0f, 60.0f, 1.0f };
        scene::UIImage containerImg{};
        containerImg.color    = { 0.08f, 0.08f, 0.12f, 0.9f };
        layoutGO.AddComponent<scene::UIImage>(containerImg);

        scene::UILayoutGroup layout{};
        layout.axis        = scene::UILayoutAxis::Horizontal;
        layout.spacing     = 8.0f;
        layout.paddingLeft = 8.0f;
        layout.paddingTop  = 8.0f;
        layoutGO.AddComponent<scene::UILayoutGroup>(layout);

        const char*        slotNames[]  = { "Slot_R", "Slot_G", "Slot_B", "Slot_Y" };
        const math::Vector4 slotColors[] = {
            { 0.85f, 0.3f,  0.3f,  1.0f },
            { 0.3f,  0.82f, 0.35f, 1.0f },
            { 0.3f,  0.5f,  0.9f,  1.0f },
            { 0.9f,  0.82f, 0.25f, 1.0f },
        };
        for (int i = 0; i < 4; ++i) {
            auto& slot = scene->CreateGameObject(slotNames[i]);
            slot.SetParent(layoutGO);
            slot.transform.localScale = { 96.0f, 44.0f, 1.0f };
            scene::UIImage img{};
            img.color = slotColors[i];
            slot.AddComponent<scene::UIImage>(img);
        }
        MakeText(*scene, layoutGO, "LayoutLabel", "LAYOUT",
                 { 340.0f, 22.0f }, 20.0f, { 0.7f, 0.9f, 1.0f, 1.0f });
    }

    // --- Phase 2 test: UIAnimator color tween (blue <-> red, ping-pong) ---
    {
        auto& animGO = scene->CreateGameObject("ColorAnim_Test");
        animGO.SetParent(canvasGO);
        animGO.transform.localPosition = { 20.0f, 775.0f, 0.0f };
        animGO.transform.localScale    = { 195.0f, 42.0f, 1.0f };
        scene::UIImage img{};
        img.color    = { 0.2f, 0.6f, 0.9f, 1.0f };
        animGO.AddComponent<scene::UIImage>(img);

        scene::UIAnimator anim{};
        anim.colorTween.from     = { 0.2f, 0.6f, 0.9f, 1.0f };
        anim.colorTween.to       = { 0.9f, 0.3f, 0.2f, 1.0f };
        anim.colorTween.duration = 1.5f;
        anim.colorTween.loop     = true;
        anim.colorTween.pingPong = true;
        anim.colorTween.active   = true;
        anim.colorTween.easing   = scene::UIEasingType::EaseInOut;
        animGO.AddComponent<scene::UIAnimator>(anim);

        MakeText(*scene, animGO, "ColorAnimLabel", "COLOR TWEEN",
                 { 20.0f, 785.0f }, 20.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
    }

    // --- Phase 2 test: UIAnimator position tween (slides left-right, ping-pong) ---
    {
        auto& posGO = scene->CreateGameObject("PosAnim_Test");
        posGO.SetParent(canvasGO);
        posGO.transform.localPosition = { 230.0f, 775.0f, 0.0f };
        posGO.transform.localScale    = { 110.0f, 42.0f, 1.0f };
        scene::UIImage img{};
        img.color    = { 0.75f, 0.4f, 0.9f, 1.0f };
        posGO.AddComponent<scene::UIImage>(img);

        scene::UIAnimator anim{};
        anim.positionTween.from     = { 230.0f, 775.0f };
        anim.positionTween.to       = { 370.0f, 775.0f };
        anim.positionTween.duration = 0.7f;
        anim.positionTween.loop     = true;
        anim.positionTween.pingPong = true;
        anim.positionTween.active   = true;
        anim.positionTween.easing   = scene::UIEasingType::EaseInOut;
        posGO.AddComponent<scene::UIAnimator>(anim);

        MakeText(*scene, posGO, "PosAnimLabel", "POS",
                 { 240.0f, 783.0f }, 20.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
    }

    // --- Phase 2 test: Anchor/Pivot (bottom-right corner badge) ---
    {
        auto& anchorGO = scene->CreateGameObject("Anchor_Test");
        anchorGO.SetParent(canvasGO);
        anchorGO.transform.localPosition = { 1724.0f, 1020.0f, 0.0f };
        anchorGO.transform.localScale    = { 180.0f, 44.0f, 1.0f };
        scene::UIImage img{};
        img.color = { 0.2f, 0.65f, 0.35f, 0.95f };
        anchorGO.AddComponent<scene::UIImage>(img);
        MakeText(*scene, anchorGO, "AnchorLabel", "ANCHOR OK",
                 { 1728.0f, 1030.0f }, 22.0f, { 0.95f, 1.0f, 0.95f, 1.0f });
    }

    // Window and editor camera setup.
    app.GetWindow().SetResizeCallback([&](uint32_t w, uint32_t h) {
        renderer.Resize(w, h);
    });

    renderer::DebugCamera debugCamera;
    debugCamera.camera.m_position = { 0.0f, 0.0f, -5.0f };
    debugCamera.camera.m_aspect   = 1920.0f / 1080.0f;
    editorApp.GetContext().editorCamera = &debugCamera.camera;

    // Main loop.
    while (app.IsRunning())
    {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose())
        {
            app.Quit();
            break;
        }

        const float dt = core::Time::DeltaTime();
        editorApp.BeginFrame();

        auto sceneRT = editorApp.GetViewportRT();
        auto gameRT  = editorApp.GetGameViewportRT();
        auto uiRT    = editorApp.GetUIViewportRT();

        if (auto* rt = resources.Get(sceneRT))
            debugCamera.camera.m_aspect =
                static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

        auto* pm = editorApp.GetContext().playMode;
        pm->ApplyPendingRestore(*scene);

        if (!pm->IsPlaying())
            debugCamera.Update(dt);

        scene::TransformSystem(*scene);
        scene::UIAnimatorSystem(*scene, dt);

        renderer::Camera gameCamera = debugCamera.camera;
        fbzz::LayerMask gameCullingMask = fbzz::Layer::Everything;
        if (auto* rt = resources.Get(gameRT))
            gameCamera.m_aspect =
                static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

        for (auto [tf, cc] : scene->View<scene::Transform, scene::CameraComponent>()) {
            if (!cc.enabled || !cc.isMain) continue;
            gameCamera.m_position = tf.position;
            gameCamera.m_rotation = tf.rotation;
            gameCamera.m_fovY    = cc.fovY;
            gameCamera.m_near    = cc.nearZ;
            gameCamera.m_far     = cc.farZ;
            gameCullingMask      = cc.cullingMask;
            break;
        }

        auto& ctx = editorApp.GetContext();
        scene::UICanvas* activeCanvas = FindFirstRootCanvas(*scene);

        renderer.BeginFrame();

        // Scene viewport: editor camera.
        renderer.SetRenderTarget(sceneRT, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.025f, 1.0f });
        scene::RenderSystem(*scene, renderer, resources,
                            debugCamera.camera, sceneRT,
                            &editorApp.GetContext().renderSettings);

        // UI viewport: UISystem preview only.
        if (uiRT.IsValid()) {
            renderer.SetRenderTarget(uiRT, resources);
            renderer.Clear({ 0.035f, 0.035f, 0.04f, 1.0f });

            math::Vector2 mouseInCanvas = { -100000.0f, -100000.0f };
            if (activeCanvas) {
                math::Vector2 mouse = input::Input::MousePosition();
                const float w = ctx.uiViewportWidth  > 0.0f ? ctx.uiViewportWidth  : 1.0f;
                const float h = ctx.uiViewportHeight > 0.0f ? ctx.uiViewportHeight : 1.0f;
                mouseInCanvas = {
                    (mouse.x - ctx.uiViewportOriginX) / w * activeCanvas->canvasWidth,
                    (mouse.y - ctx.uiViewportOriginY) / h * activeCanvas->canvasHeight
                };
            }
            scene::UISystem(*scene, renderer, resources,
                            ctx.uiViewportWidth, ctx.uiViewportHeight,
                            mouseInCanvas, false);
        }

        // Game viewport: 3D scene plus UI overlay.
        if (gameRT.IsValid()) {
            renderer.SetRenderTarget(gameRT, resources);
            renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });
            scene::RenderSystem(*scene, renderer, resources,
                                gameCamera, gameRT,
                                &editorApp.GetContext().renderSettings,
                                gameCullingMask);

            math::Vector2 mouse         = input::Input::MousePosition();
            math::Vector2 mouseInCanvas = mouse;
            if (activeCanvas) {
                const float w = ctx.gameViewportWidth  > 0.0f ? ctx.gameViewportWidth  : 1.0f;
                const float h = ctx.gameViewportHeight > 0.0f ? ctx.gameViewportHeight : 1.0f;
                mouseInCanvas = {
                    (mouse.x - ctx.gameViewportOriginX) / w * activeCanvas->canvasWidth,
                    (mouse.y - ctx.gameViewportOriginY) / h * activeCanvas->canvasHeight
                };
            }
            const bool mousePressed = input::Input::MouseButton(0);
            scene::UISystem(*scene, renderer, resources,
                            ctx.gameViewportWidth, ctx.gameViewportHeight,
                            mouseInCanvas, mousePressed);
        }

        // Editor UI.
        renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });
        editorApp.GetContext().activeScene = scene.get();
        editorApp.RenderPanels(editorApp.GetContext());
        editorApp.EndFrame(renderer);

        renderer.EndFrame();
    }

    editorApp.Shutdown();
    asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}
