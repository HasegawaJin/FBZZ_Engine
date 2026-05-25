// FBZZ Engine
// main.cpp | sandbox
// Physics test visualizer scene
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
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Systems/ConstraintDebugDrawSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/AnimatorDebugDrawSystem.hpp>
#include <Editor/EditorApp.hpp>
#include <Physics/World.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/DistanceConstraint.hpp>
#include <Physics/SpringConstraint.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace fbzz;

namespace {

struct VisualAssets
{
    std::shared_ptr<renderer::Mesh> cube;
    std::shared_ptr<renderer::Mesh> sphere;
    std::shared_ptr<renderer::Material> floor;
    std::shared_ptr<renderer::Material> red;
    std::shared_ptr<renderer::Material> green;
    std::shared_ptr<renderer::Material> blue;
    std::shared_ptr<renderer::Material> yellow;
    std::shared_ptr<renderer::Material> orange;
    std::shared_ptr<renderer::Material> purple;
    std::shared_ptr<renderer::Material> cyan;
    std::shared_ptr<renderer::Material> rubber;
    std::shared_ptr<renderer::Material> stone;
    std::shared_ptr<renderer::Material> ice;
    std::shared_ptr<renderer::Material> trigger;
};

std::shared_ptr<renderer::Material> MakeMaterial(renderer::ResourceManager& res,
                                                  const math::Vector4& color,
                                                  float roughness = 0.6f)
{
    auto mat = std::make_shared<renderer::Material>();
    mat->shader = res.LoadShader("assets/shaders/Material/Phong.hlsl");
    mat->shaderPath = "assets/shaders/Material/Phong.hlsl";
    mat->params.albedo = color;
    mat->params.roughness = roughness;
    mat->Init(res);
    return mat;
}

VisualAssets CreateAssets(renderer::ResourceManager& res)
{
    VisualAssets assets;
    assets.cube = renderer::PrimitiveMesh::Cube(res);
    assets.sphere = renderer::PrimitiveMesh::Sphere(res, 16);
    assets.floor = MakeMaterial(res, { 0.45f, 0.48f, 0.52f, 1.0f });
    assets.red = MakeMaterial(res, { 0.95f, 0.20f, 0.18f, 1.0f });
    assets.green = MakeMaterial(res, { 0.25f, 0.85f, 0.35f, 1.0f });
    assets.blue = MakeMaterial(res, { 0.20f, 0.45f, 1.00f, 1.0f });
    assets.yellow = MakeMaterial(res, { 1.00f, 0.86f, 0.22f, 1.0f });
    assets.orange = MakeMaterial(res, { 1.00f, 0.48f, 0.12f, 1.0f });
    assets.purple = MakeMaterial(res, { 0.68f, 0.35f, 0.95f, 1.0f });
    assets.cyan = MakeMaterial(res, { 0.16f, 0.86f, 0.92f, 1.0f });
    assets.rubber = MakeMaterial(res, { 0.95f, 0.12f, 0.18f, 1.0f }, 0.35f);
    assets.stone = MakeMaterial(res, { 0.48f, 0.46f, 0.42f, 1.0f }, 0.9f);
    assets.ice = MakeMaterial(res, { 0.60f, 0.92f, 1.00f, 1.0f }, 0.2f);
    assets.trigger = MakeMaterial(res, { 1.00f, 0.38f, 0.95f, 1.0f }, 0.5f);
    return assets;
}

void AttachVisual(scene::GameObject& go,
                  std::shared_ptr<renderer::Mesh> mesh,
                  std::shared_ptr<renderer::Material> mat,
                  const char* meshPath)
{
    scene::MeshRenderer mr{};
    mr.mesh = std::move(mesh);
    mr.meshPath = meshPath;
    go.AddComponent<scene::MeshRenderer>(mr);

    scene::MaterialComponent mc{};
    mc.material = std::move(mat);
    mc.shaderPath = mc.material->shaderPath;
    go.AddComponent<scene::MaterialComponent>(mc);
}

std::shared_ptr<physics::RigidBody> AttachPhysics(scene::GameObject& go,
                                                   std::shared_ptr<physics::Collider> col,
                                                   bool isStatic,
                                                   float mass,
                                                   const physics::PhysicsMaterial& material = physics::PhysicsMaterial::Default,
                                                   bool isTrigger = false)
{
    auto rb = std::make_shared<physics::RigidBody>();
    rb->m_isStatic = isStatic;
    rb->SetMass(isStatic ? 0.0f : mass);
    rb->SetPosition(go.transform.localPosition);
    rb->SetRotation(go.transform.localRotation);

    scene::RigidBodyComponent rbc{};
    rbc.rigidBody = rb;
    go.AddComponent<scene::RigidBodyComponent>(rbc);

    scene::ColliderComponent cc{};
    cc.collider = std::move(col);
    cc.material = material;
    cc.isTrigger = isTrigger;
    go.AddComponent<scene::ColliderComponent>(cc);
    return rb;
}

std::shared_ptr<physics::RigidBody> AddBox(scene::Scene& scene,
                                            const VisualAssets& assets,
                                            const std::string& name,
                                            const math::Vector3& position,
                                            const math::Vector3& halfExtents,
                                            std::shared_ptr<renderer::Material> mat,
                                            bool isStatic = false,
                                            float mass = 1.0f,
                                            const physics::PhysicsMaterial& material = physics::PhysicsMaterial::Default,
                                            bool isTrigger = false)
{
    auto& go = scene.CreateGameObject(name);
    go.transform.localPosition = position;
    go.transform.localScale = halfExtents * 2.0f;
    AttachVisual(go, assets.cube, std::move(mat), "primitive:cube");
    return AttachPhysics(go, std::make_shared<physics::AABBCollider>(halfExtents),
                         isStatic, mass, material, isTrigger);
}

std::shared_ptr<physics::RigidBody> AddSphere(scene::Scene& scene,
                                               const VisualAssets& assets,
                                               const std::string& name,
                                               const math::Vector3& position,
                                               float radius,
                                               std::shared_ptr<renderer::Material> mat,
                                               bool isStatic = false,
                                               float mass = 1.0f,
                                               const physics::PhysicsMaterial& material = physics::PhysicsMaterial::Default,
                                               bool isTrigger = false)
{
    auto& go = scene.CreateGameObject(name);
    go.transform.localPosition = position;
    go.transform.localScale = { radius * 2.0f, radius * 2.0f, radius * 2.0f };
    AttachVisual(go, assets.sphere, std::move(mat), "primitive:sphere");
    return AttachPhysics(go, std::make_shared<physics::SphereCollider>(radius),
                         isStatic, mass, material, isTrigger);
}

std::shared_ptr<physics::RigidBody> AddCapsule(scene::Scene& scene,
                                                const VisualAssets& assets,
                                                const std::string& name,
                                                const math::Vector3& position,
                                                float radius,
                                                float halfHeight,
                                                std::shared_ptr<renderer::Material> mat,
                                                bool isStatic = false)
{
    auto& go = scene.CreateGameObject(name);
    go.transform.localPosition = position;
    go.transform.localScale = { radius * 2.0f, (halfHeight + radius) * 2.0f, radius * 2.0f };
    AttachVisual(go, assets.sphere, std::move(mat), "primitive:sphere");
    return AttachPhysics(go, std::make_shared<physics::CapsuleCollider>(radius, halfHeight),
                         isStatic, 1.0f);
}

std::shared_ptr<physics::RigidBody> AddConvexCube(scene::Scene& scene,
                                                   const VisualAssets& assets,
                                                   const std::string& name,
                                                   const math::Vector3& position,
                                                   std::shared_ptr<renderer::Material> mat,
                                                   bool isStatic = false)
{
    static const std::vector<math::Vector3> verts = {
        { -0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f, -0.5f },
        {  0.5f,  0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f },
        { -0.5f, -0.5f,  0.5f }, {  0.5f, -0.5f,  0.5f },
        {  0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f },
    };

    auto& go = scene.CreateGameObject(name);
    go.transform.localPosition = position;
    go.transform.localScale = { 1.0f, 1.0f, 1.0f };
    AttachVisual(go, assets.cube, std::move(mat), "primitive:cube");
    return AttachPhysics(go, std::make_shared<physics::ConvexHullCollider>(verts),
                         isStatic, 1.0f);
}

void AddFloor(scene::Scene& scene,
              const VisualAssets& assets,
              const std::string& name,
              const math::Vector3& center,
              float width = 6.0f,
              float depth = 6.0f)
{
    AddBox(scene, assets, name, center + math::Vector3{ 0.0f, -0.5f, 0.0f },
           { width * 0.5f, 0.5f, depth * 0.5f }, assets.floor, true);
}

void AddTriangleMeshFloor(scene::Scene& scene,
                          const VisualAssets& assets,
                          const math::Vector3& origin)
{
    std::vector<math::Vector3> positions = {
        { -3.0f, 0.0f, -3.0f }, {  3.0f, 0.0f, -3.0f },
        {  3.0f, 0.0f,  3.0f }, { -3.0f, 0.0f,  3.0f },
    };
    std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };

    auto& go = scene.CreateGameObject("P2_TriangleMesh_Floor");
    go.transform.localPosition = origin;
    go.transform.localScale = { 6.0f, 0.04f, 6.0f };
    AttachVisual(go, assets.cube, assets.green, "primitive:cube");
    AttachPhysics(go, std::make_shared<physics::TriangleMeshCollider>(positions, indices),
                  true, 0.0f);
}

physics::RigidBody* FindBody(scene::Scene& scene, const char* name)
{
    auto* go = scene.Find(name);
    if (!go) return nullptr;

    auto* rb = go->GetComponent<scene::RigidBodyComponent>();
    if (!rb || !rb->rigidBody) return nullptr;
    return rb->rigidBody.get();
}

void RegisterPhysicsTestConstraints(scene::Scene& scene, physics::World& world)
{
    auto* anchor = FindBody(scene, "P11_Distance_Anchor");
    auto* limited = FindBody(scene, "P11_Distance_Limited");
    if (anchor && limited)
        world.AddConstraint(std::make_shared<physics::DistanceConstraint>(anchor, limited, 3.0f));

    auto* springA = FindBody(scene, "P11_Spring_A");
    auto* springB = FindBody(scene, "P11_Spring_B");
    if (springA && springB)
        world.AddConstraint(std::make_shared<physics::SpringConstraint>(springA, springB, 1.5f, 25.0f, 0.4f));
}

void ResetPhysicsWorld(scene::Scene& scene, physics::World& world)
{
    world = physics::World{};
    world.SetGravity({ 0.0f, -9.81f, 0.0f });
    RegisterPhysicsTestConstraints(scene, world);
}

void BuildPhysicsTestScene(scene::Scene& scene,
                           renderer::ResourceManager& res)
{
    const VisualAssets assets = CreateAssets(res);

    // Phase 1: warm-started stack.
    {
        const math::Vector3 o{ -24.0f, 0.0f, 0.0f };
        AddFloor(scene, assets, "P1_Floor", o);
        for (int i = 0; i < 5; ++i)
            AddSphere(scene, assets, "P1_StackSphere", o + math::Vector3{ 0.0f, 0.5f + i * 1.02f, 0.0f },
                      0.5f, (i % 2 == 0) ? assets.red : assets.blue);
    }

    // Phase 2: triangle mesh floor with sphere.
    {
        const math::Vector3 o{ -18.0f, 0.0f, 0.0f };
        AddTriangleMeshFloor(scene, assets, o);
        AddSphere(scene, assets, "P2_Sphere_On_BVH_Mesh", o + math::Vector3{ 0.0f, 3.0f, 0.0f },
                  0.35f, assets.yellow);
    }

    // Phase 3: overlapping convex cubes for GJK/EPA inspection.
    {
        const math::Vector3 o{ -12.0f, 0.0f, 0.0f };
        AddFloor(scene, assets, "P3_Reference_Floor", o, 4.0f, 4.0f);
        AddConvexCube(scene, assets, "P3_Convex_A", o + math::Vector3{ -0.4f, 0.5f, 0.0f }, assets.orange, true);
        AddConvexCube(scene, assets, "P3_Convex_B_Overlap_X", o + math::Vector3{ 0.4f, 0.5f, 0.0f }, assets.cyan, true);
    }

    // Phase 4: CCD fast sphere toward a thin wall and target sphere.
    {
        const math::Vector3 o{ -6.0f, 0.0f, 0.0f };
        AddFloor(scene, assets, "P4_Floor", o, 5.0f, 8.0f);
        auto bullet = AddSphere(scene, assets, "P4_CCD_Bullet", o + math::Vector3{ 0.0f, 1.0f, -3.0f },
                                0.25f, assets.yellow, false, 0.5f);
        bullet->SetVelocity({ 0.0f, 0.0f, 35.0f });
        bullet->m_useCCD = true;
        bullet->m_ccdRadius = 0.25f;
        AddSphere(scene, assets, "P4_CCD_Target", o + math::Vector3{ 0.0f, 1.0f, 2.2f },
                  0.5f, assets.green, true);
        AddBox(scene, assets, "P4_Thin_Wall", o + math::Vector3{ 0.0f, 1.0f, 3.2f },
               { 2.0f, 1.0f, 0.08f }, assets.stone, true);
    }

    // Phase 5: stress towers and high-speed drop.
    {
        const math::Vector3 o{ 0.0f, 0.0f, 0.0f };
        AddFloor(scene, assets, "P5_Floor", o, 8.0f, 8.0f);
        for (int i = 0; i < 10; ++i)
            AddSphere(scene, assets, "P5_10Sphere_Tower",
                      o + math::Vector3{ -1.6f, 0.35f + i * 0.72f, 0.0f },
                      0.35f, assets.purple);
        for (int i = 0; i < 5; ++i)
            AddBox(scene, assets, "P5_AABB_Stack",
                   o + math::Vector3{ 1.2f, 0.35f + i * 0.72f, 0.0f },
                   { 0.35f, 0.35f, 0.35f }, assets.blue);
        auto drop = AddSphere(scene, assets, "P5_HighSpeed_Drop",
                              o + math::Vector3{ 3.0f, 5.5f, 0.0f },
                              0.35f, assets.red);
        drop->SetVelocity({ 0.0f, -25.0f, 0.0f });
    }

    // Phase 6: capsule contacts.
    {
        const math::Vector3 o{ 7.0f, 0.0f, 0.0f };
        AddFloor(scene, assets, "P6_Floor", o, 7.0f, 6.0f);
        AddCapsule(scene, assets, "P6_Capsule_On_AABB", o + math::Vector3{ -2.0f, 2.5f, 0.0f },
                   0.3f, 0.5f, assets.orange);
        AddCapsule(scene, assets, "P6_Lower_Capsule", o + math::Vector3{ 0.2f, 0.8f, 0.0f },
                   0.3f, 0.5f, assets.cyan, true);
        AddSphere(scene, assets, "P6_Sphere_On_Capsule", o + math::Vector3{ 0.2f, 3.0f, 0.0f },
                  0.3f, assets.yellow);
        AddCapsule(scene, assets, "P6_Capsule_Stack_A", o + math::Vector3{ 2.0f, 0.8f, 0.0f },
                   0.3f, 0.5f, assets.blue, true);
        AddCapsule(scene, assets, "P6_Capsule_Stack_B", o + math::Vector3{ 2.0f, 2.4f, 0.0f },
                   0.3f, 0.5f, assets.green);
    }

    // Phase 7: momentum conservation pairs.
    {
        const math::Vector3 o{ 14.0f, 0.0f, 0.0f };
        AddFloor(scene, assets, "P7_Floor", o, 7.0f, 4.0f);
        auto a = AddSphere(scene, assets, "P7_Momentum_A", o + math::Vector3{ -2.0f, 0.5f, 0.0f },
                           0.5f, assets.red);
        auto b = AddSphere(scene, assets, "P7_Momentum_B", o + math::Vector3{ 2.0f, 0.5f, 0.0f },
                           0.5f, assets.blue);
        a->SetVelocity({ 4.0f, 0.0f, 0.0f });
        b->SetVelocity({ -4.0f, 0.0f, 0.0f });
    }

    // Phase 8: restitution and friction materials.
    {
        const math::Vector3 o{ -18.0f, 0.0f, 9.0f };
        AddBox(scene, assets, "P8_Rubber_Floor", o + math::Vector3{ -1.2f, -0.5f, 0.0f },
               { 1.2f, 0.5f, 1.2f }, assets.rubber, true, 0.0f, physics::PhysicsMaterial::Rubber);
        AddSphere(scene, assets, "P8_Rubber_Bounce", o + math::Vector3{ -1.2f, 4.0f, 0.0f },
                  0.3f, assets.rubber, false, 1.0f, physics::PhysicsMaterial::Rubber);
        AddBox(scene, assets, "P8_Stone_Floor", o + math::Vector3{ 1.2f, -0.5f, 0.0f },
               { 1.2f, 0.5f, 1.2f }, assets.stone, true, 0.0f, physics::PhysicsMaterial::Stone);
        AddSphere(scene, assets, "P8_Stone_Bounce", o + math::Vector3{ 1.2f, 4.0f, 0.0f },
                  0.3f, assets.stone, false, 1.0f, physics::PhysicsMaterial::Stone);

        AddBox(scene, assets, "P8_Slide_Floor", o + math::Vector3{ 0.0f, -0.5f, 3.0f },
               { 6.0f, 0.5f, 1.5f }, assets.floor, true, 0.0f, physics::PhysicsMaterial::Stone);
        auto ice = AddBox(scene, assets, "P8_Ice_Slider", o + math::Vector3{ -2.0f, 0.5f, 3.0f },
                          { 0.35f, 0.35f, 0.35f }, assets.ice, false, 1.0f, physics::PhysicsMaterial::Ice);
        auto stone = AddBox(scene, assets, "P8_Stone_Slider", o + math::Vector3{ -2.0f, 1.4f, 3.0f },
                            { 0.35f, 0.35f, 0.35f }, assets.stone, false, 1.0f, physics::PhysicsMaterial::Stone);
        ice->SetVelocity({ 5.0f, 0.0f, 0.0f });
        stone->SetVelocity({ 5.0f, 0.0f, 0.0f });
    }

    // Phase 9: convex hull integration.
    {
        const math::Vector3 o{ -8.0f, 0.0f, 9.0f };
        AddFloor(scene, assets, "P9_Floor", o, 8.0f, 6.0f);
        AddConvexCube(scene, assets, "P9_Convex_Drop", o + math::Vector3{ -2.5f, 3.0f, 0.0f }, assets.orange);
        AddConvexCube(scene, assets, "P9_Static_Hull", o + math::Vector3{ 0.0f, 0.5f, 0.0f }, assets.cyan, true);
        AddSphere(scene, assets, "P9_Sphere_On_Hull", o + math::Vector3{ 0.0f, 4.0f, 0.0f },
                  0.3f, assets.yellow);
        auto hullA = AddConvexCube(scene, assets, "P9_Convex_Impact_A",
                                   o + math::Vector3{ 2.0f, 0.6f, -1.4f }, assets.red);
        AddConvexCube(scene, assets, "P9_Convex_Impact_B",
                      o + math::Vector3{ 4.3f, 0.6f, -1.4f }, assets.blue);
        hullA->SetVelocity({ 3.0f, 0.0f, 0.0f });
    }

    // Phase 10: trigger and collision events.
    {
        const math::Vector3 o{ 2.0f, 0.0f, 9.0f };
        AddFloor(scene, assets, "P10_Floor", o, 6.0f, 4.0f);
        AddSphere(scene, assets, "P10_Trigger_A", o + math::Vector3{ -1.0f, 0.7f, 0.0f },
                  0.7f, assets.trigger, false, 1.0f, physics::PhysicsMaterial::Default, true);
        AddSphere(scene, assets, "P10_Trigger_B", o + math::Vector3{ -0.1f, 0.7f, 0.0f },
                  0.7f, assets.purple, false, 1.0f, physics::PhysicsMaterial::Default, true);
        auto solid = AddSphere(scene, assets, "P10_Solid_Into_Trigger", o + math::Vector3{ 2.4f, 0.7f, 0.0f },
                               0.7f, assets.yellow);
        solid->SetVelocity({ -2.0f, 0.0f, 0.0f });
    }

    // Phase 11: constraints.
    {
        const math::Vector3 o{ 11.0f, 0.0f, 9.0f };
        AddFloor(scene, assets, "P11_Floor", o, 8.0f, 5.0f);
        AddSphere(scene, assets, "P11_Distance_Anchor", o + math::Vector3{ -2.5f, 1.0f, 0.0f },
                  0.25f, assets.green, true);
        auto limited = AddSphere(scene, assets, "P11_Distance_Limited", o + math::Vector3{ 1.5f, 1.0f, 0.0f },
                                 0.25f, assets.red);
        limited->SetVelocity({ 6.0f, 0.0f, 0.0f });

        AddSphere(scene, assets, "P11_Spring_A", o + math::Vector3{ -2.0f, 2.5f, 1.4f },
                  0.25f, assets.cyan, true);
        AddSphere(scene, assets, "P11_Spring_B", o + math::Vector3{ 2.0f, 2.5f, 1.4f },
                  0.25f, assets.orange);
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

    auto scene = std::make_unique<scene::Scene>();
    editorApp.GetContext().activeScene = scene.get();

    auto& camGO = scene->CreateGameObject("MainCamera");
    camGO.transform.localPosition = { 0.0f, 13.0f, -24.0f };
    camGO.transform.localRotation = math::Quaternion::FromEuler({ 0.48f, 0.0f, 0.0f });
    camGO.AddComponent<scene::CameraComponent>({});

    auto& lightGO = scene->CreateGameObject("Light");
    lightGO.transform.localPosition = { 4.0f, 16.0f, -8.0f };
    lightGO.transform.localRotation = math::Quaternion::LookRotation({ -0.25f, -0.9f, 0.25f });
    lightGO.AddComponent<scene::LightComponent>({});

    physics::World physWorld;
    BuildPhysicsTestScene(*scene, resources);
    ResetPhysicsWorld(*scene, physWorld);

    app.GetWindow().SetResizeCallback([&](uint32_t w, uint32_t h) {
        renderer.Resize(w, h);
    });

    renderer::DebugCamera debugCamera;
    debugCamera.camera.m_position = { 0.0f, 13.0f, -24.0f };
    debugCamera.camera.m_aspect = 1920.0f / 1080.0f;
    editorApp.GetContext().editorCamera = &debugCamera.camera;

    while (app.IsRunning())
    {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        const float dt = core::Time::DeltaTime();

        editorApp.BeginFrame();

        auto* pm = editorApp.GetContext().playMode;
        if (pm->ApplyPendingRestore(*scene))
            ResetPhysicsWorld(*scene, physWorld);

        if (!pm->IsPlaying())
            debugCamera.Update(dt);

        scene::TransformSystem(*scene);

        if (pm->IsPlaying())
        {
            scene::PhysicsSystem(*scene, physWorld, dt);
            scene::TransformSystem(*scene);
            scene::AnimatorSystem(*scene, resources, dt);
        }
        else
        {
            scene::AnimatorSystem(*scene, resources, dt);
        }

        const auto sceneRT = editorApp.GetViewportRT();
        const auto gameRT = editorApp.GetGameViewportRT();

        if (auto* rt = resources.Get(sceneRT))
            debugCamera.camera.m_aspect = static_cast<float>(rt->GetWidth()) /
                                          static_cast<float>(rt->GetHeight());

        renderer::Camera gameCamera = debugCamera.camera;
        if (auto* rt = resources.Get(gameRT))
            gameCamera.m_aspect = static_cast<float>(rt->GetWidth()) /
                                  static_cast<float>(rt->GetHeight());

        renderer.BeginFrame();

        renderer.SetRenderTarget(sceneRT, resources);
        renderer.Clear({ 0.05f, 0.05f, 0.08f, 1.0f });
        scene::RenderSystem(*scene, renderer, resources,
                            debugCamera.camera, sceneRT,
                            &editorApp.GetContext().renderSettings);
        if (editorApp.GetContext().renderSettings.showColliders)
        {
            renderer::DebugDraw::BeginFrame(renderer, resources, debugCamera.camera.GetViewProjection());
            scene::ConstraintDebugDrawSystem(physWorld, renderer);
            renderer::DebugDraw::Flush();
        }
        if (editorApp.GetContext().showSkeleton)
        {
            scene::AnimatorDebugDrawSystem(*scene, renderer, resources,
                                           debugCamera.camera.GetViewProjection());
        }

        if (gameRT.IsValid())
        {
            renderer.SetRenderTarget(gameRT, resources);
            renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });
            scene::RenderSystem(*scene, renderer, resources,
                                gameCamera, gameRT,
                                &editorApp.GetContext().renderSettings);
            if (editorApp.GetContext().renderSettings.showColliders)
            {
                renderer::DebugDraw::BeginFrame(renderer, resources, gameCamera.GetViewProjection());
                scene::ConstraintDebugDrawSystem(physWorld, renderer);
                renderer::DebugDraw::Flush();
            }
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
