/// @file    ParticleGpuFallbackTests.cpp
/// @brief   GPU シミュレーションから CPU へ縮退する条件を固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// 焼いた煙・炎はほぼ必ず Frame Blending と Motion Vector を使う。これが縮退条件に戻ると、
/// 粒子数を増やしたいエミッターほど黙って CPU へ落ちる。

#include <TestKit/TestKit.hpp>

#include <Engine/Asset/ParticleMaterialSettings.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>

namespace fbzz::tests {
namespace {

scene::ParticleEmitterSettings GpuEmitter()
{
    scene::ParticleEmitterSettings settings;
    settings.simulationMode = scene::ParticleSimulationMode::Gpu;
    settings.simulationSpace = scene::ParticleSimulationSpace::World;
    settings.collisionMode = scene::ParticleCollisionMode::None;
    settings.prewarm = false;
    settings.trail.trailEnabled = false;
    return settings;
}

} // namespace

/// @brief Plain GPU emitter runs on the GPU.
TEST(ParticleGpuFallbackTest, PlainGpuEmitterRunsOnTheGpu)
{
    const asset::ParticleMaterialSettings material;
    EXPECT_EQ(scene::GetParticleGpuFallbackReason(GpuEmitter(), &material), scene::ParticleGpuFallbackReason::None);
}

/// @brief Frame blending and motion vectors stay on the GPU.
TEST(ParticleGpuFallbackTest, FrameBlendingAndMotionVectorsStayOnTheGpu)
{
    asset::ParticleMaterialSettings material;
    material.flipbook.spriteColumns = 8;
    material.flipbook.spriteRows = 8;
    material.flipbook.flipbookFrameBlending = true;
    material.flipbook.motionVectorFlipbook = true;
    material.flipbook.motionVectorStrength = 0.05f;
    EXPECT_EQ(scene::GetParticleGpuFallbackReason(GpuEmitter(), &material), scene::ParticleGpuFallbackReason::None);
    EXPECT_TRUE(scene::CanUseGpuSimulation(GpuEmitter(), &material));
}

/// @brief Six way maps and point lights stay on the GPU.
TEST(ParticleGpuFallbackTest, SixWayMapsAndPointLightsStayOnTheGpu)
{
    asset::ParticleMaterialSettings material;
    material.sixWayMaps = true;
    material.punctualLighting = true;
    EXPECT_EQ(scene::GetParticleGpuFallbackReason(GpuEmitter(), &material), scene::ParticleGpuFallbackReason::None);
}

/// @brief Trail still falls back to the CPU.
TEST(ParticleGpuFallbackTest, TrailStillFallsBackToTheCpu)
{
    scene::ParticleEmitterSettings settings = GpuEmitter();
    settings.trail.trailEnabled = true;
    const asset::ParticleMaterialSettings material;
    EXPECT_EQ(scene::GetParticleGpuFallbackReason(settings, &material), scene::ParticleGpuFallbackReason::Trail);
}

} // namespace fbzz::tests
