/// @file    WaterRenderSyncTests.cpp
/// @brief   Compare moving CPU water queries with production vertex deformation and submitted Horizon meshes.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Graphics/ShaderPath.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/AssetPathService.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::tests {
namespace {
constexpr uint32_t EXTENT = 128;
constexpr size_t PROBES = 32;
constexpr std::array<float, 6> TIMES{0.0f, 0.125f, 0.75f, 1.7f, 4.2f, 12.5f};

std::filesystem::path ResolveWaterSyncShader(const std::filesystem::path& requested)
{
    return testkit::ResolveShaderPath(requested, FBZZ_GRAPHICS_SHADER_ROOT);
}

renderer::AssetPathService EngineAssetPaths()
{
    renderer::AssetPathService service{};
    service.resolveAssetPath = [](const std::string& path) { return asset::AssetManager::ResolveAssetPath(path); };
    service.resolveTextureSource = [](std::string_view path, std::string& out) {
        return asset::TexDescSerializer::ResolveSourcePath(path, out);
    };
    service.normalizeTextureKey = [](std::string_view reference, std::string& out) {
        std::string sprite;
        (void)asset::ParseSpriteReference(reference, out, sprite);
    };
    service.resolveShaderCapabilities = asset::ResolveShaderCapabilities;
    return service;
}

math::Vector3 WorldPoint(const scene::Transform& pose, const math::Vector3& local)
{
    return pose.worldPosition + pose.worldRotation * math::Vector3{
        local.x * pose.worldScale.x, local.y * pose.worldScale.y, local.z * pose.worldScale.z};
}

struct SyncErrors {
    float height = 0.0f;
    float normal = 0.0f;
    size_t samples = 0;
    void Add(float heightError, const math::Vector3& a, const math::Vector3& b)
    {
        height = (std::max)(height, std::abs(heightError));
        normal = (std::max)(normal, (a - b).Length());
        ++samples;
    }
};
} /// @note namespace

class WaterRenderSyncTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousTime = Time::time;
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Water render sync", WS_POPUP,
            0, 0, EXTENT, EXTENT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, EXTENT, EXTENT);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        renderer::SetShaderPathResolver(ResolveWaterSyncShader);
        auto service = EngineAssetPaths();
        service.resolveShaderCapabilities = [](std::string_view reference) {
            return asset::ResolveShaderCapabilities(ResolveWaterSyncShader(std::filesystem::path(reference)).generic_string());
        };
        renderer::SetAssetPathService(service);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        ASSERT_TRUE(m_temp.IsValid());
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        const auto copiedRoot = m_temp.File("Assets/Shaders");
        std::error_code error;
        std::filesystem::create_directories(copiedRoot, error);
        ASSERT_FALSE(error);
        /// @note Compile unchanged production includes in a temporary asset root; no original project scene or shader is edited.
        for (const auto* directory : {"Common", "Platform", "Rendering", "Water"}) {
            std::filesystem::copy(root / directory, copiedRoot / directory, std::filesystem::copy_options::recursive, error);
            ASSERT_FALSE(error);
        }
        const auto fixtures = root.parent_path().parent_path() / "Projects/Tests/Graphics/Fixtures";
        for (const auto* name : {"WaterSurfaceSyncProbe.hlsl", "WaterSurfaceSyncRaster.hlsl", "WaterSurfaceSyncRaster.hlsl.meta"}) {
            std::filesystem::copy_file(fixtures / name, copiedRoot / name, error);
            ASSERT_FALSE(error);
        }
        m_probeShader = m_resources->LoadShader((copiedRoot / "WaterSurfaceSyncProbe.hlsl").generic_string());
        m_rasterShader = m_resources->LoadShader((copiedRoot / "WaterSurfaceSyncRaster.hlsl").generic_string());
        m_probeTarget = m_resources->CreateRenderTarget(static_cast<uint32_t>(PROBES), 2, {1, renderer::Format::RGBA32F, false});
        m_rasterTarget = m_resources->CreateRenderTarget(EXTENT, EXTENT, {1, renderer::Format::RGBA32F, true, true});
        m_shadowTarget = m_resources->CreateRenderTarget(EXTENT, EXTENT, renderer::CameraDepthTargetDesc(0));
        m_state = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_frameCB = m_resources->CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        m_waterCB = m_resources->CreateConstantBuffer(sizeof(renderer::WaterCB));
        m_lightCB = m_resources->CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        m_shadowCB = m_resources->CreateConstantBuffer(sizeof(renderer::ShadowConstantsCB));
        m_advancedCB = m_resources->CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
        m_rasterOptionsCB = m_resources->CreateConstantBuffer(sizeof(math::Vector4));
        ASSERT_TRUE(m_probeShader && m_rasterShader && m_probeTarget && m_rasterTarget && m_shadowTarget
            && m_state && m_frameCB && m_waterCB && m_lightCB && m_shadowCB && m_advancedCB && m_rasterOptionsCB);
        ASSERT_TRUE(m_resources->GetShaderCapabilities(m_rasterShader).SupportsWaterGeometry());
    }

    void TearDown() override
    {
        Time::time = m_previousTime;
        m_resources.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.imguiRenderer.reset();
        m_bundle.renderer.reset();
        renderer::SetShaderPathResolver(nullptr);
        renderer::SetAssetPathService(EngineAssetPaths());
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        EngineFixture::TearDown();
    }

    scene::EntityID AddWater(scene::Scene& source, bool horizon, bool parented = false)
    {
        auto& object = source.CreateGameObject("Sync Water");
        const auto id = object.GetID();
        if (parented) {
            auto& parent = source.CreateGameObject("Sync Parent");
            parent.transform.position = {100.0f, 12.0f, -40.0f};
            parent.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(35.0f));
            parent.transform.scale = {-2.0f, 3.0f, 0.5f};
            source.GetGameObject(id)->SetParent(parent);
            source.GetGameObject(id)->transform.position = math::Vector3::ZERO;
            source.GetGameObject(id)->transform.rotation = math::Quaternion::Identity();
            source.GetGameObject(id)->transform.scale = math::Vector3::ONE;
        }
        auto& water = source.GetGameObject(id)->AddComponent<scene::WaterComponent>();
        water.extentX = water.extentZ = 24.0f;
        water.resolutionX = water.resolutionZ = 256;
        water.chunkCount = 1;
        water.cellSize = {0.09375f, 0.09375f};
        water.farOceanEnabled = horizon;
        water.adaptivePatchLod = horizon;
        water.lodPatchResolution = 16;
        water.lodMaxDepth = 10;
        water.lodMaxPatches = 1024;
        water.lodNearDistance = 32.0f;
        water.lodNearCellSize = 0.125f;
        water.lodTargetPixels = 2.0f;
        water.waveGrouping = 0.45f;
        water.waveSpread = 0.35f;
        water.waveCount = 6;
        for (size_t i = 0; i < water.waveCount; ++i) {
            auto& wave = water.waves[i];
            wave.direction = math::Vector2{1.0f, 0.2f + 0.27f * static_cast<float>(i)}.Normalized();
            wave.amplitude = i == 0 ? 0.28f : 0.1f / static_cast<float>(i);
            wave.wavelength = 7.0f + 1.7f * static_cast<float>(i);
            wave.steepness = 0.45f;
            wave.phaseOffsetRadians = 0.23f * static_cast<float>(i + 1);
        }
        scene::FlushWorldTransforms(source);
        scene::PrepareWaterSimulationSurfaces(source);
        return id;
    }

    renderer::RenderWaterInput Extract(scene::Scene& source, const renderer::Camera& camera, float time)
    {
        Time::time = time;
        scene::RenderPassContext context(source, *m_bundle.renderer, *m_resources, camera, m_settings,
            m_rasterTarget, UINT32_MAX, m_handles);
        renderer::RenderScene result;
        scene::ExtractRenderWater(context, result);
        EXPECT_EQ(result.water.size(), 1u);
        if (result.water.empty()) return {};
        EXPECT_FLOAT_EQ(result.water.front().constants.timeParams.w, time);
        return std::move(result.water.front());
    }

    std::vector<float> Probe(const renderer::RenderWaterInput& input,
        const std::array<math::Vector3, PROBES>& points)
    {
        std::vector<renderer::WaterVertex> vertices;
        vertices.reserve(PROBES * 12);
        for (size_t row = 0; row < 2; ++row) for (const auto& point : points) {
            renderer::WaterVertex vertex{};
            vertex.position = point;
            vertex.uv = {point.x / 24.0f + 0.5f, point.z / 24.0f + 0.5f};
            for (size_t corner = 0; corner < 6; ++corner) vertices.push_back(vertex);
        }
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        const auto buffer = resources.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(vertices.front()), sizeof(vertices.front()));
        EXPECT_TRUE(buffer.IsValid());
        resources.AdvanceFrame();
        device.BeginFrame();
        renderer::PerFrameCB frame{};
        frame.viewProjection = math::Matrix4::Identity();
        resources.Update(m_frameCB, &frame, sizeof(frame));
        resources.Update(m_waterCB, &input.constants, sizeof(input.constants));
        device.SetRenderTarget(m_probeTarget, resources);
        device.Clear({0, 0, 0, 0});
        renderer::DrawCall draw;
        draw.vertexBuffer = buffer;
        draw.vertexCount = static_cast<uint32_t>(vertices.size());
        draw.shader = m_probeShader;
        draw.pipelineState = m_state;
        draw.constantBuffers[0] = m_frameCB;
        draw.constantBuffers[1] = m_waterCB;
        draw.textures[3] = input.foam;
        draw.textures[8] = input.ripple;
        draw.textures[26] = input.velocityField;
        device.Submit(draw, resources);
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_probeTarget, resources, pixels, width, height));
        EXPECT_EQ(width, PROBES); EXPECT_EQ(height, 2u);
        resources.Release(buffer);
        return pixels;
    }

    std::vector<float> Raster(renderer::RenderWaterInput input, const renderer::Camera& camera, bool captureLod = false)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        input.shader = m_rasterShader;
        input.appearanceParameters = m_rasterOptionsCB;
        auto source = std::make_shared<renderer::RenderScene>();
        source->water.push_back(std::move(input));
        renderer::SizedRenderTarget sceneColor, sceneDepth;
        m_handles.lightCB = m_lightCB;
        m_handles.defaultPSO = m_state;
        m_handles.postprocPSO = m_state;
        m_handles.shadowCB = m_shadowCB;
        m_handles.advancedGraphicsCB = m_advancedCB;
        m_handles.waterSceneColorRT = &sceneColor;
        m_handles.waterSceneDepthRT = &sceneDepth;
        renderer::RenderPassContext context{{}, device, resources, camera, m_settings,
            m_rasterTarget, UINT32_MAX, m_handles};
        context.renderScene = source;
        context.width = context.height = context.outputWidth = context.outputHeight = EXTENT;
        context.frustumCullingEnabled = true;
        context.occlusionCullingEnabled = false;
        context.resourceRegistry.BindTarget("HDR", m_rasterTarget);
        context.resourceRegistry.BindTarget("ShadowMap", m_shadowTarget);
        const renderer::LightConstantsCB light{};
        const renderer::ShadowConstantsCB shadow{};
        const renderer::AdvancedGraphicsCB advanced{};
        const math::Vector4 rasterOptions{captureLod ? 1.0f : 0.0f, 0, 0, 0};
        resources.AdvanceFrame();
        device.BeginFrame();
        resources.Update(m_lightCB, &light, sizeof(light));
        resources.Update(m_shadowCB, &shadow, sizeof(shadow));
        resources.Update(m_advancedCB, &advanced, sizeof(advanced));
        resources.Update(m_rasterOptionsCB, &rasterOptions, sizeof(rasterOptions));
        device.SetRenderTarget(m_shadowTarget, resources); device.ClearDepth();
        device.SetRenderTarget(m_rasterTarget, resources);
        device.Clear({0, 0, 0, 0}); device.ClearDepth();
        renderer::WaterRenderPass pass;
        auto passResources = context.Res();
        pass.Execute(passResources, context);
        EXPECT_GT(context.statsDrawCalls, 0);
        device.SetRenderTarget({}, resources); device.EndFrame();
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_rasterTarget, resources, pixels, width, height));
        EXPECT_EQ(width, EXTENT); EXPECT_EQ(height, EXTENT);
        sceneColor.Release(resources); sceneDepth.Release(resources);
        m_handles.waterSceneColorRT = m_handles.waterSceneDepthRT = nullptr;
        return pixels;
    }

    renderer::Camera DownCamera(const math::Vector3& center, float extent = 6.0f)
    {
        renderer::Camera camera;
        camera.m_position = center + math::Vector3{0, 8, 0};
        camera.m_rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
        camera.m_projection = renderer::ProjectionMode::Orthographic;
        camera.m_orthoHeight = extent;
        camera.m_aspect = 1.0f; camera.m_near = 0.1f; camera.m_far = 64.0f;
        return camera;
    }

    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    testkit::TempDir m_temp{"water-render-sync"};
    renderer::RenderSettings m_settings;
    renderer::RenderPassHandles m_handles{};
    renderer::ResourceHandle<renderer::ShaderTag> m_probeShader, m_rasterShader;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_probeTarget, m_rasterTarget, m_shadowTarget;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_state;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_frameCB, m_waterCB, m_lightCB, m_shadowCB, m_advancedCB, m_rasterOptionsCB;
    HWND m_window = nullptr;
    HRESULT m_com = E_FAIL;
    float m_previousTime = 0.0f;
};

TEST_F(WaterRenderSyncTest, DynamicProductionVertexMatchesWorldQueriesAtMovingShipProbeLocations)
{
    SyncErrors errors;
    float maximumChange = 0.0f;
    float minimumFixedWorldTemporalRange = 10.0f;
    float maximumFixedWorldTemporalRange = 0.0f;
    float maximumSentinelDrift = 0.0f;
    size_t temporalSamples = 0;
    for (bool parented : {false, true}) for (bool horizon : {false, true}) {
        scene::Scene source;
        const auto id = AddWater(source, horizon, parented);
        auto& water = *source.GetComponent<scene::WaterComponent>(id);
        const auto& pose = source.GetGameObject(id)->transform;
        std::array<float, PROBES> previous{};
        std::array<math::Vector3, PROBES> fixedWorld{};
        std::array<float, PROBES> temporalMinimum{}, temporalMaximum{};
        for (size_t i = 0; i < PROBES; ++i) {
            const math::Vector3 local{static_cast<float>(i % 8) * 0.3f - 1.05f, 0,
                static_cast<float>(i / 8) * 0.3f - 0.45f + (horizon ? 80.0f : 0.0f)};
            fixedWorld[i] = WorldPoint(pose, local);
        }
        for (float time : TIMES) {
            std::array<math::Vector3, PROBES> points{};
            for (size_t i = 0; i < PROBES; ++i) {
                /// @note A moving four-probe hull trajectory is numeric input, not an execution of ShipCombat gameplay.
                const size_t hull = i / 4, arm = i % 4;
                points[i] = {static_cast<float>(hull % 4) - 1.5f + time * 0.12f,
                    0, static_cast<float>(hull / 4) - 0.5f + std::sin(time) * 0.3f};
                points[i].x += arm == 0 ? 0.3f : arm == 1 ? -0.3f : 0.0f;
                points[i].z += arm == 2 ? 0.3f : arm == 3 ? -0.3f : 0.0f;
                if (horizon) points[i].z += 80.0f;
            }
            const auto input = Extract(source, DownCamera(pose.worldPosition), time);
            const auto pixels = Probe(input, points);
            ASSERT_EQ(pixels.size(), PROBES * 2u * 4u);
            for (size_t i = 0; i < PROBES; ++i) {
                const size_t p = i * 4, n = (PROBES + i) * 4;
                for (size_t c = 0; c < 4; ++c) {
                    ASSERT_TRUE(std::isfinite(pixels[p + c])); ASSERT_TRUE(std::isfinite(pixels[n + c]));
                }
                ASSERT_NEAR(pixels[p + 3], 1.0f, 0.0001f);
                const math::Vector3 displayed{pixels[p], pose.worldPosition.y + pixels[p + 2], pixels[p + 1]};
                scene::WaterSurfaceSample sample;
                ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {}, displayed, time, sample,
                    scene::WaterSurfaceReadView::SIMULATION));
                EXPECT_TRUE(sample.height.insideSurface);
                const math::Vector3 gpuNormal{pixels[n], pixels[n + 1], pixels[n + 2]};
                errors.Add(pixels[p + 2] - sample.height.displacement, gpuNormal, sample.normal);
                const auto base = WorldPoint(pose, points[i]);
                const auto horizontal = water.HorizontalDisplacementAt({base.x, base.z}, time, scene::WaterSurfaceReadView::SIMULATION);
                EXPECT_NEAR(displayed.x, base.x + horizontal.x, 0.001f);
                EXPECT_NEAR(displayed.z, base.z + horizontal.y, 0.001f);
                if (time > 0.0f) maximumChange = (std::max)(maximumChange, std::abs(pixels[p + 2] - previous[i]));
                previous[i] = pixels[p + 2];
            }

            /// @note Hold displaced world XZ fixed. Only the production inverse map chooses which source vertex reaches it.
            std::array<math::Vector3, PROBES> temporalPoints{};
            for (size_t i = 0; i < PROBES; ++i) {
                const auto origin = water.SolveUndisplacedXZ(fixedWorld[i].x, fixedWorld[i].z, time,
                    scene::WaterSurfaceReadView::SIMULATION);
                const auto local = pose.worldRotation.Conjugate()
                    * (math::Vector3{origin.x, pose.worldPosition.y, origin.y} - pose.worldPosition);
                temporalPoints[i] = {local.x / pose.worldScale.x, 0, local.z / pose.worldScale.z};
            }
            const auto temporalPixels = Probe(input, temporalPoints);
            ASSERT_EQ(temporalPixels.size(), PROBES * 2u * 4u);
            for (size_t i = 0; i < PROBES; ++i) {
                const size_t p = i * 4;
                for (size_t c = 0; c < 4; ++c) ASSERT_TRUE(std::isfinite(temporalPixels[p + c]));
                ASSERT_NEAR(temporalPixels[p + 3], 1.0f, 0.0001f);
                const float driftX = std::abs(temporalPixels[p] - fixedWorld[i].x);
                const float driftZ = std::abs(temporalPixels[p + 1] - fixedWorld[i].z);
                maximumSentinelDrift = (std::max)({maximumSentinelDrift, driftX, driftZ});
                EXPECT_LE(driftX, 0.001f); EXPECT_LE(driftZ, 0.001f);
                scene::WaterSurfaceHeightSample sample;
                ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, fixedWorld[i], time, sample,
                    scene::WaterSurfaceReadView::SIMULATION));
                EXPECT_TRUE(sample.insideSurface);
                const float height = temporalPixels[p + 2];
                EXPECT_NEAR(height, sample.displacement, 0.003f);
                if (time == TIMES.front()) temporalMinimum[i] = temporalMaximum[i] = height;
                else {
                    temporalMinimum[i] = (std::min)(temporalMinimum[i], height);
                    temporalMaximum[i] = (std::max)(temporalMaximum[i], height);
                }
                ++temporalSamples;
            }
        }
        float configurationRange = 0.0f;
        for (size_t i = 0; i < PROBES; ++i)
            configurationRange = (std::max)(configurationRange, temporalMaximum[i] - temporalMinimum[i]);
        EXPECT_GT(configurationRange, 0.1f);
        minimumFixedWorldTemporalRange = (std::min)(minimumFixedWorldTemporalRange, configurationRange);
        maximumFixedWorldTemporalRange = (std::max)(maximumFixedWorldTemporalRange, configurationRange);
    }
    EXPECT_EQ(errors.samples, 768u);
    EXPECT_LE(errors.height, 0.003f);
    /// @note Production analytic normals intentionally omit the low-frequency group-envelope derivative.
    EXPECT_LE(errors.normal, 0.05f);
    EXPECT_GT(maximumChange, 0.1f);
    EXPECT_EQ(temporalSamples, 768u);
    EXPECT_GT(minimumFixedWorldTemporalRange, 0.1f);
    RecordProperty("vertexHeightMaxMetres", std::to_string(errors.height));
    RecordProperty("vertexNormalMaxVectorDistance", std::to_string(errors.normal));
    RecordProperty("sampleCount", std::to_string(errors.samples));
    RecordProperty("movingProbeMaxHeightChangeMetres", std::to_string(maximumChange));
    RecordProperty("fixedWorldTemporalSampleCount", std::to_string(temporalSamples));
    RecordProperty("fixedWorldTemporalMinConfigurationRangeMetres", std::to_string(minimumFixedWorldTemporalRange));
    RecordProperty("fixedWorldTemporalMaxHeightRangeMetres", std::to_string(maximumFixedWorldTemporalRange));
    RecordProperty("fixedWorldTemporalMaxXZDriftMetres", std::to_string(maximumSentinelDrift));
}

TEST_F(WaterRenderSyncTest, LocalFlowSwellMatchesProductionHeightAndNormal)
{
    scene::Scene source;
    const auto id = AddWater(source, true);
    auto& water = *source.GetComponent<scene::WaterComponent>(id);
    const auto& pose = source.GetGameObject(id)->transform;
    water.enableGerstnerWaves = false;
    scene::FlowFieldSettings settings;
    settings.radius = 18.0f;
    settings.direction = {0.6f, 0.0f, 0.8f};
    settings.waterSwellAmplitude = 0.8f;
    settings.waterSwellWavelength = 14.0f;
    std::array<math::Vector3, PROBES> points{};
    for (size_t i = 0; i < points.size(); ++i)
        points[i] = {static_cast<float>(i % 8) * 5.0f - 17.5f, 0,
                     80.0f + static_cast<float>(i / 8) * 7.0f - 10.5f};
    for (bool vortex : {false, true}) {
        SCOPED_TRACE(vortex);
        if (vortex) {
            settings.fieldType = scene::FlowFieldType::Vortex;
            settings.direction = -math::Vector3::UP;
            settings.waterSwellAmplitude = 6.0f;
        }
        scene::ResolveWaterSurfaceFlows(water, pose,
            {scene::ResolveFlowField(settings, {0, 0, 80}, pose.worldRotation)});
        ASSERT_EQ(water.surfaceFlowCount, 1);
        SyncErrors errors;
        float maximumHeight = 0.0f;
        for (float time : {0.0f, 0.7f}) {
            const auto input = Extract(source, DownCamera(pose.worldPosition), time);
            const auto pixels = Probe(input, points);
            ASSERT_EQ(pixels.size(), PROBES * 2u * 4u);
            for (size_t i = 0; i < points.size(); ++i) {
                const size_t p = i * 4, n = (PROBES + i) * 4;
                scene::WaterSurfaceSample cpu;
                ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {}, points[i], time, cpu,
                    scene::WaterSurfaceReadView::SIMULATION));
                ASSERT_NEAR(pixels[p + 3], 1.0f, 0.0001f);
                errors.Add(pixels[p + 2] - cpu.height.displacement,
                    {pixels[n], pixels[n + 1], pixels[n + 2]}, cpu.normal);
                maximumHeight = (std::max)(maximumHeight, std::abs(pixels[p + 2]));
            }
        }
        EXPECT_GT(maximumHeight, vortex ? 4.0f : 0.5f);
        EXPECT_LT(errors.height, 0.001f);
        EXPECT_LT(errors.normal, 0.004f);
    }
}

TEST_F(WaterRenderSyncTest, ProductionHorizonRasterTracksNearShipHeightAndReportsCoarseLodError)
{
    const std::array<const char*, 5> names{"finiteNear", "horizonNear", "horizonOffshore", "horizonCoarse", "horizonOffshoreLargerRippleExtent"};
    std::array<SyncErrors, 5> errors{};
    std::array<float, 5> maximumCells{};
    std::array<size_t, 5> horizonSamples{};
    for (size_t configuration = 0; configuration < names.size(); ++configuration) {
        const bool horizon = configuration != 0u;
        const bool coarse = configuration == 3u;
        scene::Scene source;
        const auto id = AddWater(source, horizon);
        auto& water = *source.GetComponent<scene::WaterComponent>(id);
        const auto& pose = source.GetGameObject(id)->transform;
        if (configuration == 4u) {
            /// @note Changing the ripple footprint must not move the offshore grid or its wave phase.
            water.extentX = water.extentZ = 192.0f;
            water.meshDirty = true;
            scene::PrepareWaterSimulationSurfaces(source);
        }
        water.lodNearCellSize = coarse ? 8.0f : 0.125f;
        water.lodTargetPixels = coarse ? 64.0f : 2.0f;
        for (float time : {0.0f, 1.7f, 4.2f}) {
            SCOPED_TRACE(std::string(names[configuration]) + " time=" + std::to_string(time));
            const math::Vector3 center{time * 0.12f, pose.worldPosition.y,
                (configuration >= 2u ? 80.0f : horizon ? 6.0f : 0.0f) + time * 0.2f};
            const auto camera = DownCamera(center);
            const auto input = Extract(source, camera, time);
            const auto pixels = Raster(input, camera);
            const auto lodPixels = Raster(input, camera, true);
            ASSERT_EQ(pixels.size(), static_cast<size_t>(EXTENT) * EXTENT * 4u);
            ASSERT_EQ(lodPixels.size(), pixels.size());
            for (uint32_t y : {40u, 56u, 72u, 88u}) for (uint32_t x : {40u, 56u, 72u, 88u}) {
                SCOPED_TRACE("pixel=" + std::to_string(x) + "," + std::to_string(y));
                const size_t offset = (static_cast<size_t>(y) * EXTENT + x) * 4u;
                ASSERT_NEAR(pixels[offset + 3], 1.0f, 0.001f);
                ASSERT_NEAR(lodPixels[offset + 3], 1.0f, 0.001f);
                for (size_t channel = 0; channel < 3; ++channel) ASSERT_TRUE(std::isfinite(lodPixels[offset + channel]));
                const bool actualHorizon = lodPixels[offset] > 0.5f;
                EXPECT_EQ(actualHorizon, horizon);
                horizonSamples[configuration] += actualHorizon ? 1u : 0u;
                const float cellX = lodPixels[offset + 1], cellZ = lodPixels[offset + 2];
                EXPECT_GT(cellX, 0.0f); EXPECT_GT(cellZ, 0.0f);
                maximumCells[configuration] = (std::max)({maximumCells[configuration], cellX, cellZ});
                if (!coarse) {
                    EXPECT_LE(cellX, 0.1251f); EXPECT_LE(cellZ, 0.1251f);
                }
                const float worldX = center.x + ((static_cast<float>(x) + 0.5f) / EXTENT - 0.5f) * camera.m_orthoHeight;
                const float worldZ = center.z - ((static_cast<float>(y) + 0.5f) / EXTENT - 0.5f) * camera.m_orthoHeight;
                scene::WaterSurfaceSample sample;
                ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {}, {worldX, center.y, worldZ},
                    time, sample, scene::WaterSurfaceReadView::SIMULATION));
                EXPECT_TRUE(sample.height.insideSurface);
                const float nx = pixels[offset + 1], nz = pixels[offset + 2];
                ASSERT_TRUE(std::isfinite(pixels[offset]) && std::isfinite(nx) && std::isfinite(nz));
                const math::Vector3 normal{nx, std::sqrt((std::max)(1.0f - nx * nx - nz * nz, 0.0f)), nz};
                errors[configuration].Add(pixels[offset] - sample.height.displacement, normal, sample.normal);
            }
        }
    }
    for (size_t configuration = 0; configuration < errors.size(); ++configuration) {
        SCOPED_TRACE(names[configuration]);
        const bool coarse = configuration == 3u;
        EXPECT_EQ(errors[configuration].samples, 48u);
        EXPECT_EQ(horizonSamples[configuration], configuration != 0u ? 48u : 0u);
        EXPECT_LE(errors[configuration].height, coarse ? 1.2f : 0.015f);
        EXPECT_LE(errors[configuration].normal, coarse ? 0.8f : 0.06f);
        RecordProperty(std::string(names[configuration]) + "HeightMaxMetres", std::to_string(errors[configuration].height));
        RecordProperty(std::string(names[configuration]) + "NormalMaxVectorDistance", std::to_string(errors[configuration].normal));
        RecordProperty(std::string(names[configuration]) + "CellMaxMetres", std::to_string(maximumCells[configuration]));
        RecordProperty(std::string(names[configuration]) + "HorizonSampleCount", std::to_string(horizonSamples[configuration]));
    }
    EXPECT_FLOAT_EQ(maximumCells[2], maximumCells[4]);
    EXPECT_NEAR(errors[2].height, errors[4].height, 0.001f);
    RecordProperty("scope", "Production WaterRenderPass raster readback of geometry height, normals and submitted cell size.");
}
} /// @note namespace fbzz::tests
