/// @file    WaterMediumShaderTests.cpp
/// @brief   Water medium lookup, dry-volume masks and RGB source transmission on DX12.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <TestKit/Graphics/ShaderPath.hpp>
#include <TestKit/TempDir.hpp>
#include <Core/ILogSink.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterExclusionComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterInteractionSystem.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Graphics/Effects/RenderWaterInput.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/Format.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Math/Vector4.hpp>
#include <Math/Quaternion.hpp>
#include <Physics/World.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace fbzz::tests {
namespace {

std::filesystem::path ResolveWaterMediumShaderPath(const std::filesystem::path& requested)
{
    return testkit::ResolveShaderPath(requested, FBZZ_GRAPHICS_SHADER_ROOT);
}

struct WaterMediumProbeConstants
{
    math::Vector4 pointMode{};
    math::Vector4 source{};
};
static_assert(sizeof(WaterMediumProbeConstants) == 32);
static_assert(sizeof(math::Vector4) == 16);

struct WaterMediumProbeLightConstants
{
    std::array<math::Vector4, 36> rows{};
};
static_assert(sizeof(WaterMediumProbeLightConstants) == 576);

struct WaterMediumProbeBody
{
    math::Vector4 boundsXZ{-10.0f, -10.0f, 10.0f, 10.0f};
    math::Vector3 absorption{0.2f, 0.1f, 0.05f};
    math::Vector3 scattering{0.1f, 0.2f, 0.3f};
    float phaseG = 0.0f;
    float lightStrength = 1.0f;
    float surfaceY = 0.0f;
    float bottomY = -5.0f;
    float depthLimit = 5.0f;
    float maxDistance = 120.0f;
    int priority = 0;
    uint64_t stableOwnerKey = 0;
    bool rectangular = true;
    bool variableDepth = false;
    float packedDepthHalfStep = 0.0f;
    float certifiedWaveDisplacement = -1.0f;
    std::vector<std::array<math::Vector4, 4>> exclusions;
    std::vector<std::array<math::Vector4, 2>> plumes;
};

class WaterMediumShaderTest : public testkit::Fixture, private core::ILogSink
{
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        renderer::SetShaderPathResolver(ResolveWaterMediumShaderPath);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Water medium shader test", WS_POPUP,
            0, 0, WINDOW_EXTENT, WINDOW_EXTENT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, WINDOW_EXTENT, WINDOW_EXTENT);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT).parent_path().parent_path();
        m_probeShader = m_resources->LoadShader((root / "Assets/Shaders/Rendering/WaterMediumProbe.hlsl").generic_string());
        m_probeState = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_probeConstants = m_resources->CreateConstantBuffer(sizeof(WaterMediumProbeConstants));
        m_lightConstants = m_resources->CreateConstantBuffer(sizeof(WaterMediumProbeLightConstants));
        m_lightData.rows[1] = {1.0f, 1.0f, 1.0f, 1.0f};
        m_lightData.rows[35] = {0.2f, 0.3f, 0.4f, 0.0f};
        m_atlasTarget = m_resources->CreateRenderTarget(ATLAS_WIDTH, 1,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, false});
        m_probeTarget = m_resources->CreateRenderTarget(1, 1,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA32F, false});
        const uint8_t white[] = {255, 255, 255, 255};
        m_placeholderAtlas = m_resources->CreateTexture(white, 1, 1);
        ASSERT_TRUE(m_probeShader.IsValid() && m_probeState.IsValid() && m_probeConstants.IsValid());
        ASSERT_TRUE(m_lightConstants.IsValid());
        ASSERT_TRUE(m_atlasTarget.IsValid() && m_probeTarget.IsValid() && m_placeholderAtlas.IsValid());
    }

    void TearDown() override
    {
        if (m_frameOpen && m_bundle.renderer)
            m_bundle.renderer->EndFrame();
        if (m_assetManagerInitialized) {
            asset::AssetManager::UnloadAll();
            m_assetManagerInitialized = false;
        }
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer)
            m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        if (m_window)
            DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult))
            CoUninitialize();
        core::Logger::RemoveSink(this);
        testkit::Fixture::TearDown();
    }

    void SetMedium(const std::vector<WaterMediumProbeBody>& inputBodies, float version = 1.0f)
    {
        ASSERT_FALSE(inputBodies.empty());
        std::vector<WaterMediumProbeBody> bodies = inputBodies;
        std::stable_sort(bodies.begin(), bodies.end(), [](const auto& a, const auto& b) {
            if (a.priority != b.priority)
                return a.priority > b.priority;
            return a.stableOwnerKey < b.stableOwnerKey;
        });
        m_bodyCount = static_cast<uint32_t>(bodies.size());
        std::vector<math::Vector4> exclusions;
        std::vector<uint32_t> exclusionStarts;
        std::vector<math::Vector4> plumes;
        std::vector<uint32_t> plumeStarts;
        exclusionStarts.reserve(bodies.size());
        for (const auto& body : bodies) {
            exclusionStarts.push_back(static_cast<uint32_t>(exclusions.size() / 4u));
            for (const auto& box : body.exclusions)
                exclusions.insert(exclusions.end(), box.begin(), box.end());
            plumeStarts.push_back(static_cast<uint32_t>(plumes.size() / 2u));
            for (const auto& plume : body.plumes)
                plumes.insert(plumes.end(), plume.begin(), plume.end());
        }

        constexpr uint32_t HEADER_ROWS = 5;
        constexpr uint32_t BODY_ROWS = 12;
        constexpr uint32_t GRID_CELLS = 16u * 16u;
        const uint32_t exclusionStart = HEADER_ROWS + static_cast<uint32_t>(bodies.size()) * BODY_ROWS;
        const uint32_t plumeStart = exclusionStart + static_cast<uint32_t>(exclusions.size());
        const uint32_t gridStart = plumeStart + static_cast<uint32_t>(plumes.size());
        const uint32_t candidateStart = gridStart + GRID_CELLS;
        const uint32_t totalRows = candidateStart + static_cast<uint32_t>(bodies.size());
        std::vector<math::Vector4> rows(totalRows);

        float minX = bodies.front().boundsXZ.x;
        float minZ = bodies.front().boundsXZ.y;
        float maxX = bodies.front().boundsXZ.z;
        float maxZ = bodies.front().boundsXZ.w;
        for (const auto& body : bodies) {
            minX = (std::min)(minX, body.boundsXZ.x);
            minZ = (std::min)(minZ, body.boundsXZ.y);
            maxX = (std::max)(maxX, body.boundsXZ.z);
            maxZ = (std::max)(maxZ, body.boundsXZ.w);
        }
        rows[0] = {22349.0f, version, static_cast<float>(bodies.size()), static_cast<float>(totalRows)};
        rows[1] = {minX, minZ, maxX, maxZ};
        rows[2] = {16.0f, 16.0f, static_cast<float>(gridStart), 0.0f};
        rows[3] = {static_cast<float>(exclusions.size() / 4u), static_cast<float>(exclusionStart), static_cast<float>(plumeStart),
                   static_cast<float>(candidateStart)};
        rows[4] = {static_cast<float>(ATLAS_WIDTH), 1.0f, 1024.0f, static_cast<float>(bodies.size())};

        for (size_t index = 0; index < bodies.size(); ++index) {
            const auto& body = bodies[index];
            const size_t offset = HEADER_ROWS + index * BODY_ROWS;
            rows[offset + 0] = {(body.boundsXZ.z - body.boundsXZ.x) * 0.5f,
                (body.boundsXZ.w - body.boundsXZ.y) * 0.5f, body.variableDepth ? 1.0f : 0.0f,
                body.variableDepth ? -body.packedDepthHalfStep
                    : body.certifiedWaveDisplacement >= 0.0f ? body.certifiedWaveDisplacement + 1.0f : 0.0f};
            rows[offset + 1] = {1.0f, 0.0f, 0.0f,
                -(body.boundsXZ.x + body.boundsXZ.z) * 0.5f};
            rows[offset + 2] = {0.0f, 1.0f, 0.0f, 0.0f};
            rows[offset + 3] = {0.0f, 0.0f, 1.0f,
                -(body.boundsXZ.y + body.boundsXZ.w) * 0.5f};
            rows[offset + 4] = {body.absorption, body.phaseG};
            rows[offset + 5] = {body.scattering, body.lightStrength};
            rows[offset + 6] = {body.surfaceY, body.bottomY, body.depthLimit, body.maxDistance};
            rows[offset + 7] = {0.15f, 0.0f, 0.001f, 48.0f};
            const float tileMinX = bodies.size() == 1 ? 0.0f : static_cast<float>(index) / static_cast<float>(bodies.size());
            const float tileMaxX = bodies.size() == 1 ? 1.0f : static_cast<float>(index + 1u) / static_cast<float>(bodies.size());
            rows[offset + 8] = {tileMinX, 0.0f, tileMaxX, 1.0f};
            rows[offset + 9] = body.boundsXZ;
            rows[offset + 10] = {static_cast<float>(exclusionStarts[index]),
                static_cast<float>(body.exclusions.size()), static_cast<float>(plumeStarts[index]), static_cast<float>(body.plumes.size())};
            rows[offset + 11] = {static_cast<float>(body.priority), 1.0f, 0.0f, body.rectangular ? 1.0f : 0.0f};
        }
        std::copy(exclusions.begin(), exclusions.end(), rows.begin() + exclusionStart);
        std::copy(plumes.begin(), plumes.end(), rows.begin() + plumeStart);
        for (uint32_t cell = 0; cell < GRID_CELLS; ++cell)
            rows[gridStart + cell] = {0.0f, static_cast<float>(bodies.size()), 0.0f, 0.0f};
        for (size_t index = 0; index < bodies.size(); ++index)
            rows[candidateStart + index] = {static_cast<float>(index), 0.0f, 0.0f, 0.0f};

        if (m_mediumTable.IsValid())
            m_resources->Release(m_mediumTable);
        m_mediumTable = m_resources->CreateStructuredBuffer(rows.data(), totalRows, sizeof(math::Vector4));
        ASSERT_TRUE(m_mediumTable.IsValid());
    }

    void SetAtlas(float relativeSurfaceY, float depth, float jacobian, float coverage)
    {
        WaterMediumProbeConstants constants;
        constants.pointMode.x = static_cast<float>(m_bodyCount);
        constants.pointMode.w = 3.0f;
        constants.source = {relativeSurfaceY, depth, jacobian, coverage};
        Draw(m_atlasTarget, constants, m_placeholderAtlas);
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget({}, *m_resources);
        device.EndFrame();
        m_frameOpen = false;
        m_atlasTexture = m_resources->GetColorTexture(m_atlasTarget);
        ASSERT_TRUE(m_atlasTexture.IsValid());
    }

    void SetAtlasDepthRamp(float leftDepth, float rightDepth)
    {
        auto& device = *m_bundle.renderer;
        m_resources->AdvanceFrame();
        device.BeginFrame();
        m_frameOpen = true;
        device.SetRenderTarget(m_atlasTarget, *m_resources);
        device.Clear({0.0f, 0.0f, 0.0f, 0.0f});
        const std::array<float, 2> depths{leftDepth, rightDepth};
        for (uint32_t x = 0; x < static_cast<uint32_t>(depths.size()); ++x) {
            device.SetViewport(x, 0, 1, 1);
            WaterMediumProbeConstants constants;
            constants.pointMode = {static_cast<float>(m_bodyCount), 0.0f, 0.0f, 3.0f};
            constants.source = {0.0f, depths[x], 1.0f, 1.0f};
            SubmitProbe(constants, m_placeholderAtlas);
        }
        device.SetRenderTarget({}, *m_resources);
        device.EndFrame();
        m_frameOpen = false;
        m_atlasTexture = m_resources->GetColorTexture(m_atlasTarget);
        ASSERT_TRUE(m_atlasTexture.IsValid());
    }

    math::Vector4 Query(math::Vector3 point, float mode, math::Vector3 source = {}, float distance = 0.0f)
    {
        WaterMediumProbeConstants constants;
        constants.pointMode = {point, mode};
        constants.source = {source, distance};
        Draw(m_probeTarget, constants, m_atlasTexture);
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget({}, *m_resources);
        device.EndFrame();
        m_frameOpen = false;
        std::vector<float> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_probeTarget, *m_resources, pixels, width, height));
        EXPECT_EQ(width, 1u);
        EXPECT_EQ(height, 1u);
        if (pixels.size() < 4u)
            return {};
        return {pixels[0], pixels[1], pixels[2], pixels[3]};
    }

protected:
    void SubmitProbe(const WaterMediumProbeConstants& constants,
                     renderer::ResourceHandle<renderer::TextureTag> atlas)
    {
        auto& device = *m_bundle.renderer;
        m_resources->Update(m_probeConstants, &constants, sizeof(constants));
        m_resources->Update(m_lightConstants, &m_lightData, sizeof(m_lightData));
        renderer::DrawCall draw;
        draw.shader = m_probeShader;
        draw.pipelineState = m_probeState;
        draw.constantBuffers[13] = m_probeConstants;
        draw.constantBuffers[3] = m_lightConstants;
        draw.auxiliaryPsBuffers[0] = m_mediumTable;
        draw.textures[15] = atlas;
        draw.vertexCount = 3;
        device.Submit(draw, *m_resources);
    }

    void Draw(renderer::ResourceHandle<renderer::RenderTargetTag> target,
              const WaterMediumProbeConstants& constants,
              renderer::ResourceHandle<renderer::TextureTag> atlas)
    {
        auto& device = *m_bundle.renderer;
        m_resources->AdvanceFrame();
        device.BeginFrame();
        m_frameOpen = true;
        device.SetRenderTarget(target, *m_resources);
        device.Clear({0.0f, 0.0f, 0.0f, 0.0f});
        SubmitProbe(constants, atlas);
    }

    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]"))
            ADD_FAILURE() << entry.message;
    }

    static constexpr uint32_t WINDOW_EXTENT = 64;
    static constexpr uint32_t ATLAS_WIDTH = 2;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ResourceHandle<renderer::ShaderTag> m_probeShader;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_probeState;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_probeConstants;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_lightConstants;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_mediumTable;
    renderer::ResourceHandle<renderer::TextureTag> m_placeholderAtlas;
    renderer::ResourceHandle<renderer::TextureTag> m_atlasTexture;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_atlasTarget;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_probeTarget;
    WaterMediumProbeLightConstants m_lightData;
    uint32_t m_bodyCount = 0;
    testkit::TempDir m_temp{"water-medium-production"};
    bool m_assetManagerInitialized = false;
};

TEST_F(WaterMediumShaderTest, PointQueryAndRGBBeerShareThePackedMedium)
{
    WaterMediumProbeBody body;
    SetMedium({body});
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);

    const math::Vector4 sample = Query({0.0f, -2.0f, 0.0f}, 0.0f);
    EXPECT_NEAR(sample.x, 0.0f, 0.001f);
    EXPECT_NEAR(sample.y, 5.0f, 0.001f);
    EXPECT_NEAR(sample.z, 1.0f, 0.001f);
    EXPECT_NEAR(sample.w, 1.0f, 0.001f);

    const math::Vector4 beer = Query({-4.0f, -2.0f, 0.0f}, 1.0f, {1.0f, 0.0f, 0.0f}, 4.0f);
    EXPECT_NEAR(beer.x, std::exp(-1.2f), 0.015f);
    EXPECT_NEAR(beer.y, std::exp(-1.2f), 0.015f);
    EXPECT_NEAR(beer.z, std::exp(-1.4f), 0.015f);
    EXPECT_NEAR(beer.w, 1.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, ProductionSceneExtractionFeedsTheWaterAtlasAndMediumProbe)
{
    ASSERT_TRUE(m_temp.IsValid());
    asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
    m_assetManagerInitialized = true;

    asset::MaterialAssetAuthoring material;
    material.params["opticalMode"] = {1.0f};
    material.params["absorptionCoefficients"] = {0.28f, 0.11f, 0.055f};
    material.params["scatteringCoefficients"] = {0.02f, 0.06f, 0.095f};
    material.params["phaseG"] = {0.45f};
    material.params["underwaterLightStrength"] = {2.0f};
    const auto materialPath = m_temp.File("ExtractedWater.mat");
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(materialPath.generic_string(), material));

    scene::Scene source;
    auto& waterObject = source.CreateGameObject("Extracted Water");
    const scene::EntityID waterID = waterObject.GetID();
    auto& water = waterObject.AddComponent<scene::WaterComponent>();
    water.enableGerstnerWaves = false;
    water.resolutionX = water.resolutionZ = 4;
    water.chunkCount = 1;
    water.lodPatchResolution = 4;
    water.extentX = 40.0f;
    water.extentZ = 30.0f;
    water.buoyancyDepth = 24.0f;
    water.materialPath = materialPath.generic_string();
    water.interactionSettings.sedimentScattering = {0.31f, 0.19f, 0.08f};
    waterObject.transform.position = {3.0f, 2.0f, -4.0f};

    auto& exclusionObject = source.CreateGameObject("Extracted Dry Room");
    exclusionObject.transform.position = {9.0f, 0.0f, -4.0f};
    auto& exclusion = exclusionObject.AddComponent<scene::WaterExclusionComponent>();
    exclusion.size = {2.0f, 2.0f, 2.0f};
    scene::FlushWorldTransforms(source);

    scene::WaterInteractionImpulse sediment;
    sediment.kind = scene::WaterInteractionKind::SEDIMENT;
    sediment.worldPosition = {3.0f, -3.0f, -4.0f};
    sediment.radius = 0.75f;
    sediment.strength = 0.8f;
    sediment.lifetime = 6.0f;
    ASSERT_TRUE(scene::QueueWaterInteraction(source, waterID, sediment));
    physics::World world;
    scene::WaterInteractionSystem interactionSystem;
    ++Time::frameCount;
    SystemContext interactionContext{source, world, nullptr, nullptr, 0.1f, 0.1f, true, true};
    interactionSystem.Update(interactionContext);

    renderer::Camera camera;
    camera.m_position = {3.0f, 20.0f, -4.0f};
    camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    camera.m_projection = renderer::ProjectionMode::Orthographic;
    camera.m_orthoHeight = 64.0f;
    camera.m_aspect = 1.0f;
    camera.m_near = 0.1f;
    camera.m_far = 100.0f;
    renderer::RenderSettings settings;
    renderer::RenderPassHandles extractionHandles{};
    scene::RenderPassContext extractionContext(source, *m_bundle.renderer, *m_resources,
        camera, settings, {}, UINT32_MAX, extractionHandles);
    renderer::RenderScene extracted;
    scene::ExtractRenderWater(extractionContext, extracted);
    ASSERT_EQ(extracted.water.size(), 1u);
    const auto& extractedMedium = extracted.water.front().medium;
    ASSERT_TRUE(extractedMedium.opticalEnabled);
    ASSERT_EQ(extractedMedium.exclusions.size(), 1u);
    ASSERT_EQ(extractedMedium.plumes.size(), 1u);
    EXPECT_NEAR(extractedMedium.absorptionPhaseG.x, 0.28f, 1.0e-6f);
    EXPECT_NEAR(extractedMedium.absorptionPhaseG.y, 0.11f, 1.0e-6f);
    EXPECT_NEAR(extractedMedium.absorptionPhaseG.z, 0.055f, 1.0e-6f);
    EXPECT_NEAR(extractedMedium.scatteringLight.w, 2.0f, 1.0e-6f);

    renderer::RenderPassHandles mediumHandles{};
    renderer::RenderViewResources viewTargets;
    renderer::RenderPassContext mediumContext{{}, *m_bundle.renderer, *m_resources,
        camera, settings, {}, UINT32_MAX, mediumHandles};
    mediumContext.renderScene = std::make_shared<renderer::RenderScene>(std::move(extracted));
    mediumContext.width = mediumContext.outputWidth = WINDOW_EXTENT;
    mediumContext.height = mediumContext.outputHeight = WINDOW_EXTENT;
    mediumContext.frustumCullingEnabled = false;
    mediumContext.occlusionCullingEnabled = false;
    m_resources->AdvanceFrame();
    m_bundle.renderer->BeginFrame();
    m_frameOpen = true;
    const bool prepared = renderer::PrepareWaterMediumResources(mediumContext, viewTargets);
    EXPECT_TRUE(prepared);
    bool packed = false;
    if (prepared) {
        EXPECT_TRUE(mediumContext.waterMediumPassActive);
        EXPECT_EQ(mediumHandles.waterMediumBodyCount, 1u);
        EXPECT_EQ(mediumHandles.waterMediumBodies.size(), 1u);
        EXPECT_TRUE(mediumHandles.waterMediumTable.IsValid());
        packed = mediumHandles.waterMediumBodyCount == 1u
            && mediumHandles.waterMediumBodies.size() == 1u
            && mediumHandles.waterMediumTable.IsValid();
        if (packed) {
            mediumContext.resourceRegistry.BindTarget("WaterMediumAtlas", viewTargets.waterMediumAtlasRT);
            mediumContext.resourceRegistry.BindTarget("WaterInterface", viewTargets.waterInterfaceRT);
            mediumContext.resourceRegistry.BindTarget("HDR", m_probeTarget);
            renderer::WaterMediumAtlasPass atlasPass;
            auto passResources = mediumContext.Res();
            atlasPass.Execute(passResources, mediumContext);
        }
    }
    m_bundle.renderer->SetRenderTarget({}, *m_resources);
    m_bundle.renderer->EndFrame();
    m_frameOpen = false;
    std::vector<float> capturedAtlas;
    uint32_t capturedAtlasWidth = 0;
    uint32_t capturedAtlasHeight = 0;
    if (prepared && packed) {
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(
            viewTargets.waterMediumAtlasRT, *m_resources, capturedAtlas,
            capturedAtlasWidth, capturedAtlasHeight));
        EXPECT_EQ(capturedAtlasWidth, 1024u);
        EXPECT_EQ(capturedAtlasHeight, 1024u);
        size_t capturedOwnerPixels = 0;
        for (size_t pixel = 0; pixel + 3u < capturedAtlas.size(); pixel += 4u)
            capturedOwnerPixels += std::round(capturedAtlas[pixel + 3u]) == 1.0f;
        EXPECT_GT(capturedOwnerPixels, 1000u)
            << "The prepared owner's atlas tile was not rasterized.";
    }
    if (!prepared || !packed) {
        viewTargets.waterMediumAtlasRT.Release(*m_resources);
        viewTargets.waterInterfaceRT.Release(*m_resources);
        viewTargets.waterMediumTablePool.ReleaseAll(*m_resources);
        ADD_FAILURE();
        return;
    }

    m_mediumTable = mediumHandles.waterMediumTable;
    m_bodyCount = mediumHandles.waterMediumBodyCount;
    m_atlasTexture = m_resources->GetColorTexture(viewTargets.waterMediumAtlasRT);
    EXPECT_TRUE(m_atlasTexture.IsValid());
    if (!m_atlasTexture.IsValid()) {
        viewTargets.waterMediumAtlasRT.Release(*m_resources);
        viewTargets.waterInterfaceRT.Release(*m_resources);
        viewTargets.waterMediumTablePool.ReleaseAll(*m_resources);
        m_mediumTable = {};
        m_bodyCount = 0;
        return;
    }
    const math::Vector4 tableHeader = Query({3.0f, -3.0f, -4.0f}, 8.0f);
    EXPECT_NEAR(tableHeader.x, 1.0f, 0.001f);
    EXPECT_NEAR(tableHeader.y, 1.0f, 0.001f);
    EXPECT_GT(tableHeader.z, 5.0f);
    EXPECT_NEAR(tableHeader.w, 1024.0f, 0.001f);
    const math::Vector4 atlasSample = Query({3.0f, -3.0f, -4.0f}, 7.0f);
    EXPECT_NEAR(atlasSample.w, 1.0f, 0.001f);
    EXPECT_GT(atlasSample.y, 0.0f);
    const math::Vector4 wet = Query({3.0f, -3.0f, -4.0f}, 0.0f);
    EXPECT_NEAR(wet.w, 1.0f, 0.001f);
    EXPECT_GT(wet.y, 0.0f);
    EXPECT_GT(wet.z, 0.0f);
    const math::Vector4 dry = Query({9.0f, -1.0f, -4.0f}, 0.0f);
    EXPECT_NEAR(dry.w, 0.0f, 0.001f);
    const math::Vector4 plume = Query({3.0f, -3.0f, -4.0f}, 6.0f);
    EXPECT_GT(plume.x, 0.0f);
    EXPECT_GT(plume.y, 0.0f);
    EXPECT_GT(plume.z, 0.0f);
    EXPECT_NEAR(plume.w, 1.0f, 0.001f);
    const math::Vector4 sourceTransmission = Query({3.0f, -3.0f, -4.0f}, 1.0f, {0.0f, 1.0f, 0.0f}, 7.0f);
    EXPECT_NEAR(sourceTransmission.w, 1.0f, 0.001f);
    EXPECT_LT(sourceTransmission.x, sourceTransmission.y);
    EXPECT_LT(sourceTransmission.y, sourceTransmission.z);
    EXPECT_LT(sourceTransmission.z, 1.0f);

    viewTargets.waterMediumAtlasRT.Release(*m_resources);
    viewTargets.waterInterfaceRT.Release(*m_resources);
    viewTargets.waterMediumTablePool.ReleaseAll(*m_resources);
    m_mediumTable = {};
    m_atlasTexture = {};
    m_bodyCount = 0;
}

TEST_F(WaterMediumShaderTest, ProductionPbrReceiverAttenuatesIncomingLightButNotEmission)
{
    WaterMediumProbeBody body;
    SetMedium({body});
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);

    const math::Vector3 receiver{0.0f, -2.0f, 0.0f};
    const math::Vector4 underwater = Query(receiver, 4.0f);
    SetMedium({body}, 2.0f);
    const math::Vector4 noWaterData = Query(receiver, 4.0f);
    EXPECT_LT(underwater.x, noWaterData.x);
    EXPECT_LT(underwater.y, noWaterData.y);
    EXPECT_LT(underwater.z, noWaterData.z);

    SetMedium({body});
    const math::Vector3 emission{2.0f, 1.0f, 0.5f};
    const math::Vector4 lit = Query(receiver, 4.0f);
    const math::Vector4 litWithEmission = Query(receiver, 5.0f, emission);
    EXPECT_NEAR(litWithEmission.x - lit.x, emission.x, 0.001f);
    EXPECT_NEAR(litWithEmission.y - lit.y, emission.y, 0.001f);
    EXPECT_NEAR(litWithEmission.z - lit.z, emission.z, 0.001f);

    const math::Vector4 dryWithWaterData = Query({20.0f, -2.0f, 0.0f}, 4.0f);
    SetMedium({body}, 2.0f);
    const math::Vector4 dryWithoutWaterData = Query({20.0f, -2.0f, 0.0f}, 4.0f);
    EXPECT_NEAR(dryWithWaterData.x, dryWithoutWaterData.x, 0.001f);
    EXPECT_NEAR(dryWithWaterData.y, dryWithoutWaterData.y, 0.001f);
    EXPECT_NEAR(dryWithWaterData.z, dryWithoutWaterData.z, 0.001f);
}

TEST_F(WaterMediumShaderTest, TerrainReceiverBoundaryKeepsVolumeDepthDryBoxesAndUniformSplineExact)
{
    WaterMediumProbeBody body;
    body.variableDepth = true;
    body.packedDepthHalfStep = 0.002f;
    body.bottomY = -10;
    body.depthLimit = 10;
    SetMedium({body});
    SetAtlas(0, 4.999f, 1, 1);
    const math::Vector4 nearBottom = Query({0, -4.998f, 0}, 4);
    const math::Vector4 bottom = Query({0, -5, 0}, 4);
    EXPECT_VEC4_NEAR(bottom, nearBottom, 0.001f);
    EXPECT_NEAR(Query({0, -5, 0}, 0).w, 0, 0.001f);
    const math::Vector4 belowBottom = Query({0, -5.01f, 0}, 4);
    SetMedium({body}, 2);
    const math::Vector4 dry = Query({0, -5.01f, 0}, 4);
    EXPECT_VEC4_NEAR(belowBottom, dry, 0.001f);
    EXPECT_LT(bottom.x, dry.x);
    EXPECT_LT(bottom.y, dry.y);
    EXPECT_LT(bottom.z, dry.z);

    body.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 5},
        math::Vector4{0, 0, 1, 0}, math::Vector4{1, 0.01f, 1, 0}});
    SetMedium({body});
    EXPECT_VEC4_NEAR(Query({0, -5, 0}, 4), dry, 0.001f);
    body.exclusions.clear();
    body.depthLimit = 4.999f;
    SetMedium({body});
    EXPECT_VEC4_NEAR(Query({0, -5, 0}, 4), dry, 0.001f);
    body.depthLimit = 10;
    body.rectangular = false;
    SetMedium({body});
    EXPECT_VEC4_NEAR(Query({0, -5, 0}, 4), dry, 0.001f);
    body.rectangular = true;
    body.variableDepth = false;
    SetMedium({body});
    EXPECT_VEC4_NEAR(Query({0, -5, 0}, 4), dry, 0.001f);
}

TEST_F(WaterMediumShaderTest, HighestPriorityOverlapOwnsThePointSample)
{
    WaterMediumProbeBody low;
    low.priority = 1;
    WaterMediumProbeBody high;
    high.priority = 9;
    high.stableOwnerKey = 20;
    high.absorption = {0.8f, 0.7f, 0.6f};
    SetMedium({low, high});
    SetAtlas(0.25f, 4.0f, 0.4f, 1.0f);

    const math::Vector4 sample = Query({0.0f, -1.0f, 0.0f}, 0.0f);
    EXPECT_NEAR(sample.x, 0.25f, 0.001f);
    EXPECT_NEAR(sample.y, 4.0f, 0.001f);
    EXPECT_NEAR(sample.z, 0.4f, 0.001f);
    EXPECT_NEAR(sample.w, 1.0f, 0.001f);
    const math::Vector4 selectedAttributes = Query({0.0f, -1.0f, 0.0f}, 2.0f);
    EXPECT_NEAR(selectedAttributes.z, 0.8f, 0.001f);
    EXPECT_NEAR(selectedAttributes.w, 1.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, SourceBeerStopsAtTheSurfaceAndPhysicalBottom)
{
    WaterMediumProbeBody body;
    SetMedium({body});
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);

    const math::Vector4 upward = Query({0.0f, -2.0f, 0.0f}, 1.0f, {0.0f, 1.0f, 0.0f}, 4.0f);
    EXPECT_NEAR(upward.x, std::exp(-0.3f * 2.0f), 0.015f);
    EXPECT_NEAR(upward.y, std::exp(-0.3f * 2.0f), 0.015f);
    EXPECT_NEAR(upward.z, std::exp(-0.35f * 2.0f), 0.015f);

    const math::Vector4 downward = Query({0.0f, -2.0f, 0.0f}, 1.0f, {0.0f, -1.0f, 0.0f}, 4.0f);
    EXPECT_NEAR(downward.x, std::exp(-0.3f * 3.0f), 0.015f);
    EXPECT_NEAR(downward.y, std::exp(-0.3f * 3.0f), 0.015f);
    EXPECT_NEAR(downward.z, std::exp(-0.35f * 3.0f), 0.015f);
}

TEST_F(WaterMediumShaderTest, EqualPriorityUsesStableOwnerKeyOrdering)
{
    WaterMediumProbeBody later;
    later.priority = 4;
    later.stableOwnerKey = 80;
    later.absorption = {0.9f, 0.0f, 0.0f};
    WaterMediumProbeBody earlier;
    earlier.priority = 4;
    earlier.stableOwnerKey = 12;
    earlier.absorption = {0.3f, 0.0f, 0.0f};
    SetMedium({later, earlier});
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);

    const math::Vector4 sample = Query({0.0f, -1.0f, 0.0f}, 2.0f);
    EXPECT_NEAR(sample.z, 0.3f, 0.001f);
    EXPECT_NEAR(sample.w, 1.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, VerticalColumnRetainsLowerSurfacePeersAndExactDryIntervals)
{
    WaterMediumProbeBody ocean;
    ocean.stableOwnerKey = 20;
    ocean.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 1},
        math::Vector4{0, 0, 1, 0}, math::Vector4{2, 0.25f, 2, 0}});
    WaterMediumProbeBody lake;
    lake.stableOwnerKey = 10;
    lake.surfaceY = -1.0f;
    lake.bottomY = -5.0f;
    lake.depthLimit = 4.0f;
    lake.rectangular = false;
    lake.absorption = {0.8f, 0.7f, 0.6f};
    for (const int priority : {-1, 0}) {
        lake.priority = priority;
        SetMedium({ocean, lake});
        SetAtlas(0, 5, 1, 1);
        const auto beer = Query({0, -2, 0}, 1, {0, 1, 0}, 2);
        EXPECT_NEAR(beer.x, std::exp(-0.3f * 1.5f), 0.001f);
        EXPECT_NEAR(beer.y, std::exp(-0.3f * 1.5f), 0.001f);
        EXPECT_NEAR(beer.z, std::exp(-0.35f * 1.5f), 0.001f);
    }
}

TEST_F(WaterMediumShaderTest, VerticalColumnRetainsAHigherPriorityOwnerAboveTheReceiver)
{
    WaterMediumProbeBody ocean;
    WaterMediumProbeBody lake;
    lake.priority = 9;
    lake.surfaceY = 1;
    lake.bottomY = -1;
    lake.depthLimit = 2;
    lake.absorption = {0.8f, 0.7f, 0.6f};
    SetMedium({ocean, lake});
    SetAtlas(0, 5, 1, 1);
    const auto beer = Query({0, -2, 0}, 1, {0, 1, 0}, 3);
    EXPECT_NEAR(beer.x, std::exp(-0.3f - 0.9f * 2), 0.003f);
    EXPECT_NEAR(beer.y, std::exp(-0.3f - 0.9f * 2), 0.003f);
    EXPECT_NEAR(beer.z, std::exp(-0.35f - 0.9f * 2), 0.003f);
}

TEST_F(WaterMediumShaderTest, VerticalColumnRetainsLowerPriorityWaterBeyondTheInitialSurface)
{
    WaterMediumProbeBody ocean;
    WaterMediumProbeBody lake;
    lake.priority = -1;
    lake.surfaceY = 2;
    lake.bottomY = 1;
    lake.depthLimit = 1;
    lake.absorption = {0.8f, 0.7f, 0.6f};
    SetMedium({ocean, lake});
    SetAtlas(0, 5, 1, 1);
    const auto beer = Query({0, -2, 0}, 1, {0, 1, 0}, 4);
    EXPECT_NEAR(beer.x, std::exp(-0.3f * 2 - 0.9f), 0.003f);
    EXPECT_NEAR(beer.y, std::exp(-0.3f * 2 - 0.9f), 0.003f);
    EXPECT_NEAR(beer.z, std::exp(-0.35f * 2 - 0.9f), 0.003f);
}

TEST_F(WaterMediumShaderTest, ThreeDimensionalExclusionDriesItsBoxButPreservesWaterBelowIt)
{
    WaterMediumProbeBody body;
    body.exclusions.push_back({
        math::Vector4{1.0f, 0.0f, 0.0f, 0.0f},
        math::Vector4{0.0f, 1.0f, 0.0f, 0.5f},
        math::Vector4{0.0f, 0.0f, 1.0f, 0.0f},
        math::Vector4{1.0f, 0.5f, 1.0f, 0.0f}});
    SetMedium({body});
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);

    EXPECT_NEAR(Query({0.0f, 0.0f, 0.0f}, 0.0f).w, 0.0f, 0.001f);
    EXPECT_NEAR(Query({0.0f, -0.5f, 0.0f}, 0.0f).w, 0.0f, 0.001f);
    EXPECT_NEAR(Query({0.0f, -2.0f, 0.0f}, 0.0f).w, 1.0f, 0.001f);
    EXPECT_NEAR(Query({2.0f, -0.5f, 0.0f}, 0.0f).w, 1.0f, 0.001f);
    EXPECT_NEAR(Query({0.0f, 0.2f, 0.0f}, 0.0f).w, 0.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, UpdatedWaveAtlasValuesDriveSurfaceDepthAndCompression)
{
    WaterMediumProbeBody body;
    SetMedium({body});
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);
    const math::Vector4 staticSample = Query({0.0f, -1.0f, 0.0f}, 0.0f);
    EXPECT_NEAR(staticSample.x, 0.0f, 0.001f);
    EXPECT_NEAR(staticSample.z, 1.0f, 0.001f);
    EXPECT_EQ(Query({0.0f, -1.0f, 0.0f}, 0.0f), staticSample);

    SetAtlas(0.25f, 4.0f, 0.0f, 1.0f);
    const math::Vector4 first = Query({0.0f, -1.0f, 0.0f}, 2.0f);
    EXPECT_NEAR(first.x, 1.0f, 0.001f);
    EXPECT_NEAR(first.y, 0.0f, 0.001f);
    EXPECT_NEAR(first.w, 1.0f, 0.001f);
    EXPECT_NEAR(Query({0.0f, 0.4f, 0.0f}, 0.0f).w, 0.0f, 0.001f);
    EXPECT_EQ(Query({0.0f, -1.0f, 0.0f}, 2.0f), first);

    SetAtlas(0.5f, 3.0f, 0.8f, 1.0f);
    const math::Vector4 second = Query({0.0f, -1.0f, 0.0f}, 2.0f);
    EXPECT_NEAR(second.x, 0.2f, 0.001f);
    EXPECT_NEAR(second.y, 0.8f, 0.001f);
    EXPECT_NEAR(second.w, 1.0f, 0.001f);
    EXPECT_NE(second.x, first.x);
    EXPECT_NEAR(Query({0.0f, 0.4f, 0.0f}, 0.0f).w, 1.0f, 0.001f);
    EXPECT_NEAR(Query({0.0f, -4.1f, 0.0f}, 0.0f).w, 0.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, DryExclusionIntervalsDoNotAccumulateWaterSourceBeer)
{
    WaterMediumProbeBody body;
    body.exclusions.push_back({
        math::Vector4{1.0f, 0.0f, 0.0f, 0.0f},
        math::Vector4{0.0f, 1.0f, 0.0f, 2.0f},
        math::Vector4{0.0f, 0.0f, 1.0f, 0.0f},
        math::Vector4{2.0f, 2.0f, 1.0f, 0.0f}});
    SetMedium({body});
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);

    const math::Vector4 beer = Query({-6.0f, -2.0f, 0.0f}, 1.0f, {1.0f, 0.0f, 0.0f}, 12.0f);
    EXPECT_NEAR(beer.x, std::exp(-0.3f * 8.0f), 0.02f);
    EXPECT_NEAR(beer.y, std::exp(-0.3f * 8.0f), 0.02f);
    EXPECT_NEAR(beer.z, std::exp(-0.35f * 8.0f), 0.02f);
    EXPECT_NEAR(beer.w, 1.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, CertifiedDeepSourceKeepsExactDryIntervals)
{
    WaterMediumProbeBody body;
    body.certifiedWaveDisplacement = 0.5f;
    body.exclusions.push_back({math::Vector4{1,0,0,0}, math::Vector4{0,1,0,2},
        math::Vector4{0,0,1,0}, math::Vector4{2,2,1,0}});
    SetMedium({body}); SetAtlas(0, 5, 1, 1);
    const auto beer = Query({-6,-2,0}, 1, {1,0,0}, 12);
    EXPECT_NEAR(beer.x, std::exp(-0.3f * 8), 0.02f);
    EXPECT_NEAR(beer.z, std::exp(-0.35f * 8), 0.02f);
}

TEST_F(WaterMediumShaderTest, CertifiedSourceStillClipsTheSurfaceAndBottom)
{
    WaterMediumProbeBody body; body.certifiedWaveDisplacement = 0.5f;
    SetMedium({body}); SetAtlas(0, 5, 1, 1);
    EXPECT_NEAR(Query({0,-2,0}, 1, {0,1,0}, 8).x, std::exp(-0.3f * 2), 0.015f);
    EXPECT_NEAR(Query({0,-2,0}, 1, {0,-1,0}, 8).x, std::exp(-0.3f * 3), 0.015f);
}

TEST_F(WaterMediumShaderTest, CertifiedSourceRejectsVariableDepthProofs)
{
    WaterMediumProbeBody body; body.certifiedWaveDisplacement = 0.5f; body.variableDepth = true;
    SetMedium({body}); SetAtlas(0, 2, 1, 1);
    EXPECT_NEAR(Query({0,-1,0}, 1, {0,-1,0}, 8).x, std::exp(-0.3f), 0.02f);
}

TEST_F(WaterMediumShaderTest, UnsupportedTableVersionFailsClosed)
{
    WaterMediumProbeBody body;
    SetMedium({body}, 2.0f);
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);

    const math::Vector4 sample = Query({0.0f, -2.0f, 0.0f}, 0.0f);
    EXPECT_NEAR(sample.x, 0.0f, 0.001f);
    EXPECT_NEAR(sample.y, 0.0f, 0.001f);
    EXPECT_NEAR(sample.z, 0.0f, 0.001f);
    EXPECT_NEAR(sample.w, 0.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, PlumeScatteringHasFiniteSupportAndAddsRgbSourceExtinctionOnlyInsideWater)
{
    WaterMediumProbeBody body;
    body.plumes.push_back({math::Vector4{0, -2, 0, 2}, math::Vector4{0.4f, 0.2f, 0.1f, 0.5f}});
    SetMedium({body});
    SetAtlas(0, 5, 1, 1);
    const auto center = Query({0, -2, 0}, 6);
    EXPECT_NEAR(center.x, 0.2f, 0.001f);
    EXPECT_NEAR(center.y, 0.1f, 0.001f);
    EXPECT_NEAR(center.z, 0.05f, 0.001f);
    EXPECT_NEAR(center.w, 1, 0.001f);
    const auto halfway = Query({1, -2, 0}, 6);
    EXPECT_NEAR(halfway.x, center.x * 0.25f, 0.001f);
    const auto beyond = Query({3, -2, 0}, 6);
    EXPECT_NEAR(beyond.x, 0, 0.001f);
    EXPECT_NEAR(beyond.w, 1, 0.001f);
    EXPECT_NEAR(Query({20, -2, 0}, 6).w, 0, 0.001f);
    EXPECT_NEAR(Query({0, 1, 0}, 6).w, 0, 0.001f);
    EXPECT_NEAR(Query({0, -6, 0}, 6).w, 0, 0.001f);
    const auto withPlume = Query({-2, -2, 0}, 1, {1, 0, 0}, 4);
    body.plumes.clear();
    SetMedium({body});
    const auto withoutPlume = Query({-2, -2, 0}, 1, {1, 0, 0}, 4);
    EXPECT_LT(withPlume.x, withoutPlume.x);
    EXPECT_LT(withPlume.y, withoutPlume.y);
    EXPECT_LT(withPlume.z, withoutPlume.z);
}

TEST_F(WaterMediumShaderTest, DryExclusionSuppressesPlumeAtTheReceiverAndAlongItsSourcePath)
{
    WaterMediumProbeBody body;
    body.plumes.push_back({math::Vector4{0, -2, 0, 1}, math::Vector4{0.8f, 0.4f, 0.2f, 1}});
    body.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 2},
        math::Vector4{0, 0, 1, 0}, math::Vector4{1.5f, 1.5f, 1.5f, 0}});
    SetMedium({body});
    SetAtlas(0, 5, 1, 1);
    const auto dry = Query({0, -2, 0}, 6);
    EXPECT_NEAR(dry.x, 0, 0.001f);
    EXPECT_NEAR(dry.w, 0, 0.001f);
    const auto withPlume = Query({-4, -2, 0}, 1, {1, 0, 0}, 8);
    body.plumes.clear();
    SetMedium({body});
    const auto withoutPlume = Query({-4, -2, 0}, 1, {1, 0, 0}, 8);
    EXPECT_NEAR(withPlume.x, withoutPlume.x, 0.002f);
    EXPECT_NEAR(withPlume.y, withoutPlume.y, 0.002f);
    EXPECT_NEAR(withPlume.z, withoutPlume.z, 0.002f);
}

TEST_F(WaterMediumShaderTest, PartialDryBoxKeepsWetSubintervalsOfRectanglePlumeCells)
{
    WaterMediumProbeBody body;
    body.absorption = {0.0f, 0.0f, 0.0f};
    body.scattering = {0.0f, 0.0f, 0.0f};
    body.plumes.push_back({math::Vector4{0.0f, -2.0f, 0.0f, 2.0f},
        math::Vector4{1.0f, 1.0f, 1.0f, 0.5f}});
    body.exclusions.push_back({
        math::Vector4{1.0f, 0.0f, 0.0f, -0.5f},
        math::Vector4{0.0f, 1.0f, 0.0f, 2.0f},
        math::Vector4{0.0f, 0.0f, 1.0f, 0.0f},
        math::Vector4{0.1f, 1.0f, 1.0f, 0.0f}});
    SetMedium({body});
    SetAtlas(0.0f, 5.0f, 1.0f, 1.0f);

    const math::Vector4 source = Query({-4.0f, -2.0f, 0.0f}, 1.0f, {1.0f, 0.0f, 0.0f}, 8.0f);
    const float expected = std::exp(-0.57775f);
    EXPECT_NEAR(source.x, expected, 0.02f);
    EXPECT_NEAR(source.y, expected, 0.02f);
    EXPECT_NEAR(source.z, expected, 0.02f);
    EXPECT_NEAR(source.w, 1.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, TerrainDepthRectangleUsesSampledAtlasDepthForSourceBeer)
{
    WaterMediumProbeBody body;
    body.variableDepth = true;
    SetMedium({body});
    SetAtlasDepthRamp(5.0f, 1.0f);

    const math::Vector4 variableDepth = Query({-8.0f, -2.0f, 0.0f}, 1.0f, {1.0f, 0.0f, 0.0f}, 16.0f);
    body.variableDepth = false;
    SetMedium({body});
    const math::Vector4 uniformDepth = Query({-8.0f, -2.0f, 0.0f}, 1.0f, {1.0f, 0.0f, 0.0f}, 16.0f);

    EXPECT_NEAR(variableDepth.x, std::exp(-0.3f * 10.5f), 0.015f);
    EXPECT_NEAR(variableDepth.z, std::exp(-0.35f * 10.5f), 0.015f);
    EXPECT_NEAR(uniformDepth.x, std::exp(-0.3f * 16.0f), 0.015f);
    EXPECT_GT(variableDepth.x, uniformDepth.x);
    EXPECT_NEAR(variableDepth.y, variableDepth.x, 0.01f);
    EXPECT_LT(variableDepth.z, variableDepth.x);
    EXPECT_NEAR(variableDepth.w, 1.0f, 0.001f);
}

TEST_F(WaterMediumShaderTest, NonanalyticShallowSourceClipsTheWetIntervalBeforeBoundedSampling)
{
    WaterMediumProbeBody body;
    body.rectangular = false;
    SetMedium({body});
    SetAtlas(0, 5, 1, 1);
    const auto source = Query({0, -2, 0}, 1, {0, 1, 0}, 120);
    EXPECT_NEAR(source.x, std::exp(-0.3f * 2), 0.015f);
    EXPECT_NEAR(source.y, std::exp(-0.3f * 2), 0.015f);
    EXPECT_NEAR(source.z, std::exp(-0.35f * 2), 0.015f);
}

TEST_F(WaterMediumShaderTest, MultipleBodyShallowSourceKeepsItsReceivingOwnerAndDryIntervals)
{
    WaterMediumProbeBody body;
    body.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, 1},
        math::Vector4{0, 0, 1, 0}, math::Vector4{1, 0.5f, 1, 0}});
    WaterMediumProbeBody separate;
    separate.boundsXZ = {20, 20, 30, 30};
    separate.absorption = {10, 10, 10};
    SetMedium({body, separate});
    SetAtlas(0, 5, 1, 1);
    const auto source = Query({0, -2, 0}, 1, {0, 1, 0}, 120);
    EXPECT_NEAR(source.x, std::exp(-0.3f), 0.015f);
    EXPECT_NEAR(source.y, std::exp(-0.3f), 0.015f);
    EXPECT_NEAR(source.z, std::exp(-0.35f), 0.015f);
}

TEST_F(WaterMediumShaderTest, VerticalSourceKeepsRaisedWaveColumnAndOverlappingDryIntervals)
{
    WaterMediumProbeBody body;
    body.rectangular = false;
    body.variableDepth = true;
    body.surfaceY = 10.0f;
    body.depthLimit = 8.0f;
    body.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, -8.5f},
        math::Vector4{0, 0, 1, 0}, math::Vector4{2, 0.5f, 2, 0}});
    body.exclusions.push_back({math::Vector4{1, 0, 0, 0}, math::Vector4{0, 1, 0, -9.0f},
        math::Vector4{0, 0, 1, 0}, math::Vector4{2, 0.5f, 2, 0}});
    SetMedium({body});
    SetAtlas(1.5f, 8.0f, 1.0f, 1.0f);

    const auto toSurface = Query({0, 7.5f, 0}, 1, {0, 1, 0}, 120);
    EXPECT_NEAR(toSurface.x, std::exp(-0.3f * 2.5f), 0.002f);
    EXPECT_NEAR(toSurface.y, std::exp(-0.3f * 2.5f), 0.002f);
    EXPECT_NEAR(toSurface.z, std::exp(-0.35f * 2.5f), 0.002f);

    const auto capped = Query({0, 7.5f, 0}, 1, {0, 1, 0}, 2);
    EXPECT_NEAR(capped.x, std::exp(-0.3f * 0.5f), 0.002f);
    EXPECT_NEAR(capped.y, std::exp(-0.3f * 0.5f), 0.002f);
    EXPECT_NEAR(capped.z, std::exp(-0.35f * 0.5f), 0.002f);
}

} /// @note namespace
} /// @note namespace fbzz::tests
