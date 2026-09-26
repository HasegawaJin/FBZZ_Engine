/// @file    ParticleLoopInspectionTests.cpp
/// @brief   焼いたフリップブックの粒子再生が周期境界で空白にならないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-24
#include <TestKit/TestKit.hpp>
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

} /// @note namespace fbzz::tests
