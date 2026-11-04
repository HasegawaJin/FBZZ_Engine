/// @file    TransientGpuStateTests.cpp
/// @brief   Component の複製・移動・代入・削除で GPU 所有権が一意に保たれることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <any>

namespace fbzz::tests {
class TransientGpuStateTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_resources = std::make_unique<renderer::ResourceManager>(m_device);
    }
    void TearDown() override
    {
        m_resources.reset();
        EXPECT_EQ(m_device.m_constantsAlive, 0u);
        EXPECT_EQ(m_device.m_buffersAlive, 0u);
        EXPECT_EQ(m_device.m_texturesAlive, 0u);
        EXPECT_EQ(m_device.m_targetsAlive, 0u);
        EngineFixture::TearDown();
    }
    renderer::ResourceHandle<renderer::ConstantBufferTag> Constant() { return m_resources->CreateConstantBuffer(16); }
    renderer::ResourceHandle<renderer::BufferTag> Buffer() { return m_resources->CreateVertexBuffer(nullptr, 48, 16); }
    renderer::ResourceHandle<renderer::TextureTag> Texture()
    {
        const uint8_t pixel[]{255, 255, 255, 255};
        return m_resources->CreateTexture(pixel, 1, 1);
    }
    void Populate(scene::DoubleBufferedMesh& mesh)
    {
        for (auto& slot : mesh.slots) {
            slot = std::make_unique<renderer::Mesh>();
            slot->vertexBuffer = Buffer();
            const uint32_t indices[]{0, 1, 2};
            slot->indexBuffer = m_resources->CreateIndexBuffer(indices, 3);
        }
        mesh.geometryValid = true;
    }
    testkit::ResourceTrackingRenderer m_device;
    std::unique_ptr<renderer::ResourceManager> m_resources;
};

TEST_F(TransientGpuStateTest, ComponentAndUndoValueCopiesKeepSettingsWithoutOwningTheSourcePalette)
{
    scene::AnimatorComponent source;
    source.skinningBuffer = Constant(); source.prevSkinningBuffer = Constant();
    source.speed = 2.0f;
    const auto handle = source.skinningBuffer.Get();
    const std::any undo = source;
    const auto& copied = std::any_cast<const scene::AnimatorComponent&>(undo);
    EXPECT_FALSE(copied.skinningBuffer.IsValid());
    EXPECT_FALSE(copied.prevSkinningBuffer.IsValid());
    EXPECT_NEAR(copied.speed, 2.0f, testkit::kTolerance);
    scene::AnimatorComponent target;
    target.skinningBuffer = Constant();
    const auto replaced = target.skinningBuffer.Get();
    target = copied;
    EXPECT_EQ(m_resources->Get(replaced), nullptr);
    EXPECT_NE(m_resources->Get(handle), nullptr);
    source = source;
    EXPECT_EQ(source.skinningBuffer.Get(), handle);
    scene::AnimatorComponent moved = std::move(source);
    EXPECT_EQ(moved.skinningBuffer.Get(), handle);
    EXPECT_FALSE(source.skinningBuffer.IsValid());
}

TEST_F(TransientGpuStateTest, SkinnedCopiesRetainRequestedWeightsAndRebuildEveryGpuOutput)
{
    scene::SkinnedMeshRenderer source;
    source.morphWeights["Smile"] = 0.5f;
    source.appliedMorphWeights = source.morphWeights;
    source.morphVertexBuffers.emplace_back(Buffer());
    source.skinnedVertexBuffers.emplace_back(Buffer());
    const auto handle = source.morphVertexBuffers[0].Get();
    scene::SkinnedMeshRenderer copy = source;
    EXPECT_EQ(copy.morphWeights, source.morphWeights);
    EXPECT_TRUE(copy.appliedMorphWeights.empty());
    EXPECT_TRUE(copy.morphVertexBuffers.empty());
    EXPECT_TRUE(copy.skinnedVertexBuffers.empty());
    scene::SkinnedMeshRenderer moved;
    moved.morphVertexBuffers.emplace_back(Buffer());
    const auto replaced = moved.morphVertexBuffers[0].Get();
    moved = std::move(source);
    EXPECT_EQ(m_resources->Get(replaced), nullptr);
    EXPECT_EQ(moved.morphVertexBuffers[0].Get(), handle);
    EXPECT_TRUE(source.morphVertexBuffers.empty());
}

TEST_F(TransientGpuStateTest, ParticleCopiesRestartCpuHistoryAndResetOwnedAndBorrowedGpuState)
{
    scene::ParticleEmitter source;
    source.runtime.gpuEmitterCB = Constant(); source.runtime.renderCB = Constant();
    source.runtime.gpuParticleBuffer = m_resources->CreateStructuredBuffer(nullptr, 8, 16);
    source.runtime.gpuSpawnBuffer = source.runtime.gpuParticleBuffer;
    source.runtime.gpuInitialized = true; source.runtime.gpuCapacity = 8;
    source.runtime.particles.resize(1);
    source.runtime.particles[0].age = 0.5f;
    source.settings.emitRate = 23.0f;

    const scene::ParticleEmitter copy = source;

    EXPECT_TRUE(copy.runtime.particles.empty());
    EXPECT_NEAR(copy.settings.emitRate, 23.0f, testkit::kTolerance);
    ASSERT_EQ(source.runtime.particles.size(), 1u);
    EXPECT_NEAR(source.runtime.particles[0].age, 0.5f, testkit::kTolerance);
    EXPECT_FALSE(copy.runtime.renderCB.IsValid());
    EXPECT_FALSE(copy.runtime.gpuEmitterCB.IsValid());
    EXPECT_FALSE(copy.runtime.gpuParticleBuffer.IsValid());
    EXPECT_FALSE(copy.runtime.gpuSpawnBuffer.IsValid());
    EXPECT_FALSE(copy.runtime.gpuInitialized);
    EXPECT_EQ(copy.runtime.gpuCapacity, 0);
    EXPECT_NE(m_resources->Get(source.runtime.gpuParticleBuffer), nullptr);
}

TEST_F(TransientGpuStateTest, TrailAndSampleCopiesRecreateBuffersWithoutAnExternalCleanupList)
{
    scene::TrailComponent trail;
    trail.trailCB = Constant();
    const scene::TrailComponent trailCopy = trail;
    EXPECT_FALSE(trailCopy.trailCB.IsValid());
    scene::MeshTrailComponent source;
    source.duration = 1.5f;
    source.meshTrailCB = Constant();
    source.samples.resize(1); source.samples[0].skinningCB = Constant();
    source.samples[0].skinningCBDirty = false;
    source.samples[0].boneMatrices.push_back(math::Matrix4::Identity());
    const auto sample = source.samples[0];
    EXPECT_FALSE(sample.skinningCB.IsValid());
    EXPECT_TRUE(sample.skinningCBDirty);
    EXPECT_EQ(sample.boneMatrices.size(), 1u);
    const auto copy = source;
    EXPECT_TRUE(copy.samples.empty());
    EXPECT_FALSE(copy.meshTrailCB.IsValid());
    EXPECT_NEAR(copy.duration, source.duration, testkit::kTolerance);
}

TEST_F(TransientGpuStateTest, ProbeCopiesResetPublicationAndReturnReplacedTextures)
{
    scene::ReflectionProbeComponent source;
    source.runtimeIrradiance = Texture();
    source.runtimeCubeRT = m_resources->CreateRenderTarget(4, 4);
    source.runtimePublishedIrradiance = source.runtimeIrradiance;
    source.runtimeImmutablePublished = true;
    source.intensity = 3.0f;
    scene::ReflectionProbeComponent copy = source;
    EXPECT_FALSE(copy.runtimeCubeRT.IsValid());
    EXPECT_FALSE(copy.runtimeIrradiance.IsValid());
    EXPECT_FALSE(copy.runtimePublishedIrradiance.IsValid());
    EXPECT_FALSE(copy.runtimeImmutablePublished);
    EXPECT_NEAR(copy.intensity, 3.0f, testkit::kTolerance);
    scene::LightProbeVolumeComponent volume;
    volume.runtimeVolume = Texture();
    volume.runtimeFaces[0] = m_resources->CreateRenderTarget(4, 4);
    volume.runtimeReady = true; volume.runtimeCursor = 3;
    const auto replaced = volume.runtimeVolume.Get();
    const scene::LightProbeVolumeComponent empty;
    volume = empty;
    EXPECT_EQ(m_resources->Get(replaced), nullptr);
    EXPECT_FALSE(volume.runtimeReady);
    EXPECT_EQ(volume.runtimeCursor, 0);
}

TEST_F(TransientGpuStateTest, DoubleBufferedMeshCopyAssignmentMoveAndDestructionReturnBothSlots)
{
    scene::DoubleBufferedMesh source, target;
    Populate(source); Populate(target);
    const auto sourceBuffer = source.Current()->vertexBuffer;
    const auto replaced = target.Current()->vertexBuffer;
    target = source;
    EXPECT_FALSE(target.HasMesh());
    EXPECT_EQ(m_resources->Get(replaced), nullptr);
    EXPECT_NE(m_resources->Get(sourceBuffer), nullptr);
    source = source;
    EXPECT_TRUE(source.HasMesh());
    target = std::move(source);
    EXPECT_FALSE(source.HasMesh());
    EXPECT_EQ(target.Current()->vertexBuffer, sourceBuffer);
    target.Reset();
    EXPECT_EQ(m_device.m_buffersAlive, 0u);
}

TEST_F(TransientGpuStateTest, SceneCopiesDoNotBorrowAnotherEntityRuntimeMeshAndRemovalUnlinksItsView)
{
    scene::Scene source, destination;
    auto& object = source.CreateGameObject("Procedural");
    auto& mesh = object.AddComponent<scene::ProceduralMeshComponent>();
    EXPECT_EQ(mesh.dirty, scene::MeshDirty::None);
    Populate(mesh.runtimeMesh);
    auto& view = object.AddComponent<scene::MeshRenderer>();
    view.mesh = mesh.runtimeMesh.Current();
    auto& clone = destination.CreateGameObject("Clone");
    destination.CopyComponentsFromPassive(source, object.GetID(), clone.GetID());
    EXPECT_EQ(clone.GetComponent<scene::MeshRenderer>()->mesh, nullptr);
    EXPECT_FALSE(clone.GetComponent<scene::ProceduralMeshComponent>()->runtimeMesh.HasMesh());
    EXPECT_EQ(clone.GetComponent<scene::ProceduralMeshComponent>()->dirty, scene::MeshDirty::All);
    EXPECT_EQ(view.mesh, mesh.runtimeMesh.Current());
    object.RemoveComponent<scene::ProceduralMeshComponent>();
    EXPECT_EQ(view.mesh, nullptr);
    EXPECT_EQ(m_device.m_buffersAlive, 0u);
}

TEST_F(TransientGpuStateTest, ComponentOutlivingItsResourceManagerCannotReleaseAnotherOwnersResources)
{
    scene::AnimatorComponent component;
    component.skinningBuffer = Constant();
    m_resources.reset();
    renderer::ResourceManager next(m_device);
    const auto live = next.CreateConstantBuffer(16);
    component.ResetGpuRuntime();
    EXPECT_NE(next.Get(live), nullptr);
    next.Release(live);
}
} /// @note namespace fbzz::tests
