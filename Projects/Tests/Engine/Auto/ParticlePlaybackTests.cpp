/// @file    ParticlePlaybackTests.cpp
/// @brief   実行時と編集スクラブで粒子の時刻と発生契約が一致することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Systems/ParticleSimulationSystem.hpp>
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include <Physics/World.hpp>

namespace fbzz::tests {
class ParticlePlaybackTest : public testkit::EngineFixture {
protected:
    void SetUp() override { EngineFixture::SetUp(); m_frame = Time::frameCount; }
    void TearDown() override { Time::frameCount = m_frame; EngineFixture::TearDown(); }
    scene::ParticleEmitter& Create(const char* name)
    {
        m_object = &m_scene.CreateGameObject(name);
        auto& emitter = m_object->AddComponent<scene::ParticleEmitter>();
        emitter.settings.simulationMode = scene::ParticleSimulationMode::Cpu;
        emitter.settings.receiveFlowFields = false;
        emitter.settings.gravity = {};
        emitter.settings.emitRate = 0.0f;
        emitter.settings.lifetime = 4.0f;
        emitter.settings.lifetimeRandom = 0.0f;
        emitter.settings.maxParticles = 64;
        return emitter;
    }
    void Run(scene::ParticleEmitter& emitter, scene::GameObject& object, int frames)
    {
        for (int index = 0; index < frames; ++index) {
            ++Time::frameCount;
            (void)scene::AdvanceParticleEmitterPlayback(emitter, object.transform, 1.0f / 60.0f);
            scene::UpdateParticleCpuSimulation(m_scene, m_world, 1.0f / 60.0f, static_cast<float>(index + 1) / 60.0f);
        }
    }
    scene::Scene m_scene;
    scene::GameObject* m_object = nullptr;
    physics::World m_world;
    uint64_t m_frame = 0;
};

TEST_F(ParticlePlaybackTest, TimedBurstWithoutDurationFiresOnceInPlaybackAndScrub)
{
    auto& emitter = Create("Continuous");
    auto& object = *m_object;
    emitter.settings.duration = 0.0f;
    emitter.settings.bursts = {{0.25f, 3, 1, 0.0f, 1.0f}};
    Run(emitter, object, 30);
    ASSERT_EQ(emitter.runtime.particles.size(), 3u);
    const auto particles = emitter.runtime.particles;
    const auto playTime = emitter.runtime.playTime;
    scene::ScrubParticleEmitterForEditor(m_scene, m_world, object, emitter, 0.5f, 0.5f);
    ASSERT_EQ(emitter.runtime.particles.size(), 3u);
    EXPECT_NEAR(emitter.runtime.playTime, playTime, testkit::kTolerance);
    for (size_t index = 0; index < particles.size(); ++index) {
        EXPECT_VEC3_NEAR(emitter.runtime.particles[index].position, particles[index].position, testkit::kTolerance);
        EXPECT_VEC3_NEAR(emitter.runtime.particles[index].velocity, particles[index].velocity, testkit::kTolerance);
    }
}

TEST_F(ParticlePlaybackTest, CompactsExpiredAndKilledParticlesInTheirOriginalOrder)
{
    auto& emitter = Create("Compact");
    emitter.settings.collisionMode = scene::ParticleCollisionMode::Plane;
    emitter.settings.collisionResponse = scene::ParticleCollisionResponse::Kill;
    emitter.settings.collisionPlaneY = 0;
    emitter.runtime.particles.resize(5);
    for (size_t i = 0; i < emitter.runtime.particles.size(); ++i) {
        auto& particle = emitter.runtime.particles[i];
        particle.position = {static_cast<float>(i), 10, 0};
        particle.lifetime = 2;
    }
    emitter.runtime.particles[0].lifetime = 0;
    emitter.runtime.particles[2].position.y = -1;
    emitter.runtime.particles[4].lifetime = 0;
    ++Time::frameCount;
    scene::UpdateParticleCpuSimulation(m_scene, m_world, 0.1f, 0.1f);
    ASSERT_EQ(emitter.runtime.particles.size(), 2u);
    EXPECT_FLOAT_EQ(emitter.runtime.particles[0].position.x, 1);
    EXPECT_FLOAT_EQ(emitter.runtime.particles[1].position.x, 3);
    EXPECT_EQ(emitter.runtime.deathCountThisFrame, 2);
    EXPECT_EQ(emitter.runtime.collisionCountThisFrame, 1);
}

TEST_F(ParticlePlaybackTest, DelayLoopAndPrewarmUseTheSamePlaybackStep)
{
    auto& emitter = Create("Loop");
    auto& object = *m_object;
    emitter.settings.duration = 0.2f;
    emitter.settings.startDelay = 0.05f;
    emitter.settings.loop = true;
    emitter.settings.prewarm = true;
    emitter.settings.emitRate = 10.0f;
    emitter.settings.bursts = {{0.1f, 2, 1, 0.0f, 1.0f}};
    Run(emitter, object, 30);
    const auto count = emitter.runtime.particles.size();
    const auto playTime = emitter.runtime.playTime;
    const auto fired = emitter.runtime.burstCyclesFired;
    ASSERT_GT(count, 2u);
    scene::ScrubParticleEmitterForEditor(m_scene, m_world, object, emitter, 0.5f, 0.5f);
    EXPECT_EQ(emitter.runtime.particles.size(), count);
    EXPECT_EQ(emitter.runtime.burstCyclesFired, fired);
    EXPECT_TRUE(emitter.runtime.prewarmed);
    EXPECT_NEAR(emitter.runtime.playTime, playTime, testkit::kTolerance);
}

TEST_F(ParticlePlaybackTest, RepeatedPlaybackInOneFrameDoesNotAdvanceTimeOrRepeatBurst)
{
    auto& emitter = Create("FrameGuard");
    auto& object = *m_object;
    emitter.settings.duration = 0.0f;
    emitter.settings.bursts = {{0.05f, 2, 1, 0.0f, 1.0f}};
    ++Time::frameCount;
    EXPECT_TRUE(scene::AdvanceParticleEmitterPlayback(emitter, object.transform, 0.1f));
    EXPECT_TRUE(scene::AdvanceParticleEmitterPlayback(emitter, object.transform, 0.1f));
    EXPECT_NEAR(emitter.runtime.playTime, 0.1f, testkit::kTolerance);
    EXPECT_EQ(emitter.runtime.burstPending, 2);
}

TEST_F(ParticlePlaybackTest, RoundedWholeFrameScrubDoesNotLoseTheLastEmission)
{
    auto& emitter = Create("RoundedFrame");
    auto& object = *m_object;
    emitter.settings.duration = 0.0f;
    emitter.settings.emitRate = 60.0f;
    Run(emitter, object, 31);
    ASSERT_EQ(emitter.runtime.particles.size(), 31u);
    const auto particles = emitter.runtime.particles;
    const float target = 31.0f / 60.0f;
    scene::ScrubParticleEmitterForEditor(m_scene, m_world, object, emitter, target, target);
    ASSERT_EQ(emitter.runtime.particles.size(), 31u);
    for (size_t index = 0; index < particles.size(); ++index)
        EXPECT_VEC3_NEAR(emitter.runtime.particles[index].position, particles[index].position, testkit::kTolerance);
}
TEST_F(ParticlePlaybackTest, SchedulerUpdatesCpuParticlesWithoutResourcesOrAnyViewport)
{
    auto& emitter = Create("Headless");
    emitter.settings.duration = 0.0f;
    emitter.settings.emitRate = 60.0f;
    emitter.settings.emitVelocity = {1, 0, 0};
    emitter.settings.velocitySpread = 0.0f;
    scene::ParticleSimulationSystem system;
    SystemContext context{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f, 0, true, true};
    ++Time::frameCount;
    system.Update(context);
    ASSERT_EQ(emitter.runtime.particles.size(), 1u);
    const auto first = emitter.runtime.particles[0];
    const auto time = emitter.runtime.playTime;
    system.Update(context);
    EXPECT_EQ(emitter.runtime.particles.size(), 1u);
    EXPECT_NEAR(emitter.runtime.playTime, time, testkit::kTolerance);
    EXPECT_NEAR(emitter.runtime.particles[0].age, first.age, testkit::kTolerance);
    ++Time::frameCount;
    system.Update(context);
    ASSERT_EQ(emitter.runtime.particles.size(), 2u);
    EXPECT_GT(emitter.runtime.particles[0].position.x, first.position.x);
    EXPECT_FALSE(emitter.runtime.gpuParticleBuffer.IsValid());
}

TEST_F(ParticlePlaybackTest, HeadlessSimulationReadsFlipbookSettingsBeforeAnyGpuMaterialPreparation)
{
    testkit::TempDir temp("particle-cpu-material");
    testkit::ResourceTrackingRenderer device;
    renderer::ResourceManager resources(device);
    asset::AssetManager::Init(resources, temp.Path().generic_string() + "/");
    asset::MaterialAssetAuthoring material;
    material.renderPath = asset::RenderPath::Particle;
    material.particle.flipbook.spriteColumns = 4;
    material.particle.flipbook.spriteRows = 2;
    const auto path = temp.File("Flipbook.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
    auto& emitter = Create("Material");
    emitter.settings.materialPath = path;
    emitter.settings.duration = 0.0f;
    emitter.settings.emitRate = 60.0f;
    scene::ParticleSimulationSystem system;
    SystemContext context{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f, 0, true, true};
    ++Time::frameCount;
    system.Update(context);
    ASSERT_EQ(emitter.runtime.particles.size(), 1u);
    EXPECT_EQ(emitter.runtime.material.flipbook.spriteColumns, 4);
    const auto uv = emitter.runtime.particles.front().uvRect;
    EXPECT_NEAR(uv.z - uv.x, 0.25f, testkit::kTolerance);
    EXPECT_NEAR(uv.w - uv.y, 0.5f, testkit::kTolerance);
    EXPECT_EQ(device.m_texturesAlive, 0u);
    EXPECT_EQ(device.m_constantsAlive, 0u);
    asset::AssetManager::UnloadAll();
}

TEST_F(ParticlePlaybackTest, RenderExtractionConsumesCpuSnapshotWithoutAdvancingPlaybackOrCollision)
{
    testkit::TempDir temp("particle-cpu-snapshot");
    testkit::ResourceTrackingRenderer device;
    renderer::ResourceManager resources(device);
    asset::AssetManager::Init(resources, temp.Path().generic_string() + "/");
    auto& emitter = Create("Snapshot");
    emitter.settings.duration = 0.0f;
    emitter.settings.emitRate = 60.0f;
    scene::ParticleSimulationSystem system;
    SystemContext context{m_scene, m_world, nullptr, nullptr, 1.0f / 60.0f, 0, true, true};
    ++Time::frameCount;
    system.Update(context);
    ASSERT_EQ(emitter.runtime.particles.size(), 1u);
    const auto particle = emitter.runtime.particles.front();
    const float playTime = emitter.runtime.playTime;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles handles;
    handles.particleShader = resources.LoadShader("Particle.hlsl");
    const uint32_t indices[]{0, 1, 2};
    handles.particleIB = resources.CreateIndexBuffer(indices, 3);
    scene::RenderPassContext render(m_scene, device, resources, camera, settings, {}, UINT32_MAX, handles);
    renderer::RenderScene output;
    for (int view = 0; view < 2; ++view) scene::ExtractRenderParticles(render, output);
    EXPECT_NEAR(emitter.runtime.playTime, playTime, testkit::kTolerance);
    ASSERT_EQ(emitter.runtime.particles.size(), 1u);
    EXPECT_NEAR(emitter.runtime.particles.front().age, particle.age, testkit::kTolerance);
    EXPECT_VEC3_NEAR(emitter.runtime.particles.front().position, particle.position, testkit::kTolerance);
    ++Time::frameCount;
    scene::ExtractRenderParticles(render, output);
    EXPECT_NEAR(emitter.runtime.playTime, playTime, testkit::kTolerance);
    EXPECT_NEAR(emitter.runtime.particles.front().age, particle.age, testkit::kTolerance);
    m_scene.Clear();
    asset::AssetManager::UnloadAll();
    resources.Release(handles.particleIB);
}
} /// @note namespace fbzz::tests
