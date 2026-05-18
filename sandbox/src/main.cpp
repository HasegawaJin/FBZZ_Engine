// FBZZ Engine
// sandbox/src/main.cpp
// レンダーテストシーン — マテリアルシェーダー比較
#include <engine/Core/Application.hpp>
#include <engine/Core/Time.hpp>
#include <engine/Input/Input.hpp>
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Renderer/Camera.hpp>
#include <engine/Renderer/DebugCamera.hpp>
#include <engine/Renderer/DebugDraw.hpp>
#include <engine/Renderer/PrimitiveMesh.hpp>
#include <engine/Renderer/Material.hpp>
#include <engine/Renderer/LightSystem.hpp>
#include <engine/Scene/Scene.hpp>
#include <engine/Scene/SceneManager.hpp>
#include <engine/Scene/Systems/RenderSystem.hpp>
#include <engine/Scene/Components/MeshRenderer.hpp>
#include <physics/World.hpp>

using namespace fbzz;

namespace {

std::shared_ptr<renderer::Material> MakeMat(
    renderer::IRenderer& r,
    std::shared_ptr<renderer::IShader> shader,
    math::Vector4 albedo,
    float metallic  = 0.0f,
    float roughness = 0.5f)
{
    auto mat = std::make_shared<renderer::Material>();
    mat->shader           = shader;
    mat->params.albedo    = albedo;
    mat->params.metallic  = metallic;
    mat->params.roughness = roughness;
    mat->Init(r);
    return mat;
}

} // namespace

int main()
{
    auto& app = core::Application::Get();
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    renderer::ShaderManager::Init(&renderer);

    // ---------------------------------------------------------------- シェーダー
    auto shaderMesh       = renderer::ShaderManager::Load("assets/shaders/Mesh.hlsl");
    auto shaderUnlit      = renderer::ShaderManager::Load("assets/shaders/Material/Unlit.hlsl");
    auto shaderLit        = renderer::ShaderManager::Load("assets/shaders/Material/Lit.hlsl");
    auto shaderPhong      = renderer::ShaderManager::Load("assets/shaders/Material/Phong.hlsl");
    auto shaderBlinnPhong = renderer::ShaderManager::Load("assets/shaders/Material/BlinnPhong.hlsl");
    auto shaderPBR        = renderer::ShaderManager::Load("assets/shaders/Material/PBR.hlsl");

    // ---------------------------------------------------------------- メッシュ
    auto sphereMesh = renderer::PrimitiveMesh::Sphere(renderer, 32);
    auto cubeMesh   = renderer::PrimitiveMesh::Cube(renderer);
    auto planeMesh  = renderer::PrimitiveMesh::Plane(renderer);

    // ---------------------------------------------------------------- ライト
    renderer::LightSystem lights;
    lights.SetDirectional({
        { -0.4f, -0.8f, 0.45f },
        0.0f,
        { 1.0f, 0.98f, 0.9f },
        1.5f
    });

    // ---------------------------------------------------------------- 物理ワールド (ダミー: ボディなし)
    physics::World physWorld;

    // ---------------------------------------------------------------- シーン
    auto& sm = app.GetSceneManager();

    sm.Register("RenderTest", [&]() -> std::unique_ptr<scene::Scene> {
        auto s = std::make_unique<scene::Scene>();
        constexpr float Y = 1.0f;

        // ================================================================
        // 行 1 (Z=0): シェーダー比較
        //   Unlit / Lit / Phong / BlinnPhong / PBR を横に並べる
        // ================================================================
        struct ShaderEntry {
            std::shared_ptr<renderer::IShader> shader;
            math::Vector4 color;
            const char* name;
        };
        ShaderEntry row1[] = {
            { shaderUnlit,      { 1.0f, 0.3f, 0.3f, 1.0f }, "Unlit"      },
            { shaderLit,        { 0.3f, 0.9f, 0.3f, 1.0f }, "Lit"        },
            { shaderPhong,      { 0.3f, 0.5f, 1.0f, 1.0f }, "Phong"      },
            { shaderBlinnPhong, { 1.0f, 0.85f, 0.2f, 1.0f }, "BlinnPhong" },
            { shaderPBR,        { 0.85f, 0.85f, 0.85f, 1.0f }, "PBR"     },
        };
        for (int i = 0; i < 5; ++i) {
            auto& e = row1[i];
            auto& go = s->CreateGameObject(e.name);
            go.transform.localPosition = { (i - 2) * 3.0f, Y, 0.0f };
            go.transform.localScale    = { 0.9f, 0.9f, 0.9f };
            go.AddComponent<scene::MeshRenderer>({ sphereMesh,
                MakeMat(renderer, e.shader, e.color, 0.0f, 0.3f) });
        }

        // ================================================================
        // 行 2 (Z=4): PBR roughness 0 → 1 (metallic=0, 紫系)
        // ================================================================
        for (int i = 0; i < 5; ++i) {
            auto& go = s->CreateGameObject("PBR_Rough_" + std::to_string(i));
            go.transform.localPosition = { (i - 2) * 3.0f, Y, 4.0f };
            go.transform.localScale    = { 0.9f, 0.9f, 0.9f };
            go.AddComponent<scene::MeshRenderer>({ sphereMesh,
                MakeMat(renderer, shaderPBR,
                    { 0.7f, 0.2f, 0.9f, 1.0f },
                    0.0f, i * 0.25f) });
        }

        // ================================================================
        // 行 3 (Z=8): PBR metallic 0 → 1 (roughness=0.2, 金系)
        // ================================================================
        for (int i = 0; i < 5; ++i) {
            auto& go = s->CreateGameObject("PBR_Metal_" + std::to_string(i));
            go.transform.localPosition = { (i - 2) * 3.0f, Y, 8.0f };
            go.transform.localScale    = { 0.9f, 0.9f, 0.9f };
            go.AddComponent<scene::MeshRenderer>({ sphereMesh,
                MakeMat(renderer, shaderPBR,
                    { 1.0f, 0.78f, 0.34f, 1.0f },
                    i * 0.25f, 0.2f) });
        }

        // ================================================================
        // 床 (Lit)
        // ================================================================
        auto& floor = s->CreateGameObject("Floor");
        floor.transform.localPosition = { 0.0f, 0.0f, 4.0f };
        floor.transform.localScale    = { 30.0f, 0.2f, 22.0f };
        floor.AddComponent<scene::MeshRenderer>({ cubeMesh,
            MakeMat(renderer, shaderLit, { 0.22f, 0.22f, 0.22f, 1.0f }, 0.0f, 1.0f) });

        return s;
    });

    sm.LoadScene("RenderTest");

    // ---------------------------------------------------------------- カメラ
    renderer::DebugCamera debugCamera;
    debugCamera.camera.m_position = { 0.0f, 6.0f, -14.0f };
    debugCamera.camera.m_aspect   = 1280.0f / 720.0f;
    debugCamera.LookAt({ 0.0f, 1.0f, 4.0f });

    // ---------------------------------------------------------------- ゲームループ
    while (app.IsRunning())
    {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        const float dt = core::Time::DeltaTime();
        debugCamera.Update(dt);
        sm.Update(dt, physWorld);

        renderer.BeginFrame();
        renderer.Clear({ 0.05f, 0.08f, 0.15f, 1.0f });

        if (auto* activeScene = sm.GetActive())
            scene::RenderSystem(*activeScene, renderer, debugCamera.camera, lights);

        renderer::DebugDraw::BeginFrame(renderer, debugCamera.camera.GetViewProjection());
        renderer::DebugDraw::Line(renderer, {0,0,0}, {1,0,0}, {1,0,0,1});
        renderer::DebugDraw::Line(renderer, {0,0,0}, {0,1,0}, {0,1,0,1});
        renderer::DebugDraw::Line(renderer, {0,0,0}, {0,0,1}, {0,0,1,1});
        renderer::DebugDraw::Flush();

        renderer.EndFrame();
    }

    renderer::ShaderManager::Shutdown();
    app.Shutdown();
    return 0;
}
