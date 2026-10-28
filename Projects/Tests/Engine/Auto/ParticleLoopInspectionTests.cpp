/// @file    ParticleLoopInspectionTests.cpp
/// @brief   焼いたフリップブックの粒子再生が周期境界で空白にならないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-24
#include <TestKit/TestKit.hpp>
#include <Engine/Asset/ParticleEmitterPresets.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <limits>
#include <cmath>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include <Physics/World.hpp>
#include <vector>

namespace fbzz::tests {

class ParticleLoopInspectionTest : public testkit::EngineFixture {
protected:
    physics::World m_world;
    scene::Scene m_scene;
};

TEST_F(ParticleLoopInspectionTest, OneParticlePerCycleRemainsVisibleAtThirtyAndSixtyFps)
{
    scene::GameObject& object = m_scene.CreateGameObject("JetFlame");
    scene::ParticleEmitter& emitter = object.AddComponent<scene::ParticleEmitter>();
    emitter.settings.duration = 1.6f;
    emitter.settings.lifetime = 1.6f;
    emitter.settings.loop = true;
    emitter.settings.emitRate = 0.0f;
    emitter.settings.maxParticles = 2;
    emitter.settings.bursts = { scene::ParticleBurst{ 0.0f, 1, 1, 0.0f, 1.0f } };
    emitter.settings.sizeStart = 2.0f;
    emitter.settings.sizeEnd = 2.0f;
    emitter.settings.colorStart = { 1.0f, 1.0f, 1.0f, 1.0f };
    emitter.settings.colorEnd = { 1.0f, 1.0f, 1.0f, 1.0f };
    emitter.runtime.material.flipbook.spriteColumns = 8;
    emitter.runtime.material.flipbook.spriteRows = 8;
    emitter.runtime.material.flipbook.flipbookMode = scene::ParticleFlipbookMode::FramesPerSecond;
    emitter.runtime.material.flipbook.flipbookFramesPerSecond = 40.0f;
    std::vector<float> coverage(64, 0.2f);

    for (int fps : { 30, 60 }) {
        scene::ParticleLoopInspection result;
        ASSERT_TRUE(scene::InspectParticleEmitterLoop(m_scene, m_world, object, emitter,
                                                       fps, 3, coverage, result));
        EXPECT_EQ(result.sampledFrames, fps * 3 * 8 / 5);
        EXPECT_EQ(result.emptyFrames, 0);
        EXPECT_EQ(result.boundaryEmptyFrames, 0);
    }
}

TEST_F(ParticleLoopInspectionTest, EmptyAtlasIsReportedAsBlank)
{
    scene::GameObject& object = m_scene.CreateGameObject("Blank");
    scene::ParticleEmitter& emitter = object.AddComponent<scene::ParticleEmitter>();
    emitter.settings.duration = 1.0f;
    emitter.settings.lifetime = 1.0f;
    emitter.settings.emitRate = 0.0f;
    emitter.settings.maxParticles = 2;
    emitter.settings.bursts = { scene::ParticleBurst{ 0.0f, 1, 1, 0.0f, 1.0f } };
    std::vector<float> coverage(1, 0.0f);

    scene::ParticleLoopInspection result;
    ASSERT_TRUE(scene::InspectParticleEmitterLoop(m_scene, m_world, object, emitter,
                                                   30, 3, coverage, result));
    EXPECT_EQ(result.emptyFrames, result.sampledFrames);
    EXPECT_EQ(result.firstEmptyFrame, 0);
    EXPECT_GT(result.boundaryEmptyFrames, 0);
}


TEST_F(ParticleLoopInspectionTest, EveryStarterProducesFiniteBoundedParticlesAndReplaysDeterministically)
{
    for (const auto& preset : asset::ParticleEmitterPresets()) {
        SCOPED_TRACE(preset.name);
        scene::GameObject& object = m_scene.CreateGameObject(std::string(preset.name));
        scene::ParticleEmitter& emitter = object.AddComponent<scene::ParticleEmitter>();
        asset::ApplyParticleEmitterPreset(emitter.settings, preset.id);
        emitter.settings.simulationMode = scene::ParticleSimulationMode::Cpu;
        scene::ScrubParticleEmitterForEditor(m_scene, m_world, object, emitter, 0.5f, 0.5f);
        const auto before = emitter.runtime.particles;
        ASSERT_FALSE(before.empty());
        ASSERT_LE(before.size(), static_cast<std::size_t>(emitter.settings.maxParticles));
        scene::ScrubParticleEmitterForEditor(m_scene, m_world, object, emitter, 0.5f, 0.5f);
        ASSERT_EQ(emitter.runtime.particles.size(), before.size());
        for (std::size_t index = 0; index < before.size(); ++index) {
            const auto& particle = emitter.runtime.particles[index];
            EXPECT_TRUE(std::isfinite(particle.position.x) && std::isfinite(particle.position.y) && std::isfinite(particle.position.z));
            EXPECT_TRUE(std::isfinite(particle.size));
            EXPECT_TRUE(std::isfinite(particle.velocity.x) && std::isfinite(particle.velocity.y) && std::isfinite(particle.velocity.z));
            EXPECT_TRUE(std::isfinite(particle.color.x) && std::isfinite(particle.color.y) && std::isfinite(particle.color.z) && std::isfinite(particle.color.w));
            EXPECT_VEC3_NEAR(particle.position, before[index].position, testkit::kTolerance);
            EXPECT_VEC3_NEAR(particle.velocity, before[index].velocity, testkit::kTolerance);
            EXPECT_VEC4_NEAR(particle.color, before[index].color, testkit::kTolerance);
        }
    }
}

TEST_F(ParticleLoopInspectionTest, BurstRequestsSaturateWithoutIntegerOverflow)
{
    scene::ParticleEmitter emitter;
    emitter.settings.maxParticles = 16;
    emitter.Burst(10);
    emitter.Burst((std::numeric_limits<int>::max)());
    emitter.Burst((std::numeric_limits<int>::max)());
    EXPECT_EQ(emitter.runtime.burstPending, 16);
    emitter.Burst(-1);
    EXPECT_EQ(emitter.runtime.burstPending, 16);
    emitter.ClearParticles();
    EXPECT_EQ(emitter.runtime.burstPending, 0);
    emitter.settings.maxParticles = 0;
    emitter.Burst(10);
    EXPECT_EQ(emitter.runtime.burstPending, 0);

    /// @note Serialized burst schedules must share the same saturation contract as the public Burst API.
    scene::GameObject& object = m_scene.CreateGameObject("BurstOverflow");
    scene::ParticleEmitter& scheduled = object.AddComponent<scene::ParticleEmitter>();
    scheduled.settings.maxParticles = 16;
    scheduled.settings.emitRate = 0.0f;
    scheduled.settings.receiveFlowFields = false;
    scheduled.settings.bursts = {{0.0f, (std::numeric_limits<int>::max)(), 1, 0.0f, 1.0f},
                                {0.0f, (std::numeric_limits<int>::max)(), 1, 0.0f, 1.0f}};
    scene::ScrubParticleEmitterForEditor(m_scene, m_world, object, scheduled, 0.05f, 0.05f);
    EXPECT_EQ(scheduled.runtime.particles.size(), 16u);
}

TEST_F(ParticleLoopInspectionTest, StarterSimulationRequirementsAreReported)
{
    scene::ParticleEmitterSettings settings;
    asset::ApplyParticleEmitterPreset(settings, asset::ParticleEmitterPreset::WATER_FLOW);
    EXPECT_TRUE(scene::CanUseGpuSimulation(settings));
    asset::ApplyParticleEmitterPreset(settings, asset::ParticleEmitterPreset::LIGHT_TRACE);
    settings.simulationMode = scene::ParticleSimulationMode::Gpu;
    EXPECT_EQ(scene::GetParticleGpuFallbackReason(settings), scene::ParticleGpuFallbackReason::Trail);
}


TEST_F(ParticleLoopInspectionTest, StretchedRotatedCardsFitTheAutomaticBounds)
{
    scene::GameObject& object = m_scene.CreateGameObject("StretchedBounds");
    scene::ParticleEmitter& emitter = object.AddComponent<scene::ParticleEmitter>();
    emitter.settings.playing = false;
    emitter.settings.receiveFlowFields = false;
    emitter.settings.gravity = {};
    emitter.settings.sizeAxisScale = {0.4f, 3.0f, 1.0f};
    emitter.settings.renderMode = scene::ParticleRenderMode::StretchedBillboard;
    emitter.settings.stretchedLengthScale = 2.0f;
    emitter.settings.stretchedVelocityScale = 0.5f;
    emitter.settings.culling.cullingBoundsPadding = 0.0f;
    scene::Particle particle{};
    particle.position = {4.0f, 2.0f, 0.0f};
    particle.velocity = {0.0f, 4.0f, 0.0f};
    particle.size = particle.startSize = particle.endSize = 2.0f;
    particle.rotation = 0.7853981634f;
    emitter.runtime.particles.push_back(particle);
    scene::UpdateParticleCpuSimulation(m_scene, m_world, 0.0f, 0.0f);
    /// @note A rotated (1,1) corner lies sqrt(2) along up; stretch = 2 + 4*0.5 = 4.
    const math::Vector3 renderedCorner = particle.position + math::Vector3{0.0f, std::sqrt(2.0f) * 12.0f, 0.0f};
    EXPECT_LE((renderedCorner - emitter.runtime.boundsCenter).Length(), emitter.runtime.boundsRadius);
    EXPECT_LT(emitter.runtime.boundsRadius, 18.0f);
}

TEST_F(ParticleLoopInspectionTest, BillboardAndRibbonHistoryBoundsFollowSimulationSpaceAndNegativeScale)
{
    for (bool localSpace : {false, true}) {
        for (bool ribbon : {false, true}) {
            scene::GameObject& object = m_scene.CreateGameObject("TrailBounds");
            object.transform.worldPosition = {10.0f, 3.0f, -2.0f};
            object.transform.worldScale = {-2.0f, 3.0f, 0.5f};
            object.transform.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.5707963268f);
            scene::ParticleEmitter& emitter = object.AddComponent<scene::ParticleEmitter>();
            emitter.settings.playing = false;
            emitter.settings.receiveFlowFields = false;
            emitter.settings.gravity = {};
            emitter.settings.simulationSpace = localSpace ? scene::ParticleSimulationSpace::Local : scene::ParticleSimulationSpace::World;
            emitter.settings.trail.trailEnabled = true;
            emitter.settings.trail.trailRibbon = ribbon;
            emitter.settings.trail.trailWidthScale = 3.0f;
            emitter.settings.trail.trailRibbonWidth = 0.8f;
            emitter.settings.culling.cullingBoundsPadding = 0.0f;
            scene::Particle particle{};
            particle.position = {1.0f, 2.0f, 3.0f};
            particle.size = particle.startSize = particle.endSize = 0.4f;
            particle.trailCount = 2;
            particle.trailPoints[0] = {-2.0f, 2.0f, -3.0f};
            particle.trailPoints[1] = {-3.0f, 1.0f, -4.0f};
            emitter.runtime.particles.push_back(particle);
            scene::UpdateParticleCpuSimulation(m_scene, m_world, 0.0f, 0.0f);
            const math::Vector3 expectedCenter = localSpace ? math::Vector3{11.5f, 9.0f, 0.0f} : particle.position;
            const math::Vector3 tail = localSpace ? math::Vector3{8.0f, 6.0f, -8.0f} : particle.trailPoints[1];
            EXPECT_VEC3_NEAR(emitter.runtime.boundsCenter, expectedCenter, testkit::kLooseTolerance);
            /// @note The oldest ribbon has a 1.2m half width; billboard half width is 0.6m.
            const math::Vector3 visibleEdge = tail + math::Vector3{0.0f, ribbon ? -1.2f : -0.6f, 0.0f};
            EXPECT_LE((visibleEdge - emitter.runtime.boundsCenter).Length(), emitter.runtime.boundsRadius);
            EXPECT_TRUE(std::isfinite(emitter.runtime.boundsRadius));
        }
    }
}

TEST_F(ParticleLoopInspectionTest, EmptyPoolBoundsContainSpawnShapeAndLocalMotionEnvelope)
{
    scene::GameObject& object = m_scene.CreateGameObject("EmptyBounds");
    object.transform.worldPosition = {7.0f, 1.0f, 0.0f};
    object.transform.worldScale = {-3.0f, 2.0f, 0.5f};
    scene::ParticleEmitter& emitter = object.AddComponent<scene::ParticleEmitter>();
    emitter.settings.playing = false;
    emitter.settings.receiveFlowFields = false;
    emitter.settings.gravity = {};
    emitter.settings.velocitySpread = 0.0f;
    emitter.settings.lifetimeRandom = 0.0f;
    emitter.settings.simulationSpace = scene::ParticleSimulationSpace::Local;
    emitter.settings.shape = scene::ParticleEmitterShape::Sphere;
    emitter.settings.sphereRadius = 5.0f;
    emitter.settings.emitVelocity = {1.0f, 0.0f, 0.0f};
    emitter.settings.lifetime = 2.0f;
    emitter.settings.culling.cullingBoundsPadding = 0.0f;
    scene::UpdateParticleCpuSimulation(m_scene, m_world, 0.0f, 0.0f);
    EXPECT_VEC3_NEAR(emitter.runtime.boundsCenter, object.transform.worldPosition, testkit::kTolerance);
    /// @note The negative 3x X scale places a 5m spawn point plus 2m local travel 21m from the origin.
    EXPECT_GE(emitter.runtime.boundsRadius, 21.0f);
    EXPECT_LT(emitter.runtime.boundsRadius, 23.0f);
}

TEST_F(ParticleLoopInspectionTest, NonFiniteInputsAndHugeFiniteDistancesDoNotPoisonBounds)
{
    for (bool invalid : {false, true}) {
        scene::GameObject& object = m_scene.CreateGameObject("FiniteBounds");
        scene::ParticleEmitter& emitter = object.AddComponent<scene::ParticleEmitter>();
        emitter.settings.playing = false;
        emitter.settings.receiveFlowFields = false;
        emitter.settings.gravity = {};
        emitter.settings.trail.trailEnabled = true;
        scene::Particle particle{};
        particle.position = invalid ? math::Vector3{23.0f, 2.0f, 1.0f}
            : math::Vector3{(std::numeric_limits<float>::max)() * 0.75f, 0.0f, 0.0f};
        particle.size = particle.startSize = particle.endSize = 0.4f;
        particle.trailCount = 1;
        particle.trailPoints[0] = invalid ? math::Vector3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}
            : math::Vector3{-(std::numeric_limits<float>::max)() * 0.75f, 0.0f, 0.0f};
        emitter.runtime.particles.push_back(particle);
        scene::UpdateParticleCpuSimulation(m_scene, m_world, 0.0f, 0.0f);
        EXPECT_TRUE(std::isfinite(emitter.runtime.boundsCenter.x) && std::isfinite(emitter.runtime.boundsCenter.y)
            && std::isfinite(emitter.runtime.boundsCenter.z));
        EXPECT_TRUE(std::isfinite(emitter.runtime.boundsRadius));
        EXPECT_EQ(emitter.runtime.boundsRadius, (std::numeric_limits<float>::max)());
        /// @note Keep a known center for LOD; only overflowing distances require the origin fallback.
        EXPECT_VEC3_NEAR(emitter.runtime.boundsCenter, invalid ? particle.position : math::Vector3{}, testkit::kTolerance);
    }
}

} /// @note namespace fbzz::tests
