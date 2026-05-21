// FBZZ Engine
// main.cpp | sandbox
// SceneSerializer-loaded physics verification scene
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/ShaderManager.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Editor/EditorApp.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/World.hpp>
#include <memory>
#include <string>

using namespace fbzz;

namespace {

constexpr const char* SCENE_NAME = "PhysicsTest";
constexpr const char* SCENE_PATH = "assets/scenes/PhysicsTest.fbzz";

void ResetPhysicsWorld(physics::World& world)
{
    world = physics::World();
    world.SetGravity({ 0.0f, -9.81f, 0.0f });
}

void AddBoxBody(scene::GameObject& go,
                physics::World& world,
                const math::Vector3& halfExtents,
                float mass,
                bool isStatic,
                const physics::PhysicsMaterial& material)
{
    auto body = std::make_shared<physics::RigidBody>();
    body->m_isStatic = isStatic;
    body->m_material = material;
    body->SetPosition(go.transform.localPosition);
    body->SetRotation(go.transform.localRotation);
    body->SetMass(mass);
    body->SetCollider(std::make_shared<physics::AABBCollider>(halfExtents));

    world.AddBody(body);
    go.AddComponent<scene::RigidBodyComponent>({ body, true });
}

void AttachPhysicsBodies(scene::Scene& scene, physics::World& world)
{
    if (auto* floor = scene.Find("Floor")) {
        AddBoxBody(*floor, world, { 12.0f, 0.25f, 8.0f }, 0.0f, true, physics::PhysicsMaterial::Stone);
    }

    if (auto* box = scene.Find("DropBox_Rubber")) {
        AddBoxBody(*box, world, { 0.5f, 0.5f, 0.5f }, 1.0f, false, physics::PhysicsMaterial::Rubber);
    }

    if (auto* box = scene.Find("DropBox_Wood")) {
        AddBoxBody(*box, world, { 0.5f, 0.5f, 0.5f }, 1.5f, false, physics::PhysicsMaterial::Wood);
    }

    if (auto* box = scene.Find("DropBox_Metal")) {
        AddBoxBody(*box, world, { 0.5f, 0.5f, 0.5f }, 2.5f, false, physics::PhysicsMaterial::Metal);
        if (auto* rb = box->GetComponent<scene::RigidBodyComponent>(); rb && rb->rigidBody)
            rb->rigidBody->ApplyImpulse({ -1.8f, 0.0f, 0.0f });
    }
}

} // namespace

int main()
{
    auto& app = core::Application::Get();
    if (!app.Init()) return 1;

    auto& renderer = app.GetRenderer();
    renderer::ShaderManager::Init(&renderer);

    physics::World physWorld;
    ResetPhysicsWorld(physWorld);

    auto& sm = app.GetSceneManager();
    sm.RegisterFromFile(SCENE_NAME, SCENE_PATH, renderer);
    sm.LoadScene(SCENE_NAME);
    sm.Update(0.0f, physWorld);

    if (auto* activeScene = sm.GetActive())
        AttachPhysicsBodies(*activeScene, physWorld);

    editor::EditorApp editorApp;
    editorApp.Init(renderer, app.GetWindow());
    editorApp.GetContext().activeScene = sm.GetActive();

    app.GetWindow().SetResizeCallback([&](uint32_t w, uint32_t h) {
        renderer.Resize(w, h);
    });

    renderer::DebugCamera debugCamera;
    debugCamera.camera.m_position = { 0.0f, 6.0f, -12.0f };
    debugCamera.camera.m_aspect = 1280.0f / 720.0f;
    debugCamera.LookAt({ 0.0f, 2.0f, 0.0f });
    editorApp.GetContext().editorCamera = &debugCamera.camera;

    while (app.IsRunning())
    {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) {
            app.Quit();
            break;
        }

        const float dt = core::Time::DeltaTime();

        editorApp.BeginFrame();

        bool sceneRestored = false;
        if (auto* activeScene = sm.GetActive()) {
            if (editorApp.GetContext().playMode &&
                editorApp.GetContext().playMode->HasPendingRestore())
            {
                sceneRestored = editorApp.GetContext().playMode->ApplyPendingRestore(*activeScene);
                editorApp.GetContext().selectedEntities.clear();
                editorApp.GetContext().activeScene = sm.GetActive();
                ResetPhysicsWorld(physWorld);
                if (auto* restoredScene = sm.GetActive())
                    AttachPhysicsBodies(*restoredScene, physWorld);
            }
        }

        auto vpRT = editorApp.GetViewportRT();
        debugCamera.camera.m_aspect = vpRT
            ? static_cast<float>(vpRT->GetWidth()) / static_cast<float>(vpRT->GetHeight())
            : 1280.0f / 720.0f;
        debugCamera.Update(dt);

        if (!sceneRestored)
            sm.Update(dt, physWorld);

        renderer.BeginFrame();

        if (auto* activeScene = sm.GetActive()) {
            scene::RenderSystem(*activeScene,
                                renderer,
                                debugCamera.camera,
                                vpRT,
                                &editorApp.GetContext().renderSettings);
        }

        renderer::DebugDraw::BeginFrame(renderer, debugCamera.camera.GetViewProjection());
        renderer::DebugDraw::Line(renderer, { 0, 0, 0 }, { 2, 0, 0 }, { 1, 0, 0, 1 });
        renderer::DebugDraw::Line(renderer, { 0, 0, 0 }, { 0, 2, 0 }, { 0, 1, 0, 1 });
        renderer::DebugDraw::Line(renderer, { 0, 0, 0 }, { 0, 0, 2 }, { 0, 0, 1, 1 });
        renderer::DebugDraw::Flush();

        renderer.SetRenderTarget(nullptr);
        renderer.Clear({ 0.02f, 0.02f, 0.02f, 1.0f });

        editorApp.GetContext().activeScene = sm.GetActive();
        editorApp.RenderPanels(editorApp.GetContext());
        editorApp.EndFrame(renderer);

        renderer.EndFrame();
    }

    editorApp.Shutdown();
    renderer::ShaderManager::Shutdown();
    app.Shutdown();
    return 0;
}
