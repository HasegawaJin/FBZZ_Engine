// FBZZ Engine
// sandbox/src/main.cpp
// 剛体テスト: キューブタワー積み + 階段スロープ転がし
#include <engine/Core/Application.hpp>
#include <engine/Core/Time.hpp>
#include <engine/Input/Input.hpp>
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Renderer/Camera.hpp>
#include <engine/Renderer/DebugDraw.hpp>
#include <physics/World.hpp>
#include <physics/RigidBody.hpp>
#include <physics/SphereCollider.hpp>
#include <physics/AABBCollider.hpp>

int main()
{
    auto& app = fbzz::core::Application::Get();
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    fbzz::renderer::ShaderManager::Init(&renderer);

    // ---------------------------------------------------------------- 物理セットアップ
    fbzz::physics::World physWorld;

    // 床 (static)  top = y 0.0
    {
        auto body = std::make_shared<fbzz::physics::RigidBody>();
        body->m_isStatic = true;
        body->SetPosition({ 0.0f, -0.1f, 0.0f });
        body->SetCollider(std::make_shared<fbzz::physics::AABBCollider>(
            fbzz::math::Vector3{ 9.0f, 0.1f, 2.5f }));
        physWorld.AddBody(body);
    }

    // ---- キューブタワー (左側 x = -4.0) ----
    // 各キューブ halfExtent = 0.3  →  床 top(0.0) + 0.3 = 0.4 が静止位置
    // 少し高めから落として積み重なりを確認する
    constexpr float towerX   = -4.0f;
    constexpr float cubeHalf = 0.3f;
    const float towerRestY[] = { 0.4f, 1.0f, 1.6f, 2.2f }; // 静止 Y
    for (int i = 0; i < 4; i++)
    {
        auto cube = std::make_shared<fbzz::physics::RigidBody>();
        cube->SetMass(1.0f);
        // Y を 0.4 だけ上乗せして落下させる / 微小な X ズレで崩れを演出
        cube->SetPosition({ towerX + i * 0.02f,
                            towerRestY[i] + 0.4f,
                            0.0f });
        cube->SetCollider(std::make_shared<fbzz::physics::AABBCollider>(
            fbzz::math::Vector3{ cubeHalf, cubeHalf, cubeHalf }));
        cube->m_material = fbzz::physics::PhysicsMaterial::Wood;
        physWorld.AddBody(cube);
    }

    // ---- 階段スロープ (右側、6段 static) ----
    // step i: center = (startX + i*stepW, stepH + i*stepRise, 0)
    constexpr float stepStartX = -1.0f;
    constexpr float stepW      =  0.9f;  // 段の幅 (halfExtent.x = 0.45)
    constexpr float stepRise   =  0.3f;  // 1段ごとの高さ
    constexpr float stepHalfX  =  0.45f;
    constexpr float stepHalfY  =  0.15f;
    for (int i = 0; i < 6; i++)
    {
        auto step = std::make_shared<fbzz::physics::RigidBody>();
        step->m_isStatic = true;
        step->SetPosition({
            stepStartX + i * stepW,
            stepHalfY + i * stepRise,   // 底面が床 y=0 に揃う y=0.15 から開始
            0.0f
        });
        step->SetCollider(std::make_shared<fbzz::physics::AABBCollider>(
            fbzz::math::Vector3{ stepHalfX, stepHalfY, 0.5f }));
        physWorld.AddBody(step);
    }

    // ---- 球 (スロープ頂上から転がす) ----
    // 最上段の top y = stepHalfY + 5*stepRise + stepHalfY = 0.3 + 1.5 = 1.8
    {
        constexpr float ballR = 0.3f;
        auto ball = std::make_shared<fbzz::physics::RigidBody>();
        ball->SetMass(1.0f);
        ball->SetPosition({
            stepStartX + 5 * stepW + stepHalfX - ballR,  // 最上段の右端付近
            stepHalfY * 2.0f + 5 * stepRise + ballR + 0.05f,
            0.0f
        });
        ball->SetCollider(std::make_shared<fbzz::physics::SphereCollider>(ballR));
        ball->m_material = fbzz::physics::PhysicsMaterial::Rubber;
        ball->SetVelocity({ -2.0f, 0.0f, 0.0f }); // 摩擦なしでも階段を降りるよう初速を付与
        physWorld.AddBody(ball);
    }

    // ---------------------------------------------------------------- カメラ
    // シーン全体 (x: -5 〜 5, y: 0 〜 3) が画角に収まる位置
    fbzz::renderer::Camera camera;
    camera.m_position = { 0.0f, 2.5f, -10.0f };
    camera.m_aspect   = 1280.0f / 720.0f;
    camera.LookAt({ 0.0f, 1.2f, 0.0f });

    // ---------------------------------------------------------------- ゲームループ
    while (app.IsRunning())
    {
        fbzz::core::Time::Tick();
        fbzz::input::Input::Update();

        physWorld.Step(fbzz::core::Time::DeltaTime());

        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        renderer.BeginFrame();
        renderer.Clear({ 0.05f, 0.08f, 0.15f, 1.0f });

        fbzz::renderer::DebugDraw::BeginFrame(renderer, camera.GetViewProjection());

        // 座標軸
        fbzz::renderer::DebugDraw::Line(renderer, {0,0,0}, {1,0,0}, {1,0,0,1});
        fbzz::renderer::DebugDraw::Line(renderer, {0,0,0}, {0,1,0}, {0,1,0,1});
        fbzz::renderer::DebugDraw::Line(renderer, {0,0,0}, {0,0,1}, {0,0,1,1});

        // 全剛体をコライダー種別で色分け描画
        for (auto& body : physWorld.GetBodies())
        {
            auto col = body->GetCollider();
            if (!col) continue;

            auto type = col->GetType();
            if (type == fbzz::physics::ColliderType::SPHERE)
            {
                auto* s = static_cast<fbzz::physics::SphereCollider*>(col.get());
                fbzz::renderer::DebugDraw::Sphere(
                    renderer, body->GetPosition(), s->m_radius,
                    { 1.0f, 0.85f, 0.0f, 1.0f }); // 黄
            }
            else if (type == fbzz::physics::ColliderType::AABB)
            {
                auto* a = static_cast<fbzz::physics::AABBCollider*>(col.get());
                fbzz::math::Vector4 color = body->IsStatic()
                    ? fbzz::math::Vector4{ 0.65f, 0.65f, 0.65f, 1.0f } // 静的: グレー
                    : fbzz::math::Vector4{ 0.2f,  0.75f, 1.0f,  1.0f }; // 動的: 水色
                fbzz::renderer::DebugDraw::Box(
                    renderer, body->GetPosition(), a->m_halfExtents, color);
            }
        }

        fbzz::renderer::DebugDraw::Flush();
        renderer.EndFrame();
    }

    fbzz::renderer::ShaderManager::Shutdown();
    app.Shutdown();
    return 0;
}
