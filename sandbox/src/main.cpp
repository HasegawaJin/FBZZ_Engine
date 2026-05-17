// FBZZ Engine
// sandbox/src/main.cpp
// Scene System テスト: 球が落下・バウンドする様子を DebugDraw + Log で確認
#include <engine/Core/Application.hpp>
#include <engine/Core/Time.hpp>
#include <engine/Core/Logger.hpp>
#include <engine/Input/Input.hpp>
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Renderer/Camera.hpp>
#include <engine/Renderer/DebugDraw.hpp>
#include <engine/Scene/Scene.hpp>
#include <engine/Scene/SceneManager.hpp>
#include <engine/Scene/Components/RigidBodyComponent.hpp>
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

    // ---------------------------------------------------------------- 物理ワールド
    // シーン外の static ボディ (床・台) はここで追加する。
    // シーン内の動的ボディはシーンファクトリ内で追加する。
    physics::World physWorld;

    // 床 (top y=0)
    auto floorBody = std::make_shared<physics::RigidBody>();
    floorBody->m_isStatic = true;
    floorBody->SetPosition({ 0.0f, -0.1f, 0.0f });
    floorBody->SetCollider(std::make_shared<physics::AABBCollider>(
        math::Vector3{ 10.0f, 0.1f, 10.0f }));
    physWorld.AddBody(floorBody);

    // 台 (ball が当たって跳ねる中間ターゲット)
    auto platformBody = std::make_shared<physics::RigidBody>();
    platformBody->m_isStatic = true;
    platformBody->SetPosition({ 0.5f, 1.5f, 0.0f });
    platformBody->SetCollider(std::make_shared<physics::AABBCollider>(
        math::Vector3{ 1.2f, 0.15f, 1.2f }));
    physWorld.AddBody(platformBody);

    // ---------------------------------------------------------------- シーン登録
    auto& sm = app.GetSceneManager();

    sm.Register("Test", [&physWorld]() -> std::unique_ptr<scene::Scene> {
        auto s = std::make_unique<scene::Scene>();

        // 球 GameObject — physics と Scene の両方に登録
        auto ballRb = std::make_shared<physics::RigidBody>();
        ballRb->SetMass(1.0f);
        ballRb->SetPosition({ 0.0f, 5.0f, 0.0f });
        ballRb->SetCollider(std::make_shared<physics::SphereCollider>(0.3f));
        ballRb->m_material = physics::PhysicsMaterial::Rubber;
        physWorld.AddBody(ballRb);

        auto& ball = s->CreateGameObject("Ball");
        ball.tag = "Ball";
        ball.AddComponent<scene::RigidBodyComponent>({ ballRb });

        // 親子関係テスト: Ball に子オブジェクトを付ける
        auto& shadow = s->CreateGameObject("BallShadow");
        shadow.transform.localPosition = { 0.0f, -0.4f, 0.0f }; // 少し下にオフセット
        shadow.SetParent(ball);

        FBZZ_LOG_INFO("Scene 'Test' loaded — %d GameObjects", 2);
        return s;
    });

    sm.LoadScene("Test");

    // ---------------------------------------------------------------- カメラ
    renderer::Camera camera;
    camera.m_position = { 0.0f, 3.5f, -9.0f };
    camera.m_aspect   = 1280.0f / 720.0f;
    camera.LookAt({ 0.5f, 2.0f, 0.0f });

    uint64_t frame = 0;
    bool enableDebugLog = true;  // false にすると毎秒ログを止める

    // ---------------------------------------------------------------- ゲームループ
    while (app.IsRunning())
    {
        core::Time::Tick();
        input::Input::Update();

        const float dt = core::Time::DeltaTime();

        // SceneManager が内部で PhysicsSystem → TransformSystem → FlushDestroyQueue を呼ぶ
        sm.Update(dt, renderer, physWorld);

        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        // ---- DebugDraw (視覚確認) ----------------------------------------
        renderer.BeginFrame();
        renderer.Clear({ 0.05f, 0.08f, 0.15f, 1.0f });

        renderer::DebugDraw::BeginFrame(renderer, camera.GetViewProjection());

        // 座標軸
        renderer::DebugDraw::Line(renderer, {0,0,0}, {1,0,0}, {1,0,0,1});
        renderer::DebugDraw::Line(renderer, {0,0,0}, {0,1,0}, {0,1,0,1});
        renderer::DebugDraw::Line(renderer, {0,0,0}, {0,0,1}, {0,0,1,1});

        // 物理ボディを描画
        for (auto& body : physWorld.GetBodies()) {
            auto col = body->GetCollider();
            if (!col) continue;
            if (col->GetType() == physics::ColliderType::SPHERE) {
                auto* s = static_cast<physics::SphereCollider*>(col.get());
                renderer::DebugDraw::Sphere(
                    renderer, body->GetPosition(), s->m_radius, {1.0f, 0.85f, 0.0f, 1.0f});
            } else if (col->GetType() == physics::ColliderType::AABB) {
                auto* a = static_cast<physics::AABBCollider*>(col.get());
                math::Vector4 color = body->IsStatic()
                    ? math::Vector4{0.55f, 0.55f, 0.55f, 1.0f}
                    : math::Vector4{0.2f,  0.75f, 1.0f,  1.0f};
                renderer::DebugDraw::Box(
                    renderer, body->GetPosition(), a->m_halfExtents, color);
            }
        }

        // Scene の GameObjects を走査して BallShadow の world position をラインで示す
        if (auto* activeScene = sm.GetActive()) {
            for (auto& go : activeScene->GameObjects()) {
                if (!go.activeSelf()) continue;
                // 親子関係の確認: BallShadow の world position を白点として描画
                if (go.name == "BallShadow") {
                    math::Vector3 p = go.transform.position;
                    renderer::DebugDraw::Sphere(renderer, p, 0.08f, {1,1,1,1});
                }
            }
        }

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

                    if (go.GetParent())
                        FBZZ_LOG_INFO("          parent=[%s]", go.GetParent()->name.c_str());
                }
            }
        }

        ++frame;
    }

    renderer::ShaderManager::Shutdown();
    app.Shutdown();
    return 0;
}
