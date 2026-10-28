/// @file    ManagedParticleServicesTests.cpp
/// @brief   粒子の cache失効・CPU縮退・入力原子性・typed asset拒否の回帰。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedParticleServices.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include <array>
#include <limits>
namespace fbzz::tests {
using ParticleStatus = scene::ManagedBridgeStatus;
class ManagedParticleServicesTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        m_object = &m_scene.CreateGameObject("Particle facade"); m_view.SetContext(&m_scene, m_object);
        m_object->AddComponent<scene::ParticleEmitter>();
    }
    void TearDown() override
    { m_view.SetContext(nullptr, nullptr); m_scene.Clear(); scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown(); }
    ParticleStatus Run(uint32_t selector, bool read, int64_t integer = 0,
        std::span<const float> values = {}, std::span<const std::string> text = {})
    {
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::SERVICE_PARTICLE; request.index = selector; request.flags = read ? 1 : 0;
        return scene::managed_detail::RunParticleServices(m_view, *m_object, request, integer, values, text, m_result);
    }
    scene::Scene m_scene;
    scene::Script m_view;
    scene::GameObject* m_object = nullptr;
    scene::managed_detail::ManagedServiceResult m_result;
    bool m_previousPlay = false;
};
TEST_F(ManagedParticleServicesTest, CapacityAndSimulationCommandsInvalidateNativeGpuState)
{
    auto& particle = *m_object->GetComponent<scene::ParticleEmitter>(); particle.runtime.gpuInitialized = true; particle.runtime.gpuCapacity = 300;
    ASSERT_EQ(Run(26, false, 0), ParticleStatus::OK);
    EXPECT_EQ(particle.settings.maxParticles, 1); EXPECT_FALSE(particle.runtime.gpuInitialized); EXPECT_EQ(particle.runtime.gpuCapacity, 0);
    particle.runtime.gpuClearPending = false;
    EXPECT_EQ(Run(44, false, 2), ParticleStatus::INVALID_ARGUMENT); EXPECT_FALSE(particle.runtime.gpuClearPending);
    ASSERT_EQ(Run(44, false, 1), ParticleStatus::OK); EXPECT_TRUE(particle.runtime.gpuClearPending); EXPECT_EQ(particle.settings.simulationMode, scene::ParticleSimulationMode::Gpu);
    ASSERT_EQ(Run(46, false, 1), ParticleStatus::OK); EXPECT_EQ(particle.settings.simulationMode, scene::ParticleSimulationMode::Cpu);
    ASSERT_EQ(Run(43, true), ParticleStatus::OK); EXPECT_EQ(m_result.integer, 0);
    ASSERT_EQ(Run(44, false, 1), ParticleStatus::OK);
    ASSERT_EQ(Run(69, true), ParticleStatus::OK); EXPECT_EQ(m_result.integer, static_cast<int>(scene::ParticleGpuFallbackReason::LocalSpace));
    const std::array<float, 2> collision{0.1f, 0.5f}; ASSERT_EQ(Run(50, false, 1, collision), ParticleStatus::OK);
    EXPECT_EQ(particle.settings.simulationMode, scene::ParticleSimulationMode::Cpu); EXPECT_EQ(particle.settings.collisionMode, scene::ParticleCollisionMode::Physics);
}
TEST_F(ManagedParticleServicesTest, NonfiniteAndScopeFailuresLeaveWholeParticleGroupsUntouched)
{
    auto& settings = m_object->GetComponent<scene::ParticleEmitter>()->settings; settings.colorStart = {1, 2, 3, 4}; settings.colorEnd = {5, 6, 7, 8};
    const std::array<float, 8> invalid{9, 10, 11, 12, 13, 14, std::numeric_limits<float>::quiet_NaN(), 16}; m_result.integer = 77;
    EXPECT_EQ(Run(22, false, 0, invalid), ParticleStatus::INVALID_ARGUMENT);
    EXPECT_NEAR(settings.colorStart.x, 1, testkit::kTolerance); EXPECT_NEAR(settings.colorEnd.z, 7, testkit::kTolerance); EXPECT_EQ(m_result.integer, 77);
    const std::array<float, 2> sizes{-2, 3}; ASSERT_EQ(Run(24, false, 0, sizes), ParticleStatus::OK);
    EXPECT_NEAR(settings.sizeStart, 0, testkit::kTolerance); EXPECT_NEAR(settings.sizeEnd, 3, testkit::kTolerance);
    scene::Script::SetInPlayMode(false); const std::array<float, 3> acceleration{0, -9.8f, 0};
    EXPECT_EQ(Run(20, false, 0, acceleration), ParticleStatus::INVALID_SCOPE);
    ASSERT_EQ(Run(19, true), ParticleStatus::OK); ASSERT_EQ(m_result.values.size(), 3U); EXPECT_NEAR(m_result.values[1], -5, testkit::kTolerance);
}
TEST_F(ManagedParticleServicesTest, PrewarmNoiseSubEmitterAndChannelGroupsReuseNativeContracts)
{
    auto& particle = *m_object->GetComponent<scene::ParticleEmitter>(); auto& scope = m_scene.CreateGameObject("VFX instance");
    particle.settings.subEmitterScopeRoot = scope.GetID(); particle.runtime.prewarmed = true; particle.runtime.prewarmSpawnPending = 5;
    ASSERT_EQ(Run(55, false, 1), ParticleStatus::OK); EXPECT_FALSE(particle.runtime.prewarmed); EXPECT_EQ(particle.runtime.prewarmSpawnPending, 0);
    ASSERT_EQ(Run(44, false, 1), ParticleStatus::OK); ASSERT_EQ(Run(69, true), ParticleStatus::OK);
    EXPECT_EQ(m_result.integer, static_cast<int>(scene::ParticleGpuFallbackReason::Prewarm));
    ASSERT_EQ(Run(62, true), ParticleStatus::OK); EXPECT_TRUE(particle.settings.localForces.empty());
    const std::array<float, 3> noise{2, -1, -3}; ASSERT_EQ(Run(63, false, 0, noise), ParticleStatus::OK);
    ASSERT_EQ(particle.settings.localForces.size(), 1U); EXPECT_EQ(particle.settings.localForces[0].fieldType, scene::FlowFieldType::Curl);
    EXPECT_NEAR(particle.settings.localForces[0].noiseFrequency, 0.0001f, testkit::kTolerance); EXPECT_NEAR(particle.settings.localForces[0].noiseSpeed, -3, testkit::kTolerance);
    const std::array<std::string, 3> sub{"Birth", "", "Collision"}; ASSERT_EQ(Run(57, false, 2, {}, sub), ParticleStatus::OK);
    EXPECT_EQ(particle.settings.birthSubEmitter, "Birth"); EXPECT_TRUE(particle.settings.deathSubEmitter.empty()); EXPECT_EQ(particle.settings.subEmitterScopeRoot, scope.GetID());
    ASSERT_EQ(Run(67, false, 0x80000001LL), ParticleStatus::OK); ASSERT_EQ(Run(66, true), ParticleStatus::OK); EXPECT_EQ(m_result.integer, 0x80000001LL);
}
TEST_F(ManagedParticleServicesTest, InvalidTypedAssetsPreserveAssignmentAndMeshCacheWhileEmptyExplicitlyClears)
{
    auto& particle = *m_object->GetComponent<scene::ParticleEmitter>(); particle.settings.meshShapePath = "Old.fbx";
    particle.runtime.loadedMeshShapePath = "Old.fbx"; particle.runtime.meshShapeVertices.emplace_back();
    const std::array<float, 3> shape{2, -1, 1}; const std::array<std::string, 1> wrong{"Assets/NotAGuid.fbx"}, empty{""};
    ASSERT_EQ(Run(38, false, -1, shape, wrong), ParticleStatus::OK); EXPECT_EQ(m_result.integer, 0);
    EXPECT_EQ(particle.settings.meshShapePath, "Old.fbx"); EXPECT_EQ(particle.runtime.loadedMeshShapePath, "Old.fbx"); EXPECT_EQ(particle.runtime.meshShapeVertices.size(), 1U);
    ASSERT_EQ(Run(38, false, -1, shape, empty), ParticleStatus::OK); EXPECT_EQ(m_result.integer, 1);
    EXPECT_TRUE(particle.settings.meshShapePath.empty()); EXPECT_TRUE(particle.runtime.meshShapeVertices.empty()); EXPECT_EQ(particle.settings.shape, scene::ParticleEmitterShape::MeshSurface);
    particle.settings.materialPath = "Previous.mat";
    ASSERT_EQ(Run(73, false, 0, {}, wrong), ParticleStatus::OK); EXPECT_EQ(m_result.integer, 0); EXPECT_EQ(particle.settings.materialPath, "Previous.mat");
    ASSERT_EQ(Run(73, false, 0, {}, empty), ParticleStatus::OK); EXPECT_EQ(m_result.integer, 1); EXPECT_TRUE(particle.settings.materialPath.empty());
}
} /// @note namespace fbzz::tests
