/// @file    ParticleGpuOwnershipTests.cpp
/// @brief   粒子の描画入力、GPU 資源の所有と返却を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <TestKit/Support/TempDir.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Particles/ParticleEmitter.hpp>
#include <Engine/Rendering/Extraction/Effects/RenderParticleExtractor.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include "../../../../../Engine/src/Rendering/Passes/Debug/SelectionPasses.hpp"
#include <array>
#include <cmath>

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

/// @brief クリア・再始動・容量変更で所有バッファを返却し、借用したプールのバッファを保つ。
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

/// @brief マネージャーの交互使用でエミッターのバッファを作り直し、所有者別の材質を保つ。
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

/// @brief リセット後にエミッターと材質の資源を再作成し、無関係なバッファは解放しない。
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

/// @brief 再読み込みしたシェーダーがパラメーターを持たない場合は古い材質バッファを返却する。
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

/// @brief 正の描画予算を CPU と GPU で共有し、予算が尽きても GPU の更新を一度だけ行う。
TEST_F(ParticleGpuOwnershipTest, DrawBudgetExhaustionDoesNotReopenBudgetOrSkipGpuSimulation)
{
    auto& resources = *m_resources;
    m_emitter->settings.maxParticles = 4;
    m_emitter->settings.renderPriority = 0;
    auto& cpuObject = m_scene.CreateGameObject("CPU");
    auto& gpuObject = m_scene.CreateGameObject("GPU");
    cpuObject.AddComponent<scene::ParticleEmitter>();
    gpuObject.AddComponent<scene::ParticleEmitter>();
    auto& cpu = *cpuObject.GetComponent<scene::ParticleEmitter>();
    auto& gpu = *gpuObject.GetComponent<scene::ParticleEmitter>();
    cpu.settings = gpu.settings = m_emitter->settings;
    cpu.settings.simulationMode = scene::ParticleSimulationMode::Cpu;
    cpu.settings.sortMode = scene::ParticleSortMode::None;
    cpu.settings.renderPriority = 1;
    gpu.settings.renderPriority = 2;
    cpu.runtime.particles.resize(3);

    renderer::Camera camera;
    renderer::RenderPassHandles handles;
    handles.particleShader = resources.LoadShader("Particle.hlsl");
    handles.particleGpuShader = resources.LoadShader("ParticleGpu.hlsl");
    handles.particleGpuSimCS = resources.LoadShader("ParticleSim.hlsl");
    handles.particleGpuPSO = handles.particleGpuAlphaPSO = handles.particleGpuPremultipliedPSO
        = resources.CreatePipelineState({});
    const uint32_t indices[]{0, 1, 2};
    handles.particleIB = resources.CreateIndexBuffer(indices, 3);
    ASSERT_TRUE(handles.particleShader && handles.particleGpuShader && handles.particleGpuSimCS && handles.particleIB);

    struct BudgetCase {
        bool enabled;
        int configured;
        int cpuDraw;
        int gpuDraw;
        int dropped;
    };
    for (const BudgetCase test : {BudgetCase{true, 4, 0, 0, 2}, BudgetCase{true, 6, 2, 0, 2}, BudgetCase{true, 9, 3, 2, 1},
                                 BudgetCase{true, 0, 3, 4, 0}, BudgetCase{true, -1, 3, 4, 0},
                                 BudgetCase{false, 1, 3, 4, 0}}) {
        SCOPED_TRACE(test.configured);
        ++Time::frameCount;
        renderer::RenderSettings settings;
        settings.particleBudgetEnabled = test.enabled;
        settings.particleBudget = test.configured;
        for (const bool firstView : {true, false}) {
            scene::RenderPassContext context(m_scene, m_device, resources, camera, settings, {}, UINT32_MAX, handles);
            auto frame = std::make_shared<renderer::RenderScene>();
            scene::ExtractRenderParticles(context, *frame);
            EXPECT_EQ(context.statsParticleEmitters, 3);
            EXPECT_EQ(context.statsParticleVisible, 4 + test.cpuDraw + test.gpuDraw);
            EXPECT_EQ(context.statsParticleBudgetDropped, test.dropped);
            EXPECT_EQ(m_emitter->runtime.visibleParticleCount, 4);
            EXPECT_EQ(cpu.runtime.visibleParticleCount, test.cpuDraw);
            EXPECT_EQ(gpu.runtime.visibleParticleCount, test.gpuDraw);
            EXPECT_EQ(cpu.runtime.particles.size(), 3u);
            EXPECT_EQ(gpu.runtime.lastGpuSimulationFrame, Time::frameCount);

            const renderer::RenderParticleInput* cpuDraw = nullptr;
            const renderer::RenderParticleInput* lastGpu = nullptr;
            for (const auto& particle : frame->particles) {
                if (particle.priority == 1) cpuDraw = &particle;
                if (particle.priority == 2) lastGpu = &particle;
            }
            if (test.cpuDraw == 0) {
                EXPECT_EQ(cpuDraw, nullptr);
            } else {
                ASSERT_NE(cpuDraw, nullptr);
                EXPECT_EQ(cpuDraw->drawCount, test.cpuDraw);
                EXPECT_EQ(cpuDraw->runtime.particles.size(), static_cast<std::size_t>(test.cpuDraw));
            }
            if (firstView || test.gpuDraw > 0) {
                ASSERT_NE(lastGpu, nullptr);
                EXPECT_TRUE(lastGpu->gpu);
                EXPECT_EQ(lastGpu->drawCount, test.gpuDraw);
                EXPECT_EQ(lastGpu->simulate, firstView);
            }
            if (!firstView && lastGpu) EXPECT_FALSE(lastGpu->simulate);

            context.renderScene = std::move(frame);
            std::vector<renderer::DrawCall> draws;
            m_device.m_drawCalls = &draws;
            const auto dispatches = m_device.m_dispatches;
            renderer::ExecuteParticlePass(context);
            m_device.m_drawCalls = nullptr;
            EXPECT_EQ(m_device.m_dispatches - dispatches, firstView ? 2u : 0u);
            std::vector<uint32_t> gpuVertexCounts;
            for (const auto& draw : draws)
                if (draw.vsBuffers[0].IsValid()) gpuVertexCounts.push_back(draw.vertexCount);
            const auto expectedGpuVertices = test.gpuDraw > 0
                ? std::vector<uint32_t>{24u, static_cast<uint32_t>(test.gpuDraw) * 6u} : std::vector<uint32_t>{24u};
            EXPECT_EQ(gpuVertexCounts, expectedGpuVertices);
        }
    }
    resources.Release(handles.particleIB);
}

/// @brief 通常描画と選択マスクが提出する CPU 粒子頂点と、履歴込みの描画上限を検証する。
TEST_F(ParticleGpuOwnershipTest, CpuBillboardDrawsPreserveQuadPayloadAndTrailBudget)
{
    const auto materialPath = m_emitter->settings.materialPath;
    m_emitter->settings.enabled = false;
    auto& object = m_scene.CreateGameObject("Billboard");
    object.transform.worldPosition = {10, 3, -2};
    object.transform.worldScale = {-2, 3, 0.5f};
    object.transform.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.5707963268f);
    auto& emitter = object.AddComponent<scene::ParticleEmitter>();
    emitter.settings.materialPath = materialPath;
    emitter.settings.simulationMode = scene::ParticleSimulationMode::Cpu;
    emitter.settings.culling.cullingEnabled = false;
    emitter.settings.trail.trailEnabled = true;
    emitter.settings.trail.trailPointCount = 2;
    emitter.settings.trail.trailWidthScale = 0.25f;
    emitter.settings.trail.trailAlphaScale = 0.4f;
    emitter.settings.trail.trailColorTint = {0.5f, 1, 0.25f, 0.5f};
    renderer::Particle particle{};
    particle.position = {1, 2, 3};
    particle.velocity = {2, 4, 6};
    particle.color = {0.5f, 0.25f, 0.75f, 0.8f};
    particle.size = 2;
    particle.rotation = 0.25f;
    particle.uvRect = {0, 0.25f, 0.25f, 0.5f};
    particle.nextUvRect = {0.25f, 0.5f, 0.5f, 0.75f};
    particle.spriteBlend = 0.375f;
    particle.trailCount = 2;
    particle.trailPoints[0] = {-2, 2, -3};
    particle.trailPoints[1] = {-3, 1, -4};

    auto& resources = *m_resources;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    settings.particleBudgetEnabled = false;
    settings.selectedObjects.push_back({object.GetID().index, object.GetID().generation});
    renderer::RenderPassHandles handles;
    handles.particleShader = resources.LoadShader("Particle.hlsl");
    handles.selectionMaskParticleShader = resources.LoadShader("ParticleSelection.hlsl");
    const auto pso = resources.CreatePipelineState({});
    handles.particlePSO = handles.particleAlphaPSO = handles.particlePremultipliedPSO = handles.selectionMaskPSO = pso;
    handles.objectCB = resources.CreateConstantBuffer(sizeof(scene::PerObjectCB));
    std::vector<uint32_t> indices;
    constexpr auto QUAD_LIMIT = static_cast<uint32_t>(renderer::kMaxParticleDraw);
    indices.reserve(QUAD_LIMIT * 6);
    for (uint32_t quad = 0; quad < QUAD_LIMIT; ++quad) {
        const uint32_t first = quad * 4;
        indices.insert(indices.end(), {first, first + 1, first + 2, first + 2, first + 1, first + 3});
    }
    handles.particleIB = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
    scene::RenderPassContext context(m_scene, m_device, resources, camera, settings, {}, UINT32_MAX, handles);
    context.selectionOutlineEnabled = true;
    const std::array<math::Vector2, 4> expectedUv{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};

    for (int passCase = 0; passCase < 3; ++passCase) {
        SCOPED_TRACE(passCase);
        const bool local = passCase == 1;
        const bool capped = passCase == 2;
        emitter.settings.simulationSpace = local ? scene::ParticleSimulationSpace::Local : scene::ParticleSimulationSpace::World;
        emitter.runtime.particles.assign(capped ? renderer::kMaxParticleDraw / 3 + 1 : 1, particle);
        const auto frame = std::make_shared<renderer::RenderScene>();
        scene::ExtractRenderParticles(context, *frame);
        ASSERT_EQ(frame->particles.size(), 1u);
        context.renderScene = frame;

        std::vector<renderer::DrawCall> draws;
        m_device.m_drawCalls = &draws;
        renderer::ExecuteParticlePass(context);
        auto passResources = context.Res();
        scene::ExecuteSelectionMaskPass(passResources, context);
        m_device.m_drawCalls = nullptr;
        ASSERT_EQ(draws.size(), 2u);
        EXPECT_NE(draws[0].vertexBuffer, draws[1].vertexBuffer);

        for (size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex) {
            SCOPED_TRACE(drawIndex);
            const auto& draw = draws[drawIndex];
            EXPECT_EQ(draw.indexCount, (capped ? QUAD_LIMIT : 3u) * 6u);
            const auto* buffer = resources.Get(draw.vertexBuffer);
            ASSERT_NE(buffer, nullptr);
            ASSERT_EQ(buffer->GetStride(), sizeof(renderer::ParticleVertex));
            std::array<renderer::ParticleVertex, 12> vertices{};
            ASSERT_TRUE(buffer->CopyData(0, sizeof(vertices), vertices.data()));
            const std::array<math::Vector3, 3> centers = local
                ? std::array<math::Vector3, 3>{{{11.5f, 9, 0}, {8.5f, 9, -6}, {8, 6, -8}}}
                : std::array<math::Vector3, 3>{{particle.position, particle.trailPoints[0], particle.trailPoints[1]}};
            for (size_t i = 0; i < vertices.size(); ++i) {
                const auto& vertex = vertices[i];
                EXPECT_VEC3_NEAR((math::Vector3{vertex.center[0], vertex.center[1], vertex.center[2]}),
                    centers[i / 4], testkit::kLooseTolerance);
                const math::Vector3 velocity = local ? math::Vector3{6, 4, -2} : particle.velocity;
                EXPECT_VEC3_NEAR((math::Vector3{vertex.velocity[0], vertex.velocity[1], vertex.velocity[2]}),
                    velocity, testkit::kLooseTolerance);
                EXPECT_FLOAT_EQ(vertex.uv[0], expectedUv[i % 4].x);
                EXPECT_FLOAT_EQ(vertex.uv[1], expectedUv[i % 4].y);
                EXPECT_FLOAT_EQ(vertex.uvRect[1], particle.uvRect.y);
                EXPECT_FLOAT_EQ(vertex.nextUvRect[1], particle.nextUvRect.y);
                EXPECT_FLOAT_EQ(vertex.spriteBlend, particle.spriteBlend);
                EXPECT_FLOAT_EQ(vertex.rotation, particle.rotation);
            }
            EXPECT_FLOAT_EQ(vertices[0].size, particle.size);
            EXPECT_FLOAT_EQ(vertices[4].size, 1.5f);
            EXPECT_FLOAT_EQ(vertices[8].size, 1.0f);
            EXPECT_FLOAT_EQ(vertices[0].color[3], 0.8f);
            EXPECT_FLOAT_EQ(vertices[4].color[3], 0.32f);
            EXPECT_FLOAT_EQ(vertices[8].color[3], 0.24f);
            /// @note 通常描画はシェーダーと共通のガンマ 2.2 で変換し、選択マスクは変換前の色を使う。
            const float authoredTint = emitter.settings.trail.trailColorTint.x;
            const float expectedTint = drawIndex == 0 ? std::pow(authoredTint, 2.2f) : authoredTint;
            EXPECT_NEAR(vertices[4].color[0], particle.color.x * expectedTint, testkit::kLooseTolerance);
        }
    }
}

TEST_F(ParticleGpuOwnershipTest, ReplayedParticleSnapshotSortsBillboardsAndRibbonsForCurrentCamera)
{
    auto& resources = *m_resources;
    renderer::RenderPassHandles handles;
    handles.particleShader = resources.LoadShader("Particle.hlsl");
    handles.trailShader = resources.LoadShader("ParticleTrail.hlsl");
    handles.particlePSO = handles.trailPSO = resources.CreatePipelineState({});
    constexpr uint32_t QUAD_INDICES[]{0, 1, 2, 2, 1, 3};
    handles.particleIB = resources.CreateIndexBuffer(QUAD_INDICES, 6);
    auto snapshot = std::make_shared<renderer::RenderScene>();
    auto& emitter = snapshot->particles.emplace_back();
    emitter.settings.sortMode = renderer::ParticleSortMode::BackToFront;
    emitter.settings.trail.trailEnabled = true;
    emitter.settings.trail.trailRibbon = true;
    emitter.drawCount = 1;
    constexpr uint8_t WHITE[]{255, 255, 255, 255};
    emitter.runtime.texture = resources.CreateTexture(WHITE, 1, 1);
    emitter.runtime.renderCB = resources.CreateConstantBuffer(sizeof(renderer::ParticleRenderCB));
    emitter.runtime.trailRibbonCB = resources.CreateConstantBuffer(sizeof(renderer::TrailCB));
    ASSERT_TRUE(emitter.runtime.texture.IsValid());
    for (float x : {-8.0f, 9.0f}) {
        auto& particle = emitter.runtime.particles.emplace_back();
        particle.position = {x, 0, 0};
        particle.size = 1;
        particle.color = {1, 1, 1, 1};
        particle.trailCount = 1;
        particle.trailPoints[0] = {x - 1, 0, 0};
    }
    renderer::RenderSettings settings;

    const auto checkView = [&](float cameraX, float expectedParticleX) {
        renderer::Camera camera;
        camera.m_position = {cameraX, 0, 5};
        scene::RenderPassContext context(m_scene, m_device, resources, camera, settings, {}, UINT32_MAX, handles);
        context.renderScene = snapshot;
        std::vector<renderer::DrawCall> draws;
        m_device.m_drawCalls = &draws;
        renderer::ExecuteParticlePass(context);
        m_device.m_drawCalls = nullptr;
        ASSERT_EQ(draws.size(), 2u);
        EXPECT_EQ(draws[0].indexCount, 6u);
        EXPECT_EQ(draws[1].vertexCount, 6u);
        const auto* billboardBuffer = resources.Get(draws[0].vertexBuffer);
        const auto* ribbonBuffer = resources.Get(draws[1].vertexBuffer);
        ASSERT_NE(billboardBuffer, nullptr);
        ASSERT_NE(ribbonBuffer, nullptr);
        renderer::ParticleVertex billboard{};
        renderer::TrailVertex ribbon{};
        ASSERT_TRUE(billboardBuffer->CopyData(0, sizeof(billboard), &billboard));
        ASSERT_TRUE(ribbonBuffer->CopyData(0, sizeof(ribbon), &ribbon));
        EXPECT_FLOAT_EQ(billboard.center[0], expectedParticleX);
        EXPECT_FLOAT_EQ(ribbon.position.x, expectedParticleX);
    };

    checkView(-20, 9);
    checkView(20, -8);
    ASSERT_EQ(snapshot->particles.front().runtime.particles.size(), 2u);
    EXPECT_FLOAT_EQ(snapshot->particles.front().runtime.particles[0].position.x, -8);
    EXPECT_FLOAT_EQ(snapshot->particles.front().runtime.particles[1].position.x, 9);
}
}
