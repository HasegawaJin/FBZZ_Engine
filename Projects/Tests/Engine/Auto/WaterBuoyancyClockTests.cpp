/// @file    WaterBuoyancyClockTests.cpp
/// @brief   Keep native buoyancy's last substep at the published scene wave time instead of one step ahead.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Physics/FluidVolume.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <memory>
#include <string>

namespace fbzz::tests {
namespace {
constexpr float STEP = 1.0f / 60.0f;

/// @note The independent expected boundary uses production WaterSurfaceQuery and FluidVolume, with an explicit interval.
math::Vector3 IntegrateReference(const scene::WaterComponent& water, const scene::Transform& pose,
    math::Vector3 position, float startTime, int substeps, int repeats)
{
    physics::RigidBody body;
    body.SetPosition(position);
    body.m_linearDrag = body.m_angularDrag = 0.0f;
    body.m_allowSleeping = false;
    physics::World world;
    world.SetGravity(math::Vector3::ZERO);
    world.SetSubsteps(substeps);
    physics::BodyHandle bodyHandle;
    physics::VolumeHandle volumeHandle;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        physics::FluidVolumeDesc desc;
        desc.center = pose.worldPosition;
        desc.halfX = desc.halfZ = 1000.0f;
        desc.buoyancy = water.buoyancy;
        desc.drag = 0.0f;
        desc.depthLimit = water.buoyancyDepth;
        desc.allowProbeIntersection = true;
        desc.startTime = startTime;
        desc.surfaceHeightBound = water.SurfaceHeightBound(scene::WaterSurfaceReadView::SIMULATION);
        desc.surfaceHeight = [&water, &pose](float x, float z, float time) {
            scene::WaterSurfaceHeightSample sample;
            EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose,
                {x, pose.worldPosition.y, z}, time, sample, scene::WaterSurfaceReadView::SIMULATION));
            return sample.displacement;
        };
        desc.containsXZ = [&water, &pose](float x, float z, float time) {
            return scene::WaterSurfaceContainsXZ(water, pose, x, z, time, scene::WaterSurfaceReadView::SIMULATION);
        };
        world.BeginSceneSync();
        bodyHandle = world.SyncBody(bodyHandle, &body);
        volumeHandle = world.SyncVolume(volumeHandle, std::make_unique<physics::FluidVolume>(std::move(desc)));
        world.EndSceneSync();
        world.Step(STEP);
    }
    return body.GetVelocity();
}
} /// @note namespace

class WaterBuoyancyClockTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousTime = Time::time;
        m_previousWorld = scene::Script::GetPhysicsWorld();
    }
    void TearDown() override
    {
        Time::time = m_previousTime;
        scene::Script::SetPhysicsWorld(m_previousWorld);
        EngineFixture::TearDown();
    }
    float m_previousTime = 0.0f;
    physics::World* m_previousWorld = nullptr;
};

TEST_F(WaterBuoyancyClockTest, EndsOneAndFourBuoyancySubstepsAtThePublishedSceneWaveTime)
{
    float maximumError = 0.0f, minimumOldClockSeparation = 1.0f;
    size_t cases = 0;
    for (int substeps : {1, 4}) for (int repeats : {1, 3}) for (bool horizon : {false, true}) {
        scene::Scene source;
        auto& owner = source.CreateGameObject("Clock Water");
        const auto waterId = owner.GetID();
        auto& water = owner.AddComponent<scene::WaterComponent>();
        water.extentX = water.extentZ = 20.0f;
        water.buoyancyDepth = 5.0f;
        water.waterDrag = 0.0f;
        water.farOceanEnabled = horizon;
        water.waveCount = 1;
        water.waveGrouping = water.waveSpread = 0.0f;
        water.waves[0].direction = {1.0f, 0.0f};
        water.waves[0].amplitude = 0.3f;
        water.waves[0].wavelength = 4.0f;
        water.waves[0].steepness = 0.35f;
        auto& ship = source.CreateGameObject("Clock Probe Body");
        ship.transform.position = {horizon ? 80.0f : 1.0f, 0.0f, 0.0f};
        const auto shipId = ship.GetID();
        auto& component = ship.AddComponent<scene::RigidBodyComponent>();
        component.rigidBody = std::make_unique<physics::RigidBody>();
        component.rigidBody->SetPosition(ship.transform.position);
        component.rigidBody->m_linearDrag = component.rigidBody->m_angularDrag = 0.0f;
        component.rigidBody->m_allowSleeping = false;
        scene::FlushWorldTransforms(source);
        scene::PrepareWaterSimulationSurfaces(source);
        Time::time = 1.375f;
        const auto& currentWater = *source.GetComponent<scene::WaterComponent>(waterId);
        const auto& pose = source.GetGameObject(waterId)->transform;
        const auto position = source.GetGameObject(shipId)->transform.worldPosition;
        const auto expected = IntegrateReference(currentWater, pose, position, Time::time - STEP, substeps, repeats);
        const auto oneStepAhead = IntegrateReference(currentWater, pose, position, Time::time, substeps, repeats);
        physics::World world;
        world.SetGravity(math::Vector3::ZERO);
        world.SetSubsteps(substeps);
        scene::PhysicsSystem system;
        SystemContext context{source, world, nullptr, nullptr, STEP, STEP, true, true};
        /// @note Scheduler catch-up currently reuses Time::time. Preserve that policy while preventing future wave samples.
        for (int repeat = 0; repeat < repeats; ++repeat) system.Update(context);
        const auto actual = source.GetComponent<scene::RigidBodyComponent>(shipId)->rigidBody->GetVelocity();
        maximumError = (std::max)(maximumError, (actual - expected).Length());
        minimumOldClockSeparation = (std::min)(minimumOldClockSeparation, (oneStepAhead - expected).Length());
        EXPECT_VEC3_NEAR(actual, expected, 0.00002f);
        ++cases;
    }
    EXPECT_EQ(cases, 8u);
    EXPECT_GT(minimumOldClockSeparation, 0.0001f);
    RecordProperty("nativeBuoyancyVelocityMaxError", std::to_string(maximumError));
    RecordProperty("oldClockMinimumVelocitySeparation", std::to_string(minimumOldClockSeparation));
    RecordProperty("timeContract", "Time::Tick publishes T before scheduler; substep samples T-dt+(s+1)*dt/N; final sample T");
}
} /// @note namespace fbzz::tests
