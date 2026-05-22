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

    std::shared_ptr<physics::RigidBody> AddSphere(scene::Scene& scene,
                                                  const VisualAssets& assets,
                                                  const char* name,
                                                  const math::Vector3& position,
                                                  float radius,
                                                  float mass,
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
        return body;
    }

    std::shared_ptr<physics::RigidBody> AddBox(scene::Scene& scene,
                                               const VisualAssets& assets,
                                               const char* name,
                                               const math::Vector3& position,
                                               const math::Vector3& halfExtents,
                                               float mass,
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
        return body;
    }

    std::shared_ptr<physics::RigidBody> AddCapsule(scene::Scene& scene,
                                                  const VisualAssets& assets,
                                                  const char* name,
                                                  const math::Vector3& position,
                                                  float radius,
                                                  float halfHeight,
                                                  float mass)
    {
        auto body = std::make_shared<physics::RigidBody>();
        body->SetPosition(position);
        body->SetMass(mass);
        auto collider = std::make_shared<physics::CapsuleCollider>(radius, halfHeight);
        scene::GameObject& go = AddVisual(scene, name, assets.capsule, "primitive:capsule", assets.whiteMaterial,
                                          position, { radius * 4.0f, (halfHeight + radius) * 2.0f, radius * 4.0f });
        go.AddComponent<scene::RigidBodyComponent>({ body, true });
        go.AddComponent<scene::ColliderComponent>({ collider, physics::PhysicsMaterial::Default, false, true });
        return body;
    }

    void DrawColliderShape(renderer::IRenderer& renderer,
                           const scene::Transform& transform,
                           const scene::ColliderComponent& colliderComponent,
                           const math::Vector4& color)
    {
        const auto& collider = colliderComponent.collider;
        if (!collider) return;

        const math::Vector3 position = transform.position;
        if (collider->GetType() == physics::ColliderType::SPHERE)
        {
            auto* sphere = static_cast<physics::SphereCollider*>(collider.get());
            renderer::DebugDraw::Sphere(renderer, position, sphere->m_radius, color);
        }
        else if (collider->GetType() == physics::ColliderType::AABB)
        {
            auto* box = static_cast<physics::AABBCollider*>(collider.get());
            renderer::DebugDraw::Box(renderer, position, box->m_halfExtents, transform.rotation, color);
        }
        else if (collider->GetType() == physics::ColliderType::CAPSULE)
        {
            auto* capsule = static_cast<physics::CapsuleCollider*>(collider.get());
            renderer::DebugDraw::Capsule(renderer, position, capsule->m_radius, capsule->m_halfHeight, color);
        }
    }

    void DrawSceneColliders(renderer::IRenderer& renderer, scene::Scene& scene)
    {
        for (auto [tf, collider] : scene.View<scene::Transform, scene::ColliderComponent>())
        {
            if (!collider.enabled || !collider.collider) continue;
            const math::Vector4 color = collider.isTrigger
                ? math::Vector4{ 0.2f, 0.7f, 1.0f, 1.0f }
                : math::Vector4{ 0.2f, 1.0f, 0.45f, 1.0f };
            DrawColliderShape(renderer, tf, collider, color);
        }
    }

    void DrawSpotRange(renderer::IRenderer& renderer,
                       const math::Vector3& position,
                       const math::Vector3& direction,
                       float range,
                       float outerConeDegrees,
                       const math::Vector4& color)
    {
        if (range <= 0.0f) return;

        constexpr int kSegments = 24;
        constexpr float kPi = 3.14159265358979f;
        constexpr float kDegToRad = kPi / 180.0f;

        const math::Vector3 forward = direction.Normalized();
        math::Vector3 right = math::Vector3::Cross(math::Vector3::UP, forward);
        if (right.LengthSq() < 1e-5f)
            right = math::Vector3::Cross(math::Vector3::RIGHT, forward);
        right = right.Normalized();
        const math::Vector3 up = math::Vector3::Cross(forward, right).Normalized();

        const math::Vector3 center = position + forward * range;
        const float radius = std::tan(outerConeDegrees * kDegToRad) * range;
        math::Vector3 first = center + right * radius;
        math::Vector3 prev = first;
        for (int i = 1; i <= kSegments; ++i)
        {
            const float angle = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kSegments);
            const math::Vector3 next = center
                + right * (std::cos(angle) * radius)
                + up * (std::sin(angle) * radius);
            renderer::DebugDraw::Line(renderer, prev, next, color);
            prev = next;
        }

        renderer::DebugDraw::Line(renderer, position, first, color);
        renderer::DebugDraw::Line(renderer, position, center + right * -radius, color);
        renderer::DebugDraw::Line(renderer, position, center + up * radius, color);
        renderer::DebugDraw::Line(renderer, position, center + up * -radius, color);
    }

    void DrawLightRanges(renderer::IRenderer& renderer, scene::Scene& scene)
    {
        for (auto [tf, light] : scene.View<scene::Transform, scene::LightComponent>())
        {
            if (!light.enabled || light.type == scene::LightComponent::Type::Directional) continue;

            const math::Vector4 color = { light.color.x, light.color.y, light.color.z, 1.0f };
            if (light.type == scene::LightComponent::Type::Point)
            {
                renderer::DebugDraw::Sphere(renderer, tf.position, light.range, color);
            }
            else if (light.type == scene::LightComponent::Type::Spot)
            {
                DrawSpotRange(renderer, tf.position, tf.Forward(), light.range, light.outerCone, color);
            }
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
    editorApp.GetContext().showColliders = true;

    VisualAssets assets;
    assets.cube = renderer::PrimitiveMesh::Cube(resources);
    assets.sphere = renderer::PrimitiveMesh::Sphere(resources, 16);
    assets.capsule = renderer::PrimitiveMesh::Capsule(resources, 16);
    assets.whiteMaterial = CreateMaterial(resources, { 0.78f, 0.78f, 0.82f, 1.0f });

    auto& light = scene->CreateGameObject("DirectionalLight");
    light.transform.localPosition = { 0.0f, 8.0f, -6.0f };
    light.transform.localRotation = math::Quaternion::LookRotation({ 0.35f, -0.75f, 0.55f });
    light.AddComponent<scene::LightComponent>({});

    auto& pointLight = scene->CreateGameObject("PointLight_DebugRange");
    pointLight.transform.localPosition = { -6.0f, 4.0f, -2.0f };
    scene::LightComponent pointLightComponent{};
    pointLightComponent.type = scene::LightComponent::Type::Point;
    pointLightComponent.color = { 0.25f, 0.65f, 1.0f };
    pointLightComponent.range = 4.0f;
    pointLight.AddComponent<scene::LightComponent>(pointLightComponent);

    auto& spotLight = scene->CreateGameObject("SpotLight_DebugRange");
    spotLight.transform.localPosition = { 6.0f, 5.0f, -4.0f };
    spotLight.transform.localRotation = math::Quaternion::LookRotation(math::Vector3{ -0.2f, -0.6f, 1.0f }.Normalized());
    scene::LightComponent spotLightComponent{};
    spotLightComponent.type = scene::LightComponent::Type::Spot;
    spotLightComponent.color = { 1.0f, 0.75f, 0.25f };
    spotLightComponent.range = 6.0f;
    spotLightComponent.outerCone = 25.0f;
    spotLight.AddComponent<scene::LightComponent>(spotLightComponent);

    auto& mainCamera = scene->CreateGameObject("MainCamera");
    mainCamera.transform.localPosition = { 0.0f, 4.0f, -12.0f };
    mainCamera.transform.localRotation = math::Quaternion::LookRotation(math::Vector3{ 0.0f, -0.22f, 1.0f }.Normalized());
    mainCamera.AddComponent<scene::CameraComponent>({});

    AddBox(*scene, assets, "Ground", { 0.0f, -0.5f, 0.0f }, { 26.0f, 0.5f, 16.0f }, 0.0f, true);

    auto& rotatedBox = AddVisual(*scene,
                                 "RotatedBox_DebugOBB",
                                 assets.cube,
                                 "primitive:cube",
                                 assets.whiteMaterial,
                                 { 0.0f, 1.0f, 4.0f },
                                 { 1.8f, 0.8f, 1.2f });
    rotatedBox.transform.localRotation = math::Quaternion::FromEuler({ 0.0f, 0.65f, 0.35f });
    rotatedBox.AddComponent<scene::ColliderComponent>({
        std::make_shared<physics::AABBCollider>(math::Vector3{ 0.9f, 0.4f, 0.6f }),
        physics::PhysicsMaterial::Default,
        false,
        true
    });

    auto capsule = AddCapsule(*scene, assets, "CapsuleToSphere", { -18.0f, 4.0f, 7.0f }, 0.25f, 0.6f, 1.0f);
    auto capsuleTarget = AddSphere(*scene, assets, "CapsuleSphereTarget", { -15.6f, 1.0f, 7.0f }, 0.32f, 1.0f);
    auto capsulePairA = AddCapsule(*scene, assets, "CapsulePairA", { -11.0f, 3.5f, 7.0f }, 0.22f, 0.55f, 1.0f);
    auto capsulePairB = AddCapsule(*scene, assets, "CapsulePairB", { -9.6f, 3.5f, 7.0f }, 0.22f, 0.55f, 1.0f);

    auto gravityBody = AddSphere(*scene, assets, "GravityAreaBody", { -14.0f, 3.8f, 0.0f }, 0.28f, 1.0f);
    auto vortexBody = AddSphere(*scene, assets, "VortexAreaBody", { -8.0f, 3.5f, 0.9f }, 0.28f, 1.0f);
    auto buoyantBody = AddSphere(*scene, assets, "BuoyancyAreaBody", { -2.0f, 1.3f, 0.0f }, 0.28f, 0.8f);
    auto timeBody = AddSphere(*scene, assets, "TimeDilationAreaBody", { 4.0f, 3.2f, 0.0f }, 0.28f, 1.0f);
    auto magneticBody = AddSphere(*scene, assets, "MagneticAreaBody", { 8.0f, 3.0f, -0.9f }, 0.28f, 1.0f);
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
    AddSphere(*scene, assets, "ExplosionAreaBody", { 14.6f, 1.7f, 0.0f }, 0.28f, 1.0f);

    auto springA = AddSphere(*scene, assets, "SpringAnchor", { -18.0f, 4.2f, -8.0f }, 0.22f, 0.0f, true);
    auto springB = AddSphere(*scene, assets, "SpringBody", { -16.5f, 4.2f, -8.0f }, 0.22f, 1.0f);
    world.AddConstraint(std::make_shared<physics::SpringConstraint>(springA.get(), springB.get(), 1.5f, 18.0f, 1.0f));

    auto ropeA = AddSphere(*scene, assets, "RopeAnchor", { -10.0f, 4.8f, -8.0f }, 0.22f, 0.0f, true);
    auto ropeB = AddSphere(*scene, assets, "RopeBody", { -8.2f, 3.8f, -8.0f }, 0.22f, 1.0f);
    world.AddConstraint(std::make_shared<physics::RopeConstraint>(ropeA.get(), ropeB.get(), 2.1f));

    auto distanceA = AddSphere(*scene, assets, "DistanceBodyA", { -2.0f, 4.0f, -8.0f }, 0.22f, 1.0f);
    auto distanceB = AddSphere(*scene, assets, "DistanceBodyB", { -0.4f, 4.0f, -8.0f }, 0.22f, 1.0f);
    world.AddConstraint(std::make_shared<physics::DistanceConstraint>(distanceA.get(), distanceB.get(), 1.6f));

    std::vector<physics::RigidBody*> chainNodes;
    for (int i = 0; i < 5; ++i)
    {
        auto node = AddSphere(*scene, assets, "ChainNode", { 6.0f + i * 0.65f, 4.8f - i * 0.22f, -8.0f }, 0.18f,
                              i == 0 ? 0.0f : 1.0f, i == 0);
        chainNodes.push_back(node.get());
    }
    world.AddConstraint(std::make_shared<physics::ChainConstraint>(chainNodes, 0.7f, 6));

    auto hingeA = AddBox(*scene, assets, "HingeAnchor", { 15.0f, 3.0f, -8.0f }, { 0.25f, 0.25f, 0.25f }, 0.0f, true);
    auto hingeB = AddBox(*scene, assets, "HingeBody", { 15.95f, 3.0f, -8.0f }, { 0.7f, 0.18f, 0.18f }, 1.0f);
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

        auto& editorContext = editorApp.GetContext();
        renderer::DebugDraw::BeginFrame(renderer, resources, debugCamera.camera.GetViewProjection());
        if (editorContext.showColliders)
            DrawSceneColliders(renderer, *scene);
        if (editorContext.showLightRange)
            DrawLightRanges(renderer, *scene);

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
