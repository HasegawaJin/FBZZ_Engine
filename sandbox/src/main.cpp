// FBZZ Engine
// sandbox/src/main.cpp
// Scene System + Mesh Renderer テスト: 球が落下・バウンドし、Cube が静止する様子を確認
#include <engine/Core/Application.hpp>
#include <engine/Core/Time.hpp>
#include <engine/Core/Logger.hpp>
#include <engine/Input/Input.hpp>
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Renderer/Camera.hpp>
#include <engine/Renderer/DebugDraw.hpp>
#include <engine/Renderer/PrimitiveMesh.hpp>
#include <engine/Renderer/Material.hpp>
#include <engine/Renderer/LightSystem.hpp>
#include <engine/Scene/Scene.hpp>
#include <engine/Scene/SceneManager.hpp>
#include <engine/Scene/Systems/RenderSystem.hpp>
#include <engine/Scene/Components/RigidBodyComponent.hpp>
#include <engine/Scene/Components/MeshRenderer.hpp>
#include <engine/Asset/AssetManager.hpp>
#include <engine/Asset/Model.hpp>
#include <engine/Renderer/ITexture.hpp>
#include <physics/World.hpp>
#include <physics/RigidBody.hpp>
#include <physics/SphereCollider.hpp>
#include <physics/AABBCollider.hpp>

using namespace fbzz;

int main()
{
    auto& app = core::Application::Get();
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    renderer::ShaderManager::Init(&renderer);
    asset::AssetManager::Init(renderer, "assets/");

    // ---------------------------------------------------------------- 物理ワールド
    physics::World physWorld;

    auto floorBody = std::make_shared<physics::RigidBody>();
    floorBody->m_isStatic = true;
    floorBody->SetPosition({ 0.0f, -0.1f, 0.0f });
    floorBody->SetCollider(std::make_shared<physics::AABBCollider>(
        math::Vector3{ 10.0f, 0.1f, 10.0f }));
    physWorld.AddBody(floorBody);

    auto platformBody = std::make_shared<physics::RigidBody>();
    platformBody->m_isStatic = true;
    platformBody->SetPosition({ 0.5f, 1.5f, 0.0f });
    platformBody->SetCollider(std::make_shared<physics::AABBCollider>(
        math::Vector3{ 1.2f, 0.15f, 1.2f }));
    physWorld.AddBody(platformBody);

    // ---------------------------------------------------------------- メッシュ・マテリアル
    auto meshShader = renderer::ShaderManager::Load("assets/shaders/Mesh.hlsl");

    auto cubeMesh   = renderer::PrimitiveMesh::Cube(renderer);
    auto sphereMesh = renderer::PrimitiveMesh::Sphere(renderer, 24);
    auto planeMesh  = renderer::PrimitiveMesh::Plane(renderer);
    auto cylinderMesh = renderer::PrimitiveMesh::Cylinder(renderer, 24);
    auto torusMesh = renderer::PrimitiveMesh::Torus(renderer, 24);
    auto coneMesh = renderer::PrimitiveMesh::Cone(renderer, 24);
    auto capsuleMesh = renderer::PrimitiveMesh::Capsule(renderer, 24);

    // 球マテリアル (黄色)
    auto ballMat = std::make_shared<renderer::Material>();
    ballMat->shader         = meshShader;
    ballMat->params.albedo  = { 1.0f, 0.85f, 0.0f, 1.0f };
    ballMat->params.roughness = 0.4f;
    ballMat->Init(renderer);

    // 台マテリアル (灰色)
    auto platformMat = std::make_shared<renderer::Material>();
    platformMat->shader          = meshShader;
    platformMat->params.albedo   = { 0.55f, 0.55f, 0.55f, 1.0f };
    platformMat->params.roughness = 0.9f;
    platformMat->Init(renderer);

    // 床マテリアル (暗い灰色)
    auto floorMat = std::make_shared<renderer::Material>();
    floorMat->shader          = meshShader;
    floorMat->params.albedo   = { 0.3f, 0.3f, 0.3f, 1.0f };
    floorMat->params.roughness = 1.0f;
    floorMat->Init(renderer);

    // ---------------------------------------------------------------- ライト
    renderer::LightSystem lights;
    // lightDir: 光が進む方向 (シェーダーで L = -lightDir)
    // カメラは -Z 側から +Z へ向けて見ている。
    // 上方かつカメラ方向(-Z側)から来る光にすることで
    // トップ面・カメラ向き面が両方明るくなる
    lights.SetDirectional({
        { -0.4f, -0.8f,  0.45f },  // 右上から前方(カメラ側)へ
        0.0f,
        { 1.0f, 0.98f, 0.9f },
        1.2f
    });

    // ---------------------------------------------------------------- シーン
    auto& sm = app.GetSceneManager();

    // ---------------------------------------------------------------- テクスチャロード
    auto testTex = asset::AssetManager::Load<renderer::ITexture>("textures/test.jpg");

    // ---------------------------------------------------------------- FBX ロード
    auto fbxModel = asset::AssetManager::Load<asset::Model>("models/test.fbx");
    if (fbxModel) {
        // ModelImporter はシェーダーを設定しないため、ここで注入する
        for (auto& mat : fbxModel->materials)
            if (mat) mat->shader = meshShader;
    }

    sm.Register("Test", [&]() -> std::unique_ptr<scene::Scene> {
        auto s = std::make_unique<scene::Scene>();

        // ---- テクスチャ確認用 Plane ----------------------------------------
        auto& texPlane = s->CreateGameObject("TexturePlane");
        texPlane.transform.localPosition = { -4.0f, 1.0f, 0.0f };
        texPlane.transform.localScale    = { 2.0f, 2.0f, 2.0f };
        auto texMat = std::make_shared<renderer::Material>();
        texMat->shader        = meshShader;
        texMat->albedoTexture = testTex;
        texMat->params.albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        texMat->Init(renderer);
        texPlane.AddComponent<scene::MeshRenderer>({ planeMesh, texMat });

        // ---- FBX モデル配置 ----------------------------------------
        if (fbxModel) {
            for (size_t i = 0; i < fbxModel->meshes.size(); ++i) {
                auto& go = s->CreateGameObject("FBX_" + std::to_string(i));
                go.transform.localPosition = { 4.0f, 0.5f, 0.0f };
                go.transform.localScale    = { 1.0f, 1.0f, 1.0f };
                go.AddComponent<scene::MeshRenderer>(
                    { fbxModel->meshes[i], fbxModel->materials[i] });
            }
        }

        // 球
        math::Vector3 spawnPos = { 0.0f, 5.0f, 0.0f };
        auto ballRb = std::make_shared<physics::RigidBody>();
        ballRb->SetMass(1.0f);
        ballRb->SetPosition(spawnPos);
        ballRb->SetCollider(std::make_shared<physics::SphereCollider>(0.3f));
        ballRb->m_material = physics::PhysicsMaterial::Rubber;
        physWorld.AddBody(ballRb);

        auto& ball = s->CreateGameObject("Ball");
        ball.tag = "Ball";
        ball.AddComponent<scene::RigidBodyComponent>({ ballRb });
        ball.AddComponent<scene::MeshRenderer>({ sphereMesh, ballMat });
        ball.transform.localScale = { 0.6f, 0.6f, 0.6f };

        // 台 (静止 Cube)
        auto& platform = s->CreateGameObject("Platform");
        platform.transform.localPosition = { 0.5f, 1.5f, 0.0f };
        platform.transform.localScale    = { 2.4f, 0.3f, 2.4f };
        platform.AddComponent<scene::MeshRenderer>({ cubeMesh, platformMat });

        // 床 (静止 Cube)
        auto& floor = s->CreateGameObject("Floor");
        floor.transform.localPosition = { 0.0f, -0.1f, 0.0f };
        floor.transform.localScale    = { 20.0f, 0.2f, 20.0f };
        floor.AddComponent<scene::MeshRenderer>({ cubeMesh, floorMat });

            // 追加プリミティブ: Cube / Cylinder / Torus / Cone / Capsule
            auto& cubeDisplay = s->CreateGameObject("Cube_Display");
            cubeDisplay.transform.localPosition = { -6.0f, spawnPos.y - 2.5f, 0.0f };
            cubeDisplay.transform.localScale = { 0.8f, 0.8f, 0.8f };
            auto cubeMat = std::make_shared<renderer::Material>();
            cubeMat->shader = meshShader; cubeMat->params.albedo = { 0.9f, 0.2f, 0.2f, 1.0f };
            cubeMat->params.roughness = 0.6f; cubeMat->Init(renderer);
            cubeDisplay.AddComponent<scene::MeshRenderer>({ cubeMesh, cubeMat });

            auto& cylDisplay = s->CreateGameObject("Cylinder_Display");
            cylDisplay.transform.localPosition = { -3.0f, spawnPos.y - 2.5f, 0.0f };
            cylDisplay.transform.localScale = { 0.8f, 0.8f, 0.8f };
            auto cylMat = std::make_shared<renderer::Material>();
            cylMat->shader = meshShader; cylMat->params.albedo = { 0.2f, 0.9f, 0.2f, 1.0f };
            cylMat->params.roughness = 0.5f; cylMat->Init(renderer);
            cylDisplay.AddComponent<scene::MeshRenderer>({ cylinderMesh, cylMat });

            auto& torusDisplay = s->CreateGameObject("Torus_Display");
            torusDisplay.transform.localPosition = { 0.0f, spawnPos.y - 2.5f, 0.0f };
            torusDisplay.transform.localScale = { 0.8f, 0.8f, 0.8f };
            auto torusMat = std::make_shared<renderer::Material>();
            torusMat->shader = meshShader; torusMat->params.albedo = { 0.2f, 0.4f, 0.9f, 1.0f };
            torusMat->params.roughness = 0.3f; torusMat->Init(renderer);
            torusDisplay.AddComponent<scene::MeshRenderer>({ torusMesh, torusMat });

            auto& coneDisplay = s->CreateGameObject("Cone_Display");
            coneDisplay.transform.localPosition = { 3.0f, spawnPos.y - 2.5f, 0.0f };
            coneDisplay.transform.localScale = { 0.8f, 0.8f, 0.8f };
            auto coneMat = std::make_shared<renderer::Material>();
            coneMat->shader = meshShader; coneMat->params.albedo = { 0.9f, 0.2f, 0.9f, 1.0f };
            coneMat->params.roughness = 0.4f; coneMat->Init(renderer);
            coneDisplay.AddComponent<scene::MeshRenderer>({ coneMesh, coneMat });

            auto& capsuleDisplay = s->CreateGameObject("Capsule_Display");
            capsuleDisplay.transform.localPosition = { 6.0f, spawnPos.y - 2.5f, 0.0f };
            capsuleDisplay.transform.localScale = { 0.8f, 0.8f, 0.8f };
            auto capsuleMat = std::make_shared<renderer::Material>();
            capsuleMat->shader = meshShader; capsuleMat->params.albedo = { 0.2f, 0.9f, 0.9f, 1.0f };
            capsuleMat->params.roughness = 0.4f; capsuleMat->Init(renderer);
            capsuleDisplay.AddComponent<scene::MeshRenderer>({ capsuleMesh, capsuleMat });

        return s;
    });

    sm.LoadScene("Test");

    // ---------------------------------------------------------------- カメラ
    renderer::Camera camera;
    camera.m_position = { 0.0f, 3.5f, -9.0f };
    camera.m_aspect   = 1280.0f / 720.0f;
    camera.LookAt({ 0.5f, 2.0f, 0.0f });

    uint64_t frame      = 0;
    bool enableDebugLog = true;

    // ---------------------------------------------------------------- ゲームループ
    while (app.IsRunning())
    {
        core::Time::Tick();
        input::Input::Update();

        const float dt = core::Time::DeltaTime();

        sm.Update(dt, physWorld);

        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        // ---- 描画 --------------------------------------------------------
        renderer.BeginFrame();
        renderer.Clear({ 0.05f, 0.08f, 0.15f, 1.0f });

        // メッシュ描画 (RenderSystem)
        if (auto* activeScene = sm.GetActive())
            scene::RenderSystem(*activeScene, renderer, camera, lights);

        // DebugDraw (座標軸・物理コライダー)
        renderer::DebugDraw::BeginFrame(renderer, camera.GetViewProjection());
        renderer::DebugDraw::Line(renderer, {0,0,0}, {1,0,0}, {1,0,0,1});
        renderer::DebugDraw::Line(renderer, {0,0,0}, {0,1,0}, {0,1,0,1});
        renderer::DebugDraw::Line(renderer, {0,0,0}, {0,0,1}, {0,0,1,1});
        renderer::DebugDraw::Flush();

        renderer.EndFrame();

        // ---- Log (1秒ごと) ------------------------------------------------
        static float logTimer = 0.0f;
        logTimer += dt;
        if (enableDebugLog && logTimer >= 1.0f) {
            logTimer -= 1.0f;

            if (auto* activeScene = sm.GetActive()) {
                FBZZ_LOG_INFO("=== t=%.1fs  frame=%llu ===",
                    core::Time::TotalTime(), frame);

                for (auto& go : activeScene->GameObjects()) {
                    const auto& tf = go.transform;
                    FBZZ_LOG_INFO("  [%s]  world=(%.3f, %.3f, %.3f)",
                        go.name.c_str(),
                        tf.position.x, tf.position.y, tf.position.z);

                    if (auto* rbc = go.GetComponent<scene::RigidBodyComponent>()) {
                        if (rbc->rigidBody) {
                            auto vel = rbc->rigidBody->GetVelocity();
                            FBZZ_LOG_INFO("          vel  =(%.3f, %.3f, %.3f)",
                                vel.x, vel.y, vel.z);
                        }
                    }
                }
            }
        }

        ++frame;
    }

    asset::AssetManager::UnloadAll();
    renderer::ShaderManager::Shutdown();
    app.Shutdown();
    return 0;
}
