/// @file    ClothFlowFieldTests.cpp
/// @brief   Cloth と FlowField の受信設定、空間・チャンネル・環境風の接続を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Environment/SceneEnvironment.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Systems/ClothSystem.hpp>
#include <Physics/World.hpp>

namespace fbzz::tests {
class ClothFlowFieldTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    scene::EntityID m_cloth;
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        m_scene.Environment().enabled = false;
        auto& go = m_scene.CreateGameObject("Cloth");
        m_cloth = go.GetID();
        auto& cloth = go.AddComponent<scene::ClothComponent>();
        cloth.segments = 2; cloth.pinTop = false;
        cloth.settings.gravity = {}; cloth.settings.damping = 0;
        cloth.settings.substeps = 1; cloth.settings.iterations = 1;
        cloth.settings.stretchCompliance = cloth.settings.bendCompliance = 1.0e8f;
    }
    scene::ClothComponent& Cloth() { return *m_scene.GetComponent<scene::ClothComponent>(m_cloth); }
    scene::GameObject& AddFlow(float speed = 3, uint32_t channels = 0xffffffffu, float radius = 0, math::Vector3 center = {})
    {
        auto& go = m_scene.CreateGameObject("Flow");
        go.transform.position = go.transform.worldPosition = center;
        scene::FlowFieldSettings field;
        field.direction = {0,0,1}; field.strength = speed; field.radius = radius; field.channels = channels;
        go.AddComponent<scene::FlowField>().forces = {field};
        m_scene.InvalidateFlowFrame();
        return go;
    }
    void Tick()
    {
        SystemContext ctx{m_scene,m_world,nullptr,nullptr,0.01f,0.01f,true,true};
        scene::ClothSystem{}.Update(ctx);
        ASSERT_FALSE(Cloth().runtime.failed);
    }
    void Reset() { Cloth().runtime = scene::ClothRuntime{}; }
};
TEST_F(ClothFlowFieldTest, ReceptionIsOptInAndChannelMaskIncludesTheHighBit)
{
    AddFlow(3,0x80000000u);
    Tick();
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[4].z,0,1.0e-7f);
    Cloth().receiveFlowFields = true;
    Cloth().flowFieldChannels = 1;
    Tick();
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[4].z,0,1.0e-7f);
    Cloth().flowFieldChannels = 0x80000000u;
    Tick();
    EXPECT_GT(Cloth().runtime.solver.Positions()[4].z,0);
    Reset();
    Cloth().flowFieldChannels = 0;
    Tick();
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[4].z,0,1.0e-7f);
}
TEST_F(ClothFlowFieldTest, SceneFlowAndAdditionalWindMatchOneUniformWind)
{
    AddFlow(3);
    Cloth().settings.windVelocity = {0,0,5};
    Tick();
    const auto expected = Cloth().runtime.solver.Positions();
    Reset();
    Cloth().settings.windVelocity = {0,0,2};
    Cloth().receiveFlowFields = true;
    Tick();
    for (size_t i = 0; i < expected.size(); ++i) EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[i],expected[i],1.0e-6f);
}
TEST_F(ClothFlowFieldTest, LocalFieldUsesWorldCentersAndLeavesDistantFacesUnaffected)
{
    auto& transform = m_scene.GetGameObject(m_cloth)->transform;
    transform.position = transform.worldPosition = {5,3,2};
    AddFlow(3,0xffffffffu,0.2f,{5-2.0f/3,3-1.0f/3,2});
    Cloth().receiveFlowFields = true;
    Tick();
    EXPECT_GT(Cloth().runtime.solver.Positions()[0].z,2);
    EXPECT_NEAR(Cloth().runtime.solver.Positions()[8].z,2,1.0e-6f);
}
TEST_F(ClothFlowFieldTest, AmbientWindIsIncludedExactlyOnceAndInactiveFieldsAreIgnored)
{
    auto& fieldGo = AddFlow(20);
    fieldGo.SetActive(false);
    auto& environment = m_scene.Environment();
    environment.enabled = true; environment.direction = {0,0,1}; environment.speed = 3; environment.turbulence = 0;
    m_scene.InvalidateFlowFrame();
    Cloth().settings.windVelocity = {0,0,3};
    Tick();
    const auto expected = Cloth().runtime.solver.Positions();
    Reset();
    Cloth().settings.windVelocity = {};
    Cloth().receiveFlowFields = true;
    Tick();
    for (size_t i = 0; i < expected.size(); ++i) EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[i],expected[i],1.0e-6f);
}
TEST_F(ClothFlowFieldTest, VortexPushesOppositeSidesInOppositeDirections)
{
    auto& go = AddFlow();
    auto& field = go.GetComponent<scene::FlowField>()->forces[0];
    field.fieldType = scene::FlowFieldType::Vortex;
    field.direction = {0,1,0};
    Cloth().receiveFlowFields = true;
    Tick();
    EXPECT_GT(Cloth().runtime.solver.Positions()[0].z,0);
    EXPECT_LT(Cloth().runtime.solver.Positions()[2].z,0);
}
TEST_F(ClothFlowFieldTest, NoFieldPreservesExistingWindAndDragBehavior)
{
    Cloth().settings.windVelocity = {0,0,2};
    Tick();
    const auto expected = Cloth().runtime.solver.Positions();
    Reset();
    Cloth().receiveFlowFields = true;
    Tick();
    for (size_t i = 0; i < expected.size(); ++i) EXPECT_VEC3_NEAR(Cloth().runtime.solver.Positions()[i],expected[i],1.0e-6f);
}
TEST_F(ClothFlowFieldTest, SharedReceiverMatchesWaterAndParticleAndPreservesSnapshot)
{
    AddFlow(3, 0x80000000u);
    auto& environment = m_scene.Environment();
    environment.enabled = true; environment.direction = {0,0,1}; environment.speed = 2; environment.turbulence = 0;
    m_scene.InvalidateFlowFrame();
    const auto& frame = m_scene.FlowFrame();
    const auto sampler = scene::MakeFlowSampler(frame, {}, 0);
    const auto localSampler = scene::MakeFlowSampler(frame, {}, 0, false);
    EXPECT_VEC3_NEAR(sampler({}), (math::Vector3{0,0,5}), 1.0e-6f);
    EXPECT_VEC3_NEAR(localSampler({}), (math::Vector3{0,0,3}), 1.0e-6f);
    EXPECT_FALSE(scene::MakeFlowSampler(frame, {false, 0xffffffffu}, 0));
    EXPECT_FALSE(scene::MakeFlowSampler(frame, {true, 0}, 0));
    scene::WaterComponent water;
    water.enableGerstnerWaves = false;
    scene::ResolveWaterWaves(water, nullptr, {});
    water.PrepareWaveCache();
    EXPECT_VEC3_NEAR(scene::WaterFlowVelocityAt(water, *frame.fields, 0, {}, 0), sampler({}), 1.0e-6f);
    math::Vector3 velocity{};
    scene::ApplyFlowFields(*frame.fields, 0xffffffffu, {}, velocity, 1, 0, 1);
    EXPECT_VEC3_NEAR(velocity, sampler({}), 1.0e-6f);
    const auto fields = scene::SelectFlowFields(frame, false);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_TRUE((scene::FlowReceiver{true, 0x80000000u}.Intersects(fields[0], {}, -1)));
    EXPECT_FALSE((scene::FlowReceiver{true, 1u}.Intersects(fields[0], {}, -1)));
    bool covered = true;
    EXPECT_VEC3_NEAR((scene::FlowReceiver{false}.Sample({}, fields, 0, &covered)), math::Vector3::ZERO, 1.0e-6f);
    EXPECT_FALSE(covered);
    AddFlow(7);
    EXPECT_EQ(m_scene.FlowFrame().sceneFieldCount, 2u);
    EXPECT_VEC3_NEAR(sampler({}), (math::Vector3{0,0,5}), 1.0e-6f);
    EXPECT_VEC3_NEAR(localSampler({}), (math::Vector3{0,0,3}), 1.0e-6f);
}
}
