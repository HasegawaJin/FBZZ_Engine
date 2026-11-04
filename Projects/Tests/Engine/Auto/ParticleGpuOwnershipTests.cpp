/// @file    ParticleGpuOwnershipTests.cpp
/// @brief   粒子 GPU 資源の作成元分離と Clear・Restart・容量変更時の返却を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>

namespace fbzz::tests {
class ParticleGpuOwnershipTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_frame = Time::frameCount;
        m_delta = Time::deltaTime;
        Time::deltaTime = 1.0f / 60.0f;
        ++Time::frameCount;
        ASSERT_TRUE(m_temp.IsValid());
        m_resources = std::make_unique<renderer::ResourceManager>(m_device);
        m_device.m_shaderDescriptor.cbufferSize = 16;
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
        asset::MaterialAssetAuthoring material;
        material.renderPath = asset::RenderPath::Particle;
        material.shaderPath = "ParticleOwnership.hlsl";
        const auto path = m_temp.File("Particle.mat").generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
        auto& object = m_scene.CreateGameObject("Particle");
        m_emitter = &object.AddComponent<scene::ParticleEmitter>();
        m_emitter->settings.materialPath = path;
        m_emitter->settings.simulationMode = scene::ParticleSimulationMode::Gpu;
        m_emitter->settings.simulationSpace = scene::ParticleSimulationSpace::World;
        m_emitter->settings.maxParticles = 32;
        m_emitter->settings.sortMode = scene::ParticleSortMode::BackToFront;
        m_emitter->settings.emitRate = 60.0f;
        m_emitter->settings.receiveFlowFields = false;
    }
    void TearDown() override
    {
        m_scene.Clear();
        asset::AssetManager::UnloadAll();
        m_resources.reset();
        EXPECT_EQ(m_device.m_constantsAlive, 0u);
        EXPECT_EQ(m_device.m_structuredAlive, 0u);
        Time::frameCount = m_frame;
        Time::deltaTime = m_delta;
        EngineFixture::TearDown();
    }
    renderer::RenderParticleInput Extract(testkit::ResourceTrackingRenderer& device, renderer::ResourceManager& resources)
    {
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        handles.particleShader = resources.LoadShader("Particle.hlsl");
        handles.particleGpuShader = resources.LoadShader("ParticleGpu.hlsl");
        handles.particleGpuSimCS = resources.LoadShader("ParticleSim.hlsl");
        handles.particleGpuSortKeysCS = resources.LoadShader("ParticleKeys.hlsl");
        handles.particleGpuSortStepCS = resources.LoadShader("ParticleSort.hlsl");
        handles.particleGpuSortLocalCS = resources.LoadShader("ParticleLocal.hlsl");
        const uint32_t indices[]{0, 1, 2};
        handles.particleIB = resources.CreateIndexBuffer(indices, 3);
        scene::RenderPassContext context(m_scene, device, resources, camera, settings, {}, UINT32_MAX, handles);
        renderer::RenderScene output;
        scene::ExtractRenderParticles(context, output);
        resources.Release(handles.particleIB);
        EXPECT_EQ(output.particles.size(), 1u);
        return output.particles.empty() ? renderer::RenderParticleInput{} : output.particles.front();
    }
    void ExpectReturned(const renderer::RenderParticleInput& old, renderer::ResourceManager& resources)
    {
        EXPECT_EQ(resources.Get(old.runtime.gpuParticleBuffer), nullptr);
        EXPECT_EQ(resources.Get(old.runtime.gpuEmitterCB), nullptr);
        EXPECT_EQ(resources.Get(old.runtime.gpuSortBuffer), nullptr);
        EXPECT_EQ(resources.Get(old.runtime.gpuSortCB), nullptr);
    }
    testkit::TempDir m_temp{"particle-gpu-ownership"};
    testkit::ResourceTrackingRenderer m_device;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    scene::Scene m_scene;
    scene::ParticleEmitter* m_emitter = nullptr;
    uint64_t m_frame = 0;
    float m_delta = 0.0f;
};

TEST_F(ParticleGpuOwnershipTest, ClearRestartAndCapacityChangeReturnOwnedBuffersAndKeepBorrowedPoolBuffers)
{
    auto previous = Extract(m_device, *m_resources);
    ASSERT_TRUE(previous.gpu);
    ASSERT_NE(m_resources->Get(previous.runtime.gpuParticleBuffer), nullptr);
    ASSERT_NE(m_resources->Get(previous.runtime.gpuSortBuffer), nullptr);
    ASSERT_NE(m_resources->Get(previous.runtime.gpuSortCB), nullptr);
    const auto constants = m_device.m_constantsAlive;
    for (int operation = 0; operation < 3; ++operation) {
        if (operation == 0) m_emitter->ClearParticles();
        if (operation == 1) m_emitter->ResetPlayback();
        if (operation == 2) m_emitter->settings.maxParticles = 64;
        ++Time::frameCount;
        const auto current = Extract(m_device, *m_resources);
        ExpectReturned(previous, *m_resources);
        EXPECT_NE(m_resources->Get(current.runtime.gpuParticleBuffer), nullptr);
        EXPECT_EQ(m_device.m_constantsAlive, constants);
        EXPECT_NE(m_resources->Get(previous.runtime.gpuSpawnBuffer), nullptr);
        previous = current;
    }
    m_scene.Clear();
    ExpectReturned(previous, *m_resources);
    EXPECT_EQ(m_device.m_constantsAlive, 0u);
    EXPECT_NE(m_resources->Get(previous.runtime.gpuSpawnBuffer), nullptr);
}

TEST_F(ParticleGpuOwnershipTest, AlternatingManagersRecreateEmitterBuffersAndRetainPerOwnerMaterial)
{
    testkit::ResourceTrackingRenderer otherDevice;
    otherDevice.m_shaderDescriptor.cbufferSize = 16;
    renderer::ResourceManager other(otherDevice);
    const auto first = Extract(m_device, *m_resources);
    ASSERT_NE(m_resources->Get(first.runtime.materialParamsCB), nullptr);
    const auto switched = Extract(otherDevice, other);
    EXPECT_TRUE(switched.simulate);
    ExpectReturned(first, *m_resources);
    EXPECT_EQ(m_resources->Get(first.runtime.renderCB), nullptr);
    ASSERT_NE(other.Get(switched.runtime.gpuParticleBuffer), nullptr);
    EXPECT_NE(other.Get(switched.runtime.gpuEmitterCB), nullptr);
    EXPECT_NE(other.Get(switched.runtime.customShader), nullptr);
    EXPECT_NE(other.Get(switched.runtime.materialParamsCB), nullptr);
    const auto repeated = Extract(m_device, *m_resources);
    EXPECT_TRUE(repeated.simulate);
    ExpectReturned(switched, other);
    EXPECT_EQ(repeated.runtime.materialParamsCB, first.runtime.materialParamsCB);
    EXPECT_EQ(repeated.runtime.customShader, first.runtime.customShader);
    m_scene.Clear();
    EXPECT_EQ(m_device.m_constantsAlive, 0u);
    EXPECT_EQ(otherDevice.m_constantsAlive, 0u);
}

TEST_F(ParticleGpuOwnershipTest, ResetRecreatesEmitterAndMaterialResourcesWithoutReleasingUnrelatedBuffers)
{
    const auto initial = Extract(m_device, *m_resources);
    m_resources->Reset();
    const auto unrelated = m_resources->CreateConstantBuffer(16);
    const auto recreated = Extract(m_device, *m_resources);
    EXPECT_TRUE(recreated.simulate);
    EXPECT_NE(recreated.runtime.gpuParticleBuffer, initial.runtime.gpuParticleBuffer);
    EXPECT_NE(m_resources->Get(recreated.runtime.gpuParticleBuffer), nullptr);
    EXPECT_NE(m_resources->Get(recreated.runtime.materialParamsCB), nullptr);
    EXPECT_NE(m_resources->Get(recreated.runtime.renderCB), nullptr);
    EXPECT_NE(m_resources->Get(unrelated), nullptr);
    m_scene.Clear();
    EXPECT_EQ(m_device.m_constantsAlive, 1u);
    m_resources->Release(unrelated);
}

TEST_F(ParticleGpuOwnershipTest, ShaderReloadWithoutParametersReturnsOldMaterialBuffer)
{
    const auto initial = Extract(m_device, *m_resources);
    ASSERT_NE(m_resources->Get(initial.runtime.materialParamsCB), nullptr);
    m_device.m_shaderDescriptor = {};
    ASSERT_TRUE(m_resources->ReloadShader("ParticleOwnership.hlsl").IsValid());
    const auto reloaded = Extract(m_device, *m_resources);
    EXPECT_FALSE(reloaded.runtime.materialParamsCB.IsValid());
    EXPECT_EQ(m_resources->Get(initial.runtime.materialParamsCB), nullptr);
}
} /// @note namespace fbzz::tests
