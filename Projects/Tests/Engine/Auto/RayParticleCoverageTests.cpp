/// @file    RayParticleCoverageTests.cpp
/// @brief   Camera-independent particle and Fiber coverage keeps unsupported effect owners without changing Raster draws.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Systems/RenderFiberExtractor.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/FiberGeometry.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/RayTracing/RayScene.hpp>

namespace fbzz::tests {
namespace {
class RayParticleCoverageTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::EntityID AddEmitter(bool gpu = false)
    {
        auto& go = m_scene.CreateGameObject("Offscreen particles");
        go.layer = 7;
        go.transform.position = go.transform.worldPosition = {10000, 0, 0};
        auto& emitter = go.AddComponent<scene::ParticleEmitter>();
        emitter.settings.light.lightEnabled = false;
        emitter.runtime.isCulledThisFrame = true;
        emitter.runtime.visibleParticleCount = 0;
        if (gpu) {
            emitter.settings.simulationMode = scene::ParticleSimulationMode::Gpu;
            emitter.settings.simulationSpace = scene::ParticleSimulationSpace::World;
            emitter.settings.collisionMode = scene::ParticleCollisionMode::None;
            emitter.settings.prewarm = false;
            emitter.settings.trail.trailEnabled = false;
            emitter.settings.maxParticles = 4;
        } else emitter.runtime.particles.resize(1);
        return go.GetID();
    }
    renderer::RenderScene Snapshot()
    {
        renderer::RenderScene snapshot;
        snapshot.sceneGeneration = m_scene.GetRenderSceneGeneration();
        scene::ExtractRayParticleSources(m_scene, snapshot);
        return snapshot;
    }
};

TEST_F(RayParticleCoverageTest, OffscreenNonLightParticlesKeepTheirOwnerAndDoNotMutateRasterState)
{
    const auto id = AddEmitter();
    const auto* emitter = m_scene.GetComponent<scene::ParticleEmitter>(id);
    const auto simulationFrame = emitter->runtime.lastCpuSimulationFrame;
    const auto snapshot = Snapshot();
    ASSERT_EQ(snapshot.rayUnsupportedEffects.size(), 1u);
    ASSERT_TRUE(snapshot.particles.empty());
    const auto& source = snapshot.rayUnsupportedEffects[0];
    EXPECT_EQ(source.sourceIndex, id.index);
    EXPECT_EQ(source.sourceGeneration, id.generation);
    EXPECT_EQ(source.layerMask, 1u << 7);
    EXPECT_EQ(source.kind, renderer::RenderRayUnsupportedEffect::PARTICLE);
    EXPECT_TRUE(emitter->runtime.isCulledThisFrame);
    EXPECT_EQ(emitter->runtime.visibleParticleCount, 0);
    EXPECT_EQ(emitter->runtime.particles.size(), 1u);
    EXPECT_EQ(emitter->runtime.lastCpuSimulationFrame, simulationFrame);
    const auto rayScene = renderer::BuildRayScene(snapshot, {1u << 7});
    ASSERT_EQ(rayScene.diagnostics.size(), 1u);
    EXPECT_EQ(rayScene.diagnostics[0].objectId,
        (renderer::RayObjectId{snapshot.sceneGeneration, id.index, id.generation}));
    EXPECT_TRUE(renderer::BuildRayScene(snapshot, {1u << 1}).diagnostics.empty());
}

TEST_F(RayParticleCoverageTest, InactiveDisabledEmptyAndNonDrawnCpuMeshSourcesAreExcluded)
{
    const auto id = AddEmitter();
    auto* emitter = m_scene.GetComponent<scene::ParticleEmitter>(id);
    emitter->settings.enabled = false;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    emitter->settings.enabled = true;
    auto& parent = m_scene.CreateGameObject("Inactive parent");
    m_scene.GetGameObject(id)->SetParent(parent);
    parent.SetActive(false);
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    parent.SetActive(true);
    emitter->runtime.particles.clear();
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    emitter->runtime.particles.resize(1);
    emitter->settings.meshParticlePath = "mesh.fbx";
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    emitter->settings.meshParticlePath.clear();
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
}

TEST_F(RayParticleCoverageTest, PositiveGpuDrawCapacityCannotProveZeroAliveParticlesButZeroCapacityCan)
{
    const auto id = AddEmitter(true);
    auto* emitter = m_scene.GetComponent<scene::ParticleEmitter>(id);
    ASSERT_TRUE(emitter->runtime.particles.empty());
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
    emitter->settings.maxParticles = 0;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    emitter->settings.maxParticles = -1;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
}

class CoverageCpuTexture final : public renderer::ITexture {
public:
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
};

class CoverageCpuRenderer final : public renderer::IRenderer {
public:
    uint32_t bufferCreations = 0;
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { ++bufferCreations; return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { ++bufferCreations; return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return std::make_unique<CoverageCpuTexture>(); }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { ++bufferCreations; return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { ++bufferCreations; return {}; }
};

class RayFiberCoverageTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"RayFiberCoverage"};
    CoverageCpuRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    scene::Scene m_scene;
    renderer::Mesh m_mesh;
    asset::Model m_model;
    std::string m_materialPath;
    bool m_assetInitialized = false;
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        asset::MaterialAsset material;
        material.params["fiberLength"] = {0.2f};
        material.params["fiberDensity"] = {1.0f};
        m_materialPath = m_temp.File("Fiber.mat").generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(m_materialPath, material));
        asset::AssetManager::Init(m_resources, m_temp.Path().generic_string() + "/");
        m_assetInitialized = true;
        m_mesh.vertexBuffer = {11, 2}; m_mesh.indexBuffer = {12, 3};
        m_mesh.vertexCount = m_mesh.indexCount = 3;
        m_mesh.cpuVertices.resize(3);
        m_mesh.cpuVertices[0].position = {0, 0, 0};
        m_mesh.cpuVertices[1].position = {1, 0, 0};
        m_mesh.cpuVertices[2].position = {0, 0, 1};
        for (auto& vertex : m_mesh.cpuVertices) vertex.normal = {0, 1, 0};
        m_mesh.cpuIndices = {0, 2, 1};
    }
    void TearDown() override
    {
        m_scene.Clear();
        if (m_assetInitialized) asset::AssetManager::UnloadAll();
        testkit::EngineFixture::TearDown();
    }
    scene::EntityID AddFiber()
    {
        auto& go = m_scene.CreateGameObject("Offscreen Fiber");
        go.layer = 7;
        go.transform.position = go.transform.worldPosition = {10000, 0, 0};
        auto& mesh = go.AddComponent<scene::MeshRenderer>();
        mesh.mesh = &m_mesh;
        mesh.lodVisible = false;
        auto& fiber = go.AddComponent<scene::FiberComponent>();
        fiber.m_materialPath = m_materialPath;
        return go.GetID();
    }
    renderer::RenderScene Snapshot(renderer::RenderScene output = {})
    {
        output.sceneGeneration = m_scene.GetRenderSceneGeneration();
        scene::ExtractRayFiberSources(m_scene, m_resources, output);
        return output;
    }
};

TEST_F(RayFiberCoverageTest, OffscreenRendererLodDoesNotDropFiberOwnerOrChangeRasterVisibility)
{
    const auto id = AddFiber();
    auto* mesh = m_scene.GetComponent<scene::MeshRenderer>(id);
    const auto snapshot = Snapshot();
    ASSERT_TRUE(snapshot.fibers.empty());
    ASSERT_EQ(snapshot.rayUnsupportedEffects.size(), 1u);
    EXPECT_EQ(snapshot.rayUnsupportedEffects[0].kind, renderer::RenderRayUnsupportedEffect::FIBER);
    EXPECT_EQ(snapshot.rayUnsupportedEffects[0].sourceIndex, id.index);
    EXPECT_EQ(snapshot.rayUnsupportedEffects[0].sourceGeneration, id.generation);
    EXPECT_EQ(snapshot.rayUnsupportedEffects[0].layerMask, 1u << 7);
    EXPECT_FALSE(mesh->lodVisible);
    EXPECT_EQ(m_scene.GetComponent<scene::FiberComponent>(id)->m_renderIdentity, 0u);
    EXPECT_EQ(m_renderer.bufferCreations, 0u);
    const auto rayScene = renderer::BuildRayScene(snapshot, {1u << 7});
    ASSERT_EQ(rayScene.diagnostics.size(), 1u);
    EXPECT_EQ(rayScene.diagnostics[0].objectId, (renderer::RayObjectId{snapshot.sceneGeneration, id.index, id.generation}));
    EXPECT_TRUE(renderer::BuildRayScene(snapshot, {1u << 1}).diagnostics.empty());
    mesh->lodVisible = true;
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
}

TEST_F(RayFiberCoverageTest, DisabledInactiveZeroDensityLengthAndRootCountAreExcluded)
{
    const auto id = AddFiber();
    auto* fiber = m_scene.GetComponent<scene::FiberComponent>(id);
    auto* mesh = m_scene.GetComponent<scene::MeshRenderer>(id);
    fiber->m_enabled = false;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    fiber->m_enabled = true;
    m_scene.GetGameObject(id)->SetActive(false);
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    m_scene.GetGameObject(id)->SetActive(true);
    mesh->enabled = false;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    mesh->enabled = true;
    fiber->m_densityScale = 0;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    fiber->m_densityScale = 1;
    fiber->m_lengthScale = 0;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    fiber->m_lengthScale = 1;
    m_mesh.indexCount = 0;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    m_mesh.indexCount = 3;
    fiber->m_shellCount = 0;
    /// @note Raster clamps Shell layers to at least one; raw zero is not proof of no surface.
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
    asset::MaterialAsset zeroDensity;
    zeroDensity.params["fiberDensity"] = {0};
    const auto emptyMaterial = m_temp.File("ZeroDensity.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(emptyMaterial, zeroDensity));
    fiber->m_materialPath = emptyMaterial;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    EXPECT_EQ(m_renderer.bufferCreations, 0u);
}

TEST_F(RayFiberCoverageTest, FinAndBladeCountProofMatchesEmptyNonmanifoldAndProducerBudgetsWithoutUploads)
{
    const auto id = AddFiber();
    auto* fiber = m_scene.GetComponent<scene::FiberComponent>(id);
    fiber->m_mode = scene::FiberRenderMode::BLADE;
    fiber->m_bladeDensity = 1;
    std::vector<renderer::FiberBladeRoot> roots;
    ASSERT_TRUE(renderer::BuildFiberBlades(m_mesh, fiber->m_bladeDensity, fiber->m_bladeWidth, 1, roots));
    ASSERT_TRUE(roots.empty());
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    fiber->m_bladeDensity = 2;
    ASSERT_TRUE(renderer::BuildFiberBlades(m_mesh, fiber->m_bladeDensity, fiber->m_bladeWidth, 1, roots));
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
    m_mesh.indexCount = 0;
    /// @note Expanded Blade/Fin draws consume CPU-generated geometry, not the root mesh's current GPU index count.
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
    fiber->m_bladeDensity = 65536;
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
    fiber->m_bladeDensity = 65538;
    EXPECT_FALSE(renderer::BuildFiberBlades(m_mesh, fiber->m_bladeDensity, fiber->m_bladeWidth, 1, roots));
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    fiber->m_mode = scene::FiberRenderMode::FIN;
    renderer::FiberFinMesh fins;
    ASSERT_TRUE(renderer::BuildFiberFins(m_mesh, fins));
    ASSERT_FALSE(fins.m_indices.empty());
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
    fiber->m_mode = scene::FiberRenderMode::HYBRID;
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
    fiber->m_mode = scene::FiberRenderMode::FIN;
    m_mesh.cpuIndices = {0, 2, 1, 0, 2, 1, 0, 2, 1};
    EXPECT_FALSE(renderer::BuildFiberFins(m_mesh, fins));
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    m_mesh.cpuIndices = {0, 2, 1};
    m_mesh.cpuVertices[2].position = m_mesh.cpuVertices[1].position;
    ASSERT_TRUE(renderer::BuildFiberFins(m_mesh, fins));
    ASSERT_TRUE(fins.m_indices.empty());
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    EXPECT_EQ(m_renderer.bufferCreations, 0u);
}

TEST_F(RayFiberCoverageTest, SkinnedShellKeepsLodHiddenSourcesButRespectsExplicitSlotVisibility)
{
    const auto id = AddFiber();
    m_scene.GetComponent<scene::MeshRenderer>(id)->enabled = false;
    m_model.meshes.push_back(std::make_unique<renderer::Mesh>(m_mesh));
    m_model.meshes[0]->isSkinned = true;
    auto* go = m_scene.GetGameObject(id);
    auto& skin = go->AddComponent<scene::SkinnedMeshRenderer>();
    skin.model = &m_model;
    skin.lodVisible = false;
    EXPECT_EQ(Snapshot().rayUnsupportedEffects.size(), 1u);
    auto& material = go->AddComponent<scene::MaterialComponent>();
    material.visible = false;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    material.visible = true;
    m_scene.GetComponent<scene::FiberComponent>(id)->m_mode = scene::FiberRenderMode::BLADE;
    EXPECT_TRUE(Snapshot().rayUnsupportedEffects.empty());
    EXPECT_FALSE(skin.lodVisible);
    EXPECT_EQ(m_renderer.bufferCreations, 0u);
}

TEST_F(RayFiberCoverageTest, TerrainUsesAlreadyResolvedSeededBladeCountsWithoutGeneratingAnotherShape)
{
    const auto id = AddFiber();
    auto* go = m_scene.GetGameObject(id);
    go->GetComponent<scene::MeshRenderer>()->enabled = false;
    auto& fiber = *go->GetComponent<scene::FiberComponent>();
    fiber.m_mode = scene::FiberRenderMode::BLADE;
    fiber.m_renderIdentity = 73;
    go->AddComponent<scene::TerrainComponent>();
    renderer::RenderScene input;
    input.fibers.push_back({});
    input.fibers[0].identity = 73;
    input.fibers[0].layer = 7;
    input.fibers[0].settings.m_mode = scene::FiberRenderMode::BLADE;
    input.fibers[0].blades.m_blades = {19, 4};
    const auto empty = Snapshot(input);
    EXPECT_TRUE(empty.rayUnsupportedEffects.empty());
    EXPECT_TRUE(renderer::BuildRayScene(empty).diagnostics.empty());
    input.fibers[0].blades.m_bladeCount = 1;
    const auto snapshot = Snapshot(input);
    ASSERT_EQ(snapshot.rayUnsupportedEffects.size(), 1u);
    EXPECT_EQ(snapshot.rayUnsupportedEffects[0].sourceIndex, id.index);
    EXPECT_EQ(snapshot.fibers[0].blades.m_bladeCount, 1u);
    EXPECT_EQ(m_renderer.bufferCreations, 0u);
}
} /// @note namespace
} /// @note namespace fbzz::tests
