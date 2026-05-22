// FBZZ Engine
// main.cpp | sandbox
// Physics Volume / Constraint / CapsuleCollider verification scene
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Editor/EditorApp.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/ChainConstraint.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/HingeConstraint.hpp>
#include <Physics/RopeConstraint.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/SpringConstraint.hpp>
#include <Physics/World.hpp>
#include <Math/Vector4.hpp>
#include <memory>
#include <utility>
#include <vector>

using namespace fbzz;

namespace
{
    struct BodyDebug
    {
        std::shared_ptr<physics::RigidBody> body;
        std::shared_ptr<physics::Collider> collider;
        scene::GameObject* gameObject = nullptr;
        math::Vector4 color;
    };

    struct VisualAssets
    {
        std::shared_ptr<renderer::Mesh> cube;
        std::shared_ptr<renderer::Mesh> sphere;
        std::shared_ptr<renderer::Mesh> capsule;
        std::shared_ptr<renderer::Material> whiteMaterial;
    };

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

    scene::GameObject& AddVisual(scene::Scene& scene,
                                 const char* name,
                                 const std::shared_ptr<renderer::Mesh>& mesh,
                                 const char* meshPath,
                                 const std::shared_ptr<renderer::Material>& material,
                                 const math::Vector3& position,
                                 const math::Vector3& scale)
    {
        scene::GameObject& go = scene.CreateGameObject(name);
        go.transform.localPosition = position;
        go.transform.localScale = scale;
        scene::MeshRenderer meshRenderer{};
        meshRenderer.mesh = mesh;
        meshRenderer.material = material;
        meshRenderer.enabled = true;
        meshRenderer.meshPath = meshPath;
        meshRenderer.shaderPath = material ? material->shaderPath : std::string{};
        go.AddComponent<scene::MeshRenderer>(std::move(meshRenderer));
        return go;
    }

    std::shared_ptr<physics::RigidBody> AddSphere(physics::World& world,
                                                  scene::Scene& scene,
                                                  std::vector<BodyDebug>& debugBodies,
                                                  const VisualAssets& assets,
                                                  const char* name,
                                                  const math::Vector3& position,
                                                  float radius,
                                                  float mass,
                                                  const math::Vector4& color,
                                                  bool isStatic = false)
    {
        auto body = std::make_shared<physics::RigidBody>();
        body->m_isStatic = isStatic;
        body->SetPosition(position);
        body->SetMass(mass);
        auto collider = std::make_shared<physics::SphereCollider>(radius);
        scene::GameObject& go = AddVisual(scene, name, assets.sphere, "primitive:sphere", assets.whiteMaterial,
                                          position, math::Vector3::ONE * (radius * 2.0f));
        go.AddComponent<scene::RigidBodyComponent>({ body, true });
        go.AddComponent<scene::ColliderComponent>({ collider, physics::PhysicsMaterial::Default, false, true });
        debugBodies.push_back({ body, collider, &go, color });
        return body;
    }

    std::shared_ptr<physics::RigidBody> AddBox(physics::World& world,
                                               scene::Scene& scene,
                                               std::vector<BodyDebug>& debugBodies,
                                               const VisualAssets& assets,
                                               const char* name,
                                               const math::Vector3& position,
                                               const math::Vector3& halfExtents,
                                               float mass,
                                               const math::Vector4& color,
                                               bool isStatic = false)
    {
        auto body = std::make_shared<physics::RigidBody>();
        body->m_isStatic = isStatic;
        body->SetPosition(position);
        body->SetMass(mass);
        auto collider = std::make_shared<physics::AABBCollider>(halfExtents);
        scene::GameObject& go = AddVisual(scene, name, assets.cube, "primitive:cube", assets.whiteMaterial,
                                          position, halfExtents * 2.0f);
        go.AddComponent<scene::RigidBodyComponent>({ body, true });
        go.AddComponent<scene::ColliderComponent>({ collider, physics::PhysicsMaterial::Default, false, true });
        debugBodies.push_back({ body, collider, &go, color });
        return body;
    }

    std::shared_ptr<physics::RigidBody> AddCapsule(physics::World& world,
                                                  scene::Scene& scene,
                                                  std::vector<BodyDebug>& debugBodies,
                                                  const VisualAssets& assets,
                                                  const char* name,
                                                  const math::Vector3& position,
                                                  float radius,
                                                  float halfHeight,
                                                  float mass,
                                                  const math::Vector4& color)
    {
        auto body = std::make_shared<physics::RigidBody>();
        body->SetPosition(position);
        body->SetMass(mass);
        auto collider = std::make_shared<physics::CapsuleCollider>(radius, halfHeight);
        scene::GameObject& go = AddVisual(scene, name, assets.capsule, "primitive:capsule", assets.whiteMaterial,
                                          position, { radius * 4.0f, (halfHeight + radius) * 2.0f, radius * 4.0f });
        go.AddComponent<scene::RigidBodyComponent>({ body, true });
        go.AddComponent<scene::ColliderComponent>({ collider, physics::PhysicsMaterial::Default, false, true });
        debugBodies.push_back({ body, collider, &go, color });
        return body;
    }

    void SyncVisuals(const std::vector<BodyDebug>& debugBodies)
    {
        for (const BodyDebug& debug : debugBodies)
        {
            if (!debug.gameObject) continue;
            debug.gameObject->transform.localPosition = debug.body->GetPosition();
            debug.gameObject->transform.localRotation = debug.body->GetRotation();
        }
    }

    void DrawBody(renderer::IRenderer& renderer, const BodyDebug& debug)
    {
        const auto collider = debug.collider;
        if (!collider) return;

        const math::Vector3 position = debug.body->GetPosition();
        if (collider->GetType() == physics::ColliderType::SPHERE)
        {
            auto* sphere = static_cast<physics::SphereCollider*>(collider.get());
            renderer::DebugDraw::Sphere(renderer, position, sphere->m_radius, debug.color);
        }
        else if (collider->GetType() == physics::ColliderType::AABB)
        {
            auto* box = static_cast<physics::AABBCollider*>(collider.get());
            renderer::DebugDraw::Box(renderer, position, box->m_halfExtents, debug.color);
        }
        else if (collider->GetType() == physics::ColliderType::CAPSULE)
        {
            auto* capsule = static_cast<physics::CapsuleCollider*>(collider.get());
            renderer::DebugDraw::Capsule(renderer, position, capsule->m_radius, capsule->m_halfHeight, debug.color);
        }
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

    physics::World world;
    world.SetGravity({ 0.0f, -9.81f, 0.0f });

    auto scene = std::make_unique<scene::Scene>();
    editorApp.GetContext().activeScene = scene.get();

    VisualAssets assets;
    assets.cube = renderer::PrimitiveMesh::Cube(resources);
    assets.sphere = renderer::PrimitiveMesh::Sphere(resources, 16);
    assets.capsule = renderer::PrimitiveMesh::Capsule(resources, 16);
    assets.whiteMaterial = CreateMaterial(resources, { 0.78f, 0.78f, 0.82f, 1.0f });

    auto& light = scene->CreateGameObject("DirectionalLight");
    light.transform.localPosition = { 0.0f, 8.0f, -6.0f };
    light.transform.localRotation = math::Quaternion::LookRotation({ 0.35f, -0.75f, 0.55f });
    light.AddComponent<scene::LightComponent>({});

    auto& mainCamera = scene->CreateGameObject("MainCamera");
    mainCamera.transform.localPosition = { 0.0f, 4.0f, -12.0f };
    mainCamera.transform.localRotation = math::Quaternion::LookRotation(math::Vector3{ 0.0f, -0.22f, 1.0f }.Normalized());
    mainCamera.AddComponent<scene::CameraComponent>({});

    std::vector<BodyDebug> bodies;
    AddBox(world, *scene, bodies, assets, "Ground", { 0.0f, -0.5f, 0.0f }, { 26.0f, 0.5f, 16.0f }, 0.0f, { 0.45f, 0.45f, 0.45f, 1.0f }, true);

    auto capsule = AddCapsule(world, *scene, bodies, assets, "CapsuleToSphere", { -18.0f, 4.0f, 7.0f }, 0.25f, 0.6f, 1.0f, { 1.0f, 0.85f, 0.2f, 1.0f });
    auto capsuleTarget = AddSphere(world, *scene, bodies, assets, "CapsuleSphereTarget", { -15.6f, 1.0f, 7.0f }, 0.32f, 1.0f, { 1.0f, 0.55f, 0.2f, 1.0f });
    auto capsulePairA = AddCapsule(world, *scene, bodies, assets, "CapsulePairA", { -11.0f, 3.5f, 7.0f }, 0.22f, 0.55f, 1.0f, { 1.0f, 0.95f, 0.35f, 1.0f });
    auto capsulePairB = AddCapsule(world, *scene, bodies, assets, "CapsulePairB", { -9.6f, 3.5f, 7.0f }, 0.22f, 0.55f, 1.0f, { 1.0f, 0.65f, 0.25f, 1.0f });

    auto gravityBody = AddSphere(world, *scene, bodies, assets, "GravityAreaBody", { -14.0f, 3.8f, 0.0f }, 0.28f, 1.0f, { 0.2f, 0.7f, 1.0f, 1.0f });
    auto vortexBody = AddSphere(world, *scene, bodies, assets, "VortexAreaBody", { -8.0f, 3.5f, 0.9f }, 0.28f, 1.0f, { 0.9f, 0.35f, 1.0f, 1.0f });
    auto buoyantBody = AddSphere(world, *scene, bodies, assets, "BuoyancyAreaBody", { -2.0f, 1.3f, 0.0f }, 0.28f, 0.8f, { 0.2f, 1.0f, 0.8f, 1.0f });
    auto timeBody = AddSphere(world, *scene, bodies, assets, "TimeDilationAreaBody", { 4.0f, 3.2f, 0.0f }, 0.28f, 1.0f, { 0.7f, 0.7f, 1.0f, 1.0f });
    auto magneticBody = AddSphere(world, *scene, bodies, assets, "MagneticAreaBody", { 8.0f, 3.0f, -0.9f }, 0.28f, 1.0f, { 1.0f, 0.35f, 0.35f, 1.0f });
    magneticBody->m_charge = 8.0f;
    magneticBody->SetVelocity({ 2.0f, 0.0f, 0.0f });

    auto& gravityVolume = scene->CreateGameObject("GravityArea");
    gravityVolume.transform.localPosition = { -14.0f, 1.7f, 0.0f };
    gravityVolume.AddComponent<scene::ColliderComponent>({
        std::make_shared<physics::AABBCollider>(math::Vector3{ 1.1f, 2.2f, 1.1f }),
        physics::PhysicsMaterial::Default,
        true,
        true
    });
    gravityVolume.AddComponent<scene::VolumeComponent>({ .type = physics::VolumeType::Gravity, .gravity = { 0.0f, 7.0f, 0.0f } });

    auto& vortexVolume = scene->CreateGameObject("VortexArea");
    vortexVolume.transform.localPosition = { -8.0f, 1.6f, 0.0f };
    vortexVolume.AddComponent<scene::ColliderComponent>({
        std::make_shared<physics::CapsuleCollider>(1.25f, 2.0f),
        physics::PhysicsMaterial::Default,
        true,
        true
    });
    vortexVolume.AddComponent<scene::VolumeComponent>({
        .type = physics::VolumeType::Vortex,
        .swirlStrength = 10.0f,
        .inwardStrength = 3.0f,
        .liftStrength = 8.0f
    });

    auto& buoyancyVolume = scene->CreateGameObject("BuoyancyArea");
    buoyancyVolume.transform.localPosition = { -2.0f, 0.8f, 0.0f };
    buoyancyVolume.AddComponent<scene::ColliderComponent>({
        std::make_shared<physics::AABBCollider>(math::Vector3{ 1.1f, 0.8f, 1.1f }),
        physics::PhysicsMaterial::Default,
        true,
        true
    });
    buoyancyVolume.AddComponent<scene::VolumeComponent>({
        .type = physics::VolumeType::Buoyancy,
        .buoyancy = 1.4f,
        .drag = 1.2f
    });

    auto& timeVolume = scene->CreateGameObject("TimeDilationArea");
    timeVolume.transform.localPosition = { 4.0f, 1.6f, 0.0f };
    timeVolume.AddComponent<scene::ColliderComponent>({
        std::make_shared<physics::SphereCollider>(1.15f),
        physics::PhysicsMaterial::Default,
        true,
        true
    });
    timeVolume.AddComponent<scene::VolumeComponent>({ .type = physics::VolumeType::TimeDilation, .timeScale = 0.25f });

    auto& magneticVolume = scene->CreateGameObject("MagneticArea");
    magneticVolume.transform.localPosition = { 8.0f, 1.6f, 0.0f };
    magneticVolume.AddComponent<scene::ColliderComponent>({
        std::make_shared<physics::AABBCollider>(math::Vector3{ 1.2f, 1.7f, 1.2f }),
        physics::PhysicsMaterial::Default,
        true,
        true
    });
    magneticVolume.AddComponent<scene::VolumeComponent>({
        .type = physics::VolumeType::Magnetic,
        .magneticField = { 0.0f, 0.0f, 4.0f }
    });

    auto& explosionVolume = scene->CreateGameObject("ExplosionArea");
    explosionVolume.transform.localPosition = { 14.0f, 1.7f, 0.0f };
    explosionVolume.AddComponent<scene::ColliderComponent>({
        std::make_shared<physics::SphereCollider>(1.25f),
        physics::PhysicsMaterial::Default,
        true,
        true
    });
    explosionVolume.AddComponent<scene::VolumeComponent>({ .type = physics::VolumeType::Explosion, .explosionImpulse = 4.0f, .duration = 0.05f });
    auto explosionBody = AddSphere(world, *scene, bodies, assets, "ExplosionAreaBody", { 14.6f, 1.7f, 0.0f }, 0.28f, 1.0f, { 1.0f, 0.7f, 0.2f, 1.0f });

    auto springA = AddSphere(world, *scene, bodies, assets, "SpringAnchor", { -18.0f, 4.2f, -8.0f }, 0.22f, 0.0f, { 0.4f, 1.0f, 0.4f, 1.0f }, true);
    auto springB = AddSphere(world, *scene, bodies, assets, "SpringBody", { -16.5f, 4.2f, -8.0f }, 0.22f, 1.0f, { 0.4f, 1.0f, 0.4f, 1.0f });
    world.AddConstraint(std::make_shared<physics::SpringConstraint>(springA.get(), springB.get(), 1.5f, 18.0f, 1.0f));

    auto ropeA = AddSphere(world, *scene, bodies, assets, "RopeAnchor", { -10.0f, 4.8f, -8.0f }, 0.22f, 0.0f, { 1.0f, 0.7f, 0.2f, 1.0f }, true);
    auto ropeB = AddSphere(world, *scene, bodies, assets, "RopeBody", { -8.2f, 3.8f, -8.0f }, 0.22f, 1.0f, { 1.0f, 0.7f, 0.2f, 1.0f });
    world.AddConstraint(std::make_shared<physics::RopeConstraint>(ropeA.get(), ropeB.get(), 2.1f));

    auto distanceA = AddSphere(world, *scene, bodies, assets, "DistanceBodyA", { -2.0f, 4.0f, -8.0f }, 0.22f, 1.0f, { 0.35f, 0.55f, 1.0f, 1.0f });
    auto distanceB = AddSphere(world, *scene, bodies, assets, "DistanceBodyB", { -0.4f, 4.0f, -8.0f }, 0.22f, 1.0f, { 0.35f, 0.55f, 1.0f, 1.0f });
    world.AddConstraint(std::make_shared<physics::DistanceConstraint>(distanceA.get(), distanceB.get(), 1.6f));

    std::vector<physics::RigidBody*> chainNodes;
    for (int i = 0; i < 5; ++i)
    {
        auto node = AddSphere(world, *scene, bodies, assets, "ChainNode", { 6.0f + i * 0.65f, 4.8f - i * 0.22f, -8.0f }, 0.18f,
                              i == 0 ? 0.0f : 1.0f, { 0.8f, 0.45f, 1.0f, 1.0f }, i == 0);
        chainNodes.push_back(node.get());
    }
    world.AddConstraint(std::make_shared<physics::ChainConstraint>(chainNodes, 0.7f, 6));

    auto hingeA = AddBox(world, *scene, bodies, assets, "HingeAnchor", { 15.0f, 3.0f, -8.0f }, { 0.25f, 0.25f, 0.25f }, 0.0f, { 1.0f, 0.45f, 0.45f, 1.0f }, true);
    auto hingeB = AddBox(world, *scene, bodies, assets, "HingeBody", { 15.95f, 3.0f, -8.0f }, { 0.7f, 0.18f, 0.18f }, 1.0f, { 1.0f, 0.45f, 0.45f, 1.0f });
    hingeB->ApplyAngularImpulse({ 0.0f, 0.0f, 3.0f });
    world.AddConstraint(std::make_shared<physics::HingeConstraint>(
        hingeA.get(),
        hingeB.get(),
        math::Vector3{ 0.25f, 0.0f, 0.0f },
        math::Vector3{ -0.7f, 0.0f, 0.0f },
        math::Vector3::FORWARD));

    capsule->ApplyImpulse({ 3.0f, 0.0f, 0.0f });
    capsulePairA->ApplyImpulse({ 1.1f, 0.0f, 0.0f });
    capsulePairB->ApplyImpulse({ -1.1f, 0.0f, 0.0f });
    capsuleTarget->ApplyImpulse({ -0.4f, 0.0f, 0.0f });
    gravityBody->ApplyImpulse({ 0.0f, -1.0f, 0.0f });
    vortexBody->ApplyImpulse({ 1.5f, 0.0f, 0.0f });
    buoyantBody->ApplyImpulse({ 0.0f, -2.0f, 0.0f });
    timeBody->ApplyImpulse({ 0.0f, -1.8f, 0.0f });

    app.GetWindow().SetResizeCallback([&](uint32_t w, uint32_t h) {
        renderer.Resize(w, h);
    });

    renderer::DebugCamera debugCamera;
    debugCamera.camera.m_position = { 0.0f, 10.0f, -26.0f };
    debugCamera.camera.m_aspect = 1280.0f / 720.0f;
    debugCamera.LookAt({ 0.0f, 2.2f, 0.0f });
    editorApp.GetContext().editorCamera = &debugCamera.camera;

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
        auto gameRT = editorApp.GetGameViewportRT();
        if (auto* rt = resources.Get(sceneRT))
            debugCamera.camera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

        auto* pm = editorApp.GetContext().playMode;
        pm->ApplyPendingRestore(*scene);

        if (!pm->IsPlaying())
            debugCamera.Update(dt);

        scene::TransformSystem(*scene);
        if (pm->IsPlaying()) {
            scene::PhysicsSystem(*scene, world, dt);
            scene::TransformSystem(*scene);
        }

        renderer::Camera gameCamera = debugCamera.camera;
        if (auto* rt = resources.Get(gameRT))
            gameCamera.m_aspect = static_cast<float>(rt->GetWidth()) / static_cast<float>(rt->GetHeight());

        for (auto [tf, cc] : scene->View<scene::Transform, scene::CameraComponent>()) {
            if (!cc.enabled || !cc.isMain) continue;
            gameCamera.m_position = tf.position;
            gameCamera.m_rotation = tf.rotation;
            gameCamera.m_fovY = cc.fovY;
            gameCamera.m_near = cc.nearZ;
            gameCamera.m_far = cc.farZ;
            break;
        }

        renderer.BeginFrame();
        renderer.SetRenderTarget(sceneRT, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.025f, 1.0f });
        scene::RenderSystem(*scene,
                            renderer,
                            resources,
                            debugCamera.camera,
                            sceneRT,
                            &editorApp.GetContext().renderSettings);

        renderer::DebugDraw::BeginFrame(renderer, resources, debugCamera.camera.GetViewProjection());
        renderer::DebugDraw::Box(renderer, { -14.0f, 1.7f, 0.0f }, { 1.1f, 2.2f, 1.1f }, { 0.2f, 0.55f, 1.0f, 1.0f });
        renderer::DebugDraw::Box(renderer, { -8.0f, 1.6f, 0.0f }, { 1.25f, 2.0f, 1.25f }, { 0.9f, 0.35f, 1.0f, 1.0f });
        renderer::DebugDraw::Box(renderer, { -2.0f, 0.8f, 0.0f }, { 1.1f, 0.8f, 1.1f }, { 0.2f, 1.0f, 0.8f, 1.0f });
        renderer::DebugDraw::Sphere(renderer, { 4.0f, 1.6f, 0.0f }, 1.15f, { 1.0f, 0.35f, 0.35f, 1.0f });
        renderer::DebugDraw::Box(renderer, { 8.0f, 1.6f, 0.0f }, { 1.2f, 1.7f, 1.2f }, { 1.0f, 0.2f, 0.2f, 1.0f });
        renderer::DebugDraw::Sphere(renderer, { 14.0f, 1.7f, 0.0f }, 1.25f, { 1.0f, 0.7f, 0.2f, 1.0f });

        for (const BodyDebug& body : bodies)
            DrawBody(renderer, body);

        renderer::DebugDraw::Line(renderer, springA->GetPosition(), springB->GetPosition(), { 0.4f, 1.0f, 0.4f, 1.0f });
        renderer::DebugDraw::Line(renderer, ropeA->GetPosition(), ropeB->GetPosition(), { 1.0f, 0.7f, 0.2f, 1.0f });
        renderer::DebugDraw::Line(renderer, distanceA->GetPosition(), distanceB->GetPosition(), { 0.35f, 0.55f, 1.0f, 1.0f });
        for (size_t i = 0; i + 1 < chainNodes.size(); ++i)
            renderer::DebugDraw::Line(renderer, chainNodes[i]->GetPosition(), chainNodes[i + 1]->GetPosition(), { 0.8f, 0.45f, 1.0f, 1.0f });
        renderer::DebugDraw::Line(renderer, hingeA->GetPosition(), hingeB->GetPosition(), { 1.0f, 0.45f, 0.45f, 1.0f });

        renderer::DebugDraw::Line(renderer, { 0, 0, 0 }, { 2, 0, 0 }, { 1, 0, 0, 1 });
        renderer::DebugDraw::Line(renderer, { 0, 0, 0 }, { 0, 2, 0 }, { 0, 1, 0, 1 });
        renderer::DebugDraw::Line(renderer, { 0, 0, 0 }, { 0, 0, 2 }, { 0, 0, 1, 1 });
        renderer::DebugDraw::Flush();

        if (gameRT.IsValid()) {
            renderer.SetRenderTarget(gameRT, resources);
            renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });
            scene::RenderSystem(*scene,
                                renderer,
                                resources,
                                gameCamera,
                                gameRT,
                                &editorApp.GetContext().renderSettings);
        }

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
