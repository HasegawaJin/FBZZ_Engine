/// @file    ClothContactSystemTests.cpp
/// @brief   Cloth の衝突フィルターとシーンからの接触速度・力積転送を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Systems/ClothSystem.hpp>
#include <Physics/World.hpp>
#include <Physics/Layer.hpp>

namespace fbzz::tests {
class ClothContactSystemTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    scene::EntityID AddCloth(float z = 0)
    {
        auto& go = m_scene.CreateGameObject("Cloth");
        go.transform.position = go.transform.worldPosition = {0,0,z};
        auto& cloth = go.AddComponent<scene::ClothComponent>();
        cloth.segments = 2;
        cloth.pinTop = false;
        cloth.settings.gravity = {};
        cloth.settings.dragCoefficient = 0;
        cloth.settings.damping = 0;
        cloth.settings.substeps = 1;
        cloth.settings.iterations = 1;
        cloth.settings.stretchCompliance = 1.0e8f;
        cloth.settings.bendCompliance = 1.0e8f;
        return go.GetID();
    }
    scene::ClothComponent& Cloth(scene::EntityID id) { return *m_scene.GetComponent<scene::ClothComponent>(id); }
    scene::GameObject& AddSphere()
    {
        auto& go = m_scene.CreateGameObject("Sphere");
        go.layer = 3;
        go.transform.position = go.transform.worldPosition = {0,-1,-0.4f};
        go.AddComponent<scene::SphereColliderComponent>().radius = 0.5f;
        return go;
    }
    void Tick()
    {
        SystemContext ctx{m_scene,m_world,nullptr,nullptr,0.01f,0.01f,true,true};
        scene::ClothSystem{}.Update(ctx);
    }
};

TEST_F(ClothContactSystemTest, BothClothMaskAndWorldMatrixFilterExternalContacts)
{
    const auto id = AddCloth();
    AddSphere();
    Cloth(id).collisionMask = 0;
    Tick();
    EXPECT_NEAR(Cloth(id).runtime.solver.Positions()[4].z,0,1.0e-6f);
    Cloth(id).collisionMask = 1u<<3;
    LayerCollisionMatrix matrix;
    matrix.Set(0,3,false);
    m_world.SetCollisionMatrix(matrix);
    Tick();
    EXPECT_NEAR(Cloth(id).runtime.solver.Positions()[4].z,0,1.0e-6f);
    matrix.Set(0,3,true);
    m_world.SetCollisionMatrix(matrix);
    Tick();
    EXPECT_GT(Cloth(id).runtime.solver.Positions()[4].z,0.1f);
    EXPECT_FALSE(Cloth(id).runtime.failed);
}

TEST_F(ClothContactSystemTest, TransformDrivenSurfaceTransfersTangentialVelocity)
{
    const auto id = AddCloth();
    auto& sphere = AddSphere();
    Cloth(id).settings.friction = 1;
    Cloth(id).settings.gravity = {0,0,-10};
    Cloth(id).collisionMask = 0;
    Tick();
    Cloth(id).collisionMask = 0xffffffffu;
    Tick();
    sphere.transform.worldPosition.x += 0.01f;
    Tick();
    EXPECT_GT(Cloth(id).runtime.solver.Velocities()[4].x,0.9f);
    EXPECT_FALSE(Cloth(id).runtime.failed);
}

TEST_F(ClothContactSystemTest, ReactionIsOptInAndFindsAncestorBody)
{
    const auto id = AddCloth();
    Cloth(id).settings.gravity = {0,0,-10};
    auto& parent = m_scene.CreateGameObject("Body");
    auto& rb = parent.AddComponent<scene::RigidBodyComponent>();
    rb.rigidBody = std::make_unique<physics::RigidBody>();
    rb.rigidBody->SetMass(2);
    rb.rigidBody->SetPosition({0,-1,-0.4f});
    auto& sphere = AddSphere();
    sphere.SetParent(parent);
    sphere.GetComponent<scene::SphereColliderComponent>()->attachToParentBody = true;
    Tick();
    EXPECT_VEC3_NEAR(rb.rigidBody->GetVelocity(),math::Vector3{},1.0e-6f);
    Cloth(id).runtime = scene::ClothRuntime{};
    Cloth(id).twoWayCoupling = true;
    Tick();
    EXPECT_LT(rb.rigidBody->GetVelocity().z,-0.001f);
    EXPECT_FALSE(Cloth(id).runtime.failed);
}

TEST_F(ClothContactSystemTest, InterClothCcdReachesSolverWithoutEnablingSelfCollision)
{
    const auto a = AddCloth();
    const auto b = AddCloth(0.1f);
    auto& transform = m_scene.GetGameObject(b)->transform;
    transform.position = transform.worldPosition = {0.5f,0.5f,0.1f};
    Cloth(a).overridePins = true;
    Cloth(a).pinnedParticles = {0,1,2,3,4,5,6,7,8};
    Cloth(a).interCollisionDistance = Cloth(b).interCollisionDistance = 0.05f;
    Cloth(b).settings.gravity = {0,0,-2000};
    for (bool continuous : {false,true}) {
        Cloth(b).runtime = scene::ClothRuntime{};
        Cloth(b).interContinuousCollision = continuous;
        Cloth(b).interCollisionFaces = true;
        Tick();
        ASSERT_FALSE(Cloth(b).runtime.failed);
        const float z = Cloth(b).runtime.solver.Positions()[4].z;
        if (continuous) EXPECT_GE(z,0.0499f);
        else EXPECT_LT(z,0);
        EXPECT_FLOAT_EQ(Cloth(b).settings.selfCollisionDistance,0);
    }
}
TEST_F(ClothContactSystemTest, SeparateClothsHonorMutualMasksAndFixedParticles)
{
    const auto a = AddCloth();
    const auto b = AddCloth(0.02f);
    m_scene.GetGameObject(b)->layer = 2;
    Cloth(a).interCollisionDistance = Cloth(b).interCollisionDistance = 0.1f;
    Cloth(a).overridePins = true;
    Cloth(a).pinnedParticles = {0,1,2,3,4,5,6,7,8};
    Cloth(b).collisionMask = 0;
    Tick();
    EXPECT_NEAR(Cloth(b).runtime.solver.Positions()[4].z,0.02f,1.0e-6f);
    Cloth(b).collisionMask = 1;
    Tick();
    EXPECT_NEAR(Cloth(a).runtime.solver.Positions()[4].z,0,1.0e-6f);
    EXPECT_NEAR(Cloth(b).runtime.solver.Positions()[4].z,0.1f,1.0e-5f);
    EXPECT_FALSE(Cloth(a).runtime.failed);
    EXPECT_FALSE(Cloth(b).runtime.failed);
}
}
