/// @file    RayGameTests.cpp
/// @brief   Game の輸送成分・短期再構成・再投影境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayGameResources.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Passes/RayTracing/RayPathTracePass.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {
class RayGameContractTest : public testkit::Fixture {};

renderer::RayReconstructionSurface Surface(uint32_t pixel)
{
    renderer::RayReconstructionSurface value;
    value.positionDepth = {(static_cast<float>(pixel) + 0.5f) / 2 - 1, 0, 2, 0.5f};
    value.normalRoughness = {0, 0, -1, 0.045f};
    value.albedoMetallic = {0.5f, 0.25f, 0.125f, 0};
    value.previousPositionValid = {value.positionDepth.x, 0, 2, 1};
    value.objectMaterialValid = {7, 9, 1, 1};
    value.sceneFlags = {42, 1, 0, 0};
    return value;
}

TEST_F(RayGameContractTest, CameraCutSeparatesOrdinaryMotionFromProjectionAndLargePoseChanges)
{
    renderer::Camera previous;
    previous.m_position = {};
    auto current = previous;
    current.m_position.x = 0.1f;
    EXPECT_FALSE(renderer::IsRayGameCameraCut(previous, current));
    current.m_position.x = 2;
    EXPECT_TRUE(renderer::IsRayGameCameraCut(previous, current));
    current = previous; current.m_fovY = 61;
    EXPECT_TRUE(renderer::IsRayGameCameraCut(previous, current));
    current = previous; current.LookAt({1, 0, 0});
    EXPECT_TRUE(renderer::IsRayGameCameraCut(previous, current));
}
TEST_F(RayGameContractTest, UsesFullIdentityMaterialAndCameraRelativeDepthForHistoryValidation)
{
    renderer::Camera camera;
    camera.m_position = {};
    const auto original = Surface(0);
    auto current = original;
    EXPECT_TRUE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current.objectMaterialValid[1]++;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current = original; current.objectMaterialValid[2]++;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current = original; current.sceneFlags[1]++;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current = original; current.previousPositionValid.w = 0;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current = original; current.previousPositionValid.x += 0.01f;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    auto translated = original;
    translated.positionDepth.z += 1000; translated.previousPositionValid.z += 1000;
    camera.m_position.z = 1000;
    current = translated; current.previousPositionValid.x += 0.01f;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, translated, camera));
}

class RayGameTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Ray Game test", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 32, 32);
        ASSERT_NE(m_bundle.renderer, nullptr);
        if (!m_bundle.renderer->GetCapabilities().inlineRayQuery) GTEST_SKIP() << "Inline RayQuery unavailable";
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        m_shader = m_resources->LoadShader((root / "RayTracing/RayGameReconstruction.cs.hlsl").generic_string());
        m_reader = m_resources->LoadShader((root / "../../Projects/Tests/Graphics/Shaders/RayGameRead.cs.hlsl").lexically_normal().generic_string());
        m_copy = m_resources->LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_output = m_resources->CreateComputeTexture(4, 1);
        m_target = m_resources->CreateRenderTarget(4, 1, {1, renderer::Format::RGBA16F, false});
        m_pipeline = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_cb = m_resources->CreateConstantBuffer(sizeof(renderer::RayReconstructionConstants));
        ASSERT_TRUE(m_shader && m_reader && m_copy && m_output && m_target && m_pipeline && m_cb);
        core::Logger::AddSink(this);
    }
    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        EXPECT_EQ(m_validationFailures, 0u);
        testkit::Fixture::TearDown();
    }
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.find("  [WARNING] (id=") == std::string::npos
            && entry.message.find("  [ERROR] (id=") == std::string::npos
            && entry.message.find("  [CORRUPTION] (id=") == std::string::npos) return;
        /// @note Existing PathColor clears have no optimized-clear metadata. This one performance-only warning is intentionally scoped to the GPU fixture.
        if (entry.message.find("  [WARNING] (id=820) ID3D12CommandList::ClearRenderTargetView: The application did not pass any clear value")
            != std::string::npos) return;
        ++m_validationFailures;
    }
    void Begin()
    {
        m_resources->AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_frameOpen = true;
    }
    std::vector<float> Capture()
    {
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget(m_target, *m_resources);
        renderer::DrawCall draw;
        draw.shader = m_copy; draw.pipelineState = m_pipeline; draw.vertexCount = 3; draw.textures[5] = m_output;
        device.Submit(draw, *m_resources);
        device.SetRenderTarget({}, *m_resources); device.EndFrame(); m_frameOpen = false;
        std::vector<float> result; uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_target, *m_resources, result, width, height));
        EXPECT_EQ(width, 4u); EXPECT_EQ(height, 1u);
        return result;
    }
    std::vector<float> Reconstruct(std::array<renderer::RayGameTransportRecord, 4> frame,
        std::array<renderer::RayReconstructionSurface, 4> surface,
        std::array<renderer::RayReconstructionHistoryRecord, 4> previous, bool reset = false, bool readCount = false)
    {
        const auto input = m_resources->CreateStructuredBuffer(frame.data(), 4, sizeof(frame[0]));
        const auto firstSurface = m_resources->CreateStructuredBuffer(surface.data(), 4, sizeof(surface[0]));
        const auto old = m_resources->CreateStructuredBuffer(previous.data(), 4, sizeof(previous[0]));
        const auto next = m_resources->CreateRWStructuredBuffer(nullptr, 4, sizeof(previous[0]));
        renderer::RayReconstructionConstants constants;
        constants.previousCameraPosition = {0, 0, 0, 1};
        constants.previousCameraRight = {1, 0, 0, 1}; constants.previousCameraUp = {0, 1, 0, 1};
        constants.previousCameraForward = {0, 0, 1, 0};
        constants.previousOrthographic = 1; constants.width = 4; constants.height = 1;
        constants.resetHistory = reset; constants.spatialRadius = 0;
        Begin();
        m_resources->Update(m_cb, &constants, sizeof(constants));
        renderer::ComputeCall call;
        call.shader = m_shader; call.constantBuffers[0] = m_cb;
        call.srvBuffers[14] = input; call.srvBuffers[15] = firstSurface; call.srvBuffers[29] = old;
        call.uavBuffers[0] = next;
        m_bundle.renderer->Dispatch(call, *m_resources);
        constants.stage = 1; m_resources->Update(m_cb, &constants, sizeof(constants));
        call.uavBuffers[0] = {}; call.uavOutputs[0] = m_output;
        call.srvBuffers[14] = next; call.srvBuffers[15] = input; call.srvBuffers[29] = firstSurface;
        m_bundle.renderer->Dispatch(call, *m_resources);
        if (readCount) {
            renderer::ComputeCall read;
            read.shader = m_reader; read.srvBuffers[14] = next; read.uavOutputs[0] = m_output;
            m_bundle.renderer->Dispatch(read, *m_resources);
        }
        return Capture();
    }
    std::shared_ptr<renderer::RenderScene> Scene(float alphaCutoff = 0.0f)
    {
        auto scene = std::make_shared<renderer::RenderScene>();
        scene->sceneGeneration = 77;
        std::array<renderer::Vertex, 4> vertices{};
        vertices[0].position = {-10, -10, 2}; vertices[1].position = {-10, 10, 2};
        vertices[2].position = {10, 10, 2}; vertices[3].position = {10, -10, 2};
        for (auto& vertex : vertices) { vertex.normal = {0, 0, -1}; vertex.tangent = {1, 0, 0}; }
        const std::array<uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
        renderer::RenderObject object;
        object.sourceIndex = 7; object.sourceGeneration = 9; object.itemCount = 1; object.colorEligible = true;
        object.boundsCenter = {0, 0, 2}; object.boundsRadius = 20;
        scene->objects.push_back(object);
        renderer::RenderMeshItem item;
        item.vertexBuffer = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
        item.indexBuffer = m_resources->CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        item.vertexCount = 4; item.indexCount = 6; item.vertexStride = sizeof(renderer::Vertex);
        item.vertexContentVersion = m_resources->Get(item.vertexBuffer)->GetContentVersion();
        item.indexContentVersion = m_resources->Get(item.indexBuffer)->GetContentVersion();
        item.boundsCenter = {0, 0, 2}; item.boundsRadius = 20;
        item.material.valid = true; item.material.directGBufferParams = true;
        item.material.capabilities.gbufferEquivalentShader = true;
        item.material.rayCapabilities = renderer::ResolveStaticRayMaterialCapabilities(
            true, renderer::BlendMode::OPAQUE_BLEND, 1.0f, alphaCutoff, false);
        auto& surface = item.material.surface;
        surface.standardSurfaceSupported = true; surface.issue = renderer::SurfaceMaterialIssue::NONE;
        surface.baseColor = {0.6f, 0.3f, 0.1f, 1}; surface.roughness = 1; surface.emission = {0.2f, 0.1f, 0.05f};
        surface.alphaCutoff = alphaCutoff;
        auto& params = item.material.gbufferParams;
        const auto put = [&](size_t offset, float value) { std::memcpy(params.data() + offset, &value, sizeof(value)); };
        put(0, 0.6f); put(4, 0.3f); put(8, 0.1f); put(12, 1); put(20, 1); put(24, 1); put(28, 1);
        put(32, 0.2f); put(36, 0.1f); put(40, 0.05f); put(44, 1); put(48, 1); put(52, 1);
        put(64, alphaCutoff);
        item.material.paramsBuffer = m_resources->CreateConstantBuffer(96);
        m_resources->Update(item.material.paramsBuffer, params.data(), params.size());
        scene->items.push_back(item);
        return scene;
    }
    renderer::RenderViewResources* RenderScene(std::shared_ptr<renderer::RenderScene> scene,
        renderer::Camera& camera, uint32_t viewKey, uint64_t frame, renderer::PathTracingProfile profile,
        const char* disabledPass = nullptr, bool perturbDepth = false)
    {
        auto& device = *m_bundle.renderer;
        auto& rendering = m_resources->Rendering();
        auto& view = rendering.View(viewKey);
        renderer::RenderSettings settings;
        settings.renderScale = 1; settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
        settings.modeRequest.pathProfile = profile;
        if (disabledPass) settings.passOverrides.push_back({disabledPass, false});
        settings.ibl.enabled = false; settings.postProcess.fog.enabled = false; settings.froxelFog.enabled = false;
        settings.volumetricLight.enabled = false; settings.postProcess.bloom.enabled = false; settings.autoExposure.enabled = false;
        if (!rendering.PrepareView(view, device, m_target, settings)) return nullptr;
        auto& shared = rendering.Shared();
        if (!m_resources->Get(shared.frameCB)) shared.frameCB = m_resources->CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        if (!m_resources->Get(shared.objectCB)) shared.objectCB = m_resources->CreateConstantBuffer(sizeof(renderer::PerObjectCB));
        if (!m_resources->Get(shared.lightCB)) shared.lightCB = m_resources->CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        if (!m_resources->Get(shared.postprocCB)) shared.postprocCB = m_resources->CreateConstantBuffer(sizeof(renderer::PostProcCB));
        if (!m_resources->Get(shared.shadowCB)) shared.shadowCB = m_resources->CreateConstantBuffer(sizeof(renderer::ShadowConstantsCB));
        if (!m_resources->Get(shared.punctualShadowCB))
            shared.punctualShadowCB = m_resources->CreateConstantBuffer(sizeof(renderer::PunctualShadowConstantsCB));
        if (!m_resources->Get(shared.defaultPSO)) shared.defaultPSO = m_resources->CreatePipelineState({
            renderer::RasterizerMode::SOLID, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        shared.postprocPSO = m_pipeline;
        const std::string root = FBZZ_GRAPHICS_SHADER_ROOT;
        shared.gbufferShader = m_resources->LoadShader(root + "/Pipeline/Deferred/GBuffer.hlsl");
        shared.gbufferInstancedShader = {};
        shared.deferredLightingShader = m_copy; shared.depthCopyShader = m_copy;
        shared.rayPathShader = m_resources->LoadShader(root + "/RayTracing/RayPathTrace.cs.hlsl");
        shared.rayPathResolveShader = m_resources->LoadShader(root + "/RayTracing/RayPathResolve.hlsl");
        shared.rayGameReconstructionShader = m_shader; shared.copyColorShader = m_copy; shared.compositeShader = m_copy;
        renderer::RenderPassHandles handles;
        rendering.BindPassHandles(view, handles);
        renderer::RenderPassContext context{{}, device, *m_resources, camera, settings, m_target, ~0u, handles};
        context.experimentalRayTracingEnabled = true;
        context.renderScene = std::move(scene); context.frameStamp = frame;
        context.width = context.outputWidth = 4; context.height = context.outputHeight = 1;
        context.lightData.lightDir = {0, 0, 1}; context.lightData.lightColor = {1, 1, 1}; context.lightData.lightIntensity = 1;
        renderer::PrepareAdvancedConstants(context, view, {});
        const auto plan = renderer::PrepareViewRenderPlan(*m_resources, device, settings, view, shared, handles,
            context.experimentalRayTracingEnabled);
        if (!plan.IsValid()) return nullptr;
        Begin();
        renderer::BuildViewPipeline(view.pipeline, context, view, shared, {plan});
        const auto names = view.pipeline.RegisteredPassNames();
        if (disabledPass) {
            EXPECT_EQ(view.renderPlan.effectiveMode, renderer::RenderMode::RASTER);
            EXPECT_FALSE(view.rayPathCovered); EXPECT_FALSE(view.rayPathPrepared);
            EXPECT_FALSE(context.rayPathPassActive);
            for (const char* name : {"RayPathTrace", "RayGameTemporal", "RayGameSpatial"})
                EXPECT_EQ(std::find(names.begin(), names.end(), name), names.end());
            /// @note Only preparation/plan fallback is under test; the fixture's dummy Raster lighting shader must not execute.
        } else {
            EXPECT_EQ(view.renderPlan.effectiveMode, renderer::RenderMode::PATH_TRACING);
            EXPECT_EQ(std::find(names.begin(), names.end(), "DeferredLighting"), names.end());
            EXPECT_EQ(std::find(names.begin(), names.end(), "TAA"), names.end());
            EXPECT_TRUE(view.pipeline.Execute(context));
            if (perturbDepth) {
                const auto shader = m_resources->LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
                    / "../../Projects/Tests/Graphics/Shaders/RayGameDepthPerturb.hlsl").lexically_normal().generic_string());
                const auto depthCopy = m_resources->CreateRenderTarget(4, 1, {1, renderer::Format::RGBA16F, false});
                const auto depthWriter = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
                    renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
                const auto constants = m_resources->CreateConstantBuffer(16);
                const std::array<float, 4> bias{-0.00002f, 0, 0, 0};
                m_resources->Update(constants, bias.data(), sizeof(bias));
                if (!shader || !depthCopy || !depthWriter || !constants) return nullptr;
                device.SetRenderTarget(depthCopy, *m_resources);
                renderer::DrawCall draw;
                draw.shader = m_copy; draw.pipelineState = m_pipeline; draw.vertexCount = 3;
                draw.textures[5] = m_resources->GetDepthTexture(view.gbuffer);
                device.Submit(draw, *m_resources);
                device.SetRenderTarget(view.gbuffer, *m_resources);
                device.ClearDepth();
                draw.shader = shader; draw.pipelineState = depthWriter; draw.constantBuffers[0] = constants;
                draw.textures[5] = m_resources->GetColorTexture(depthCopy, 0);
                device.Submit(draw, *m_resources);
                /// @note Re-dispatch the production pass with its original center sample and declarations after a bounded inward depth perturbation.
                renderer::RayPathTracePass trace(view.rayPath, shared.rayPathShader);
                renderer::PassBuilder builder;
                trace.Setup(builder, context);
                renderer::PassResources pass(context.resourceRegistry, builder.Accesses(), trace.Name());
                trace.Execute(pass, context);
                EXPECT_TRUE(view.rayPath.dispatchSucceeded);
                m_resources->Release(depthCopy); m_resources->Release(depthWriter); m_resources->Release(constants);
            }
        }
        device.SetRenderTarget({}, *m_resources); device.EndFrame(); m_frameOpen = false;
        return &view;
    }
    std::vector<float> ReadTable(renderer::ResourceHandle<renderer::StructuredBufferTag> table, uint32_t mode)
    {
        const std::array<uint32_t, 4> constants{mode, 0, 0, 0};
        const auto cb = m_resources->CreateConstantBuffer(sizeof(constants));
        m_resources->Update(cb, constants.data(), sizeof(constants));
        Begin();
        renderer::ComputeCall read;
        read.shader = m_reader; read.constantBuffers[0] = cb;
        read.srvBuffers[14] = table; read.uavOutputs[0] = m_output;
        m_bundle.renderer->Dispatch(read, *m_resources);
        return Capture();
    }
    std::vector<float> ReadTexture(renderer::ResourceHandle<renderer::TextureTag> texture)
    {
        const auto saved = m_output; m_output = texture;
        Begin(); const auto result = Capture(); m_output = saved;
        return result;
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ResourceHandle<renderer::ShaderTag> m_shader, m_reader, m_copy;
    renderer::ResourceHandle<renderer::TextureTag> m_output;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_cb;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
    uint32_t m_validationFailures = 0;
};

TEST_F(RayGameTest, ReusesOnlyDiffuseHistoryAndAddsIndependentRadianceExactlyOnce)
{
    std::array<renderer::RayGameTransportRecord, 4> frame{};
    std::array<renderer::RayReconstructionSurface, 4> surface{};
    std::array<renderer::RayReconstructionHistoryRecord, 4> previous{};
    for (uint32_t i = 0; i < 4; ++i) {
        surface[i] = Surface(i); previous[i].surface = surface[i];
        frame[i].diffuse = {1, 0, 0, 1}; frame[i].specular = {0, 0.5f, 0, 1}; frame[i].independent = {0, 0, 0.25f, 1};
        previous[i].diffuse = {3, 0, 0, 7}; previous[i].specular = {0, 99, 0, 7};
    }
    const auto result = Reconstruct(frame, surface, previous);
    ASSERT_EQ(result.size(), 16u);
    EXPECT_NEAR(result[0], 2.75f, 0.001f); EXPECT_NEAR(result[1], 0.5f, 0.001f); EXPECT_NEAR(result[2], 0.25f, 0.001f);
    const auto counts = Reconstruct(frame, surface, previous, false, true);
    EXPECT_NEAR(counts[0], 8, 0.001f); EXPECT_NEAR(counts[1], 1, 0.001f);
}
TEST_F(RayGameTest, RejectsSkinnedMissingMotionFullIdentityChangesAndDisocclusion)
{
    std::array<renderer::RayGameTransportRecord, 4> frame{};
    std::array<renderer::RayReconstructionSurface, 4> surface{};
    std::array<renderer::RayReconstructionHistoryRecord, 4> previous{};
    for (uint32_t i = 0; i < 4; ++i) {
        surface[i] = Surface(i); previous[i].surface = surface[i];
        frame[i].diffuse = {1, 0, 0, 1}; previous[i].diffuse = {20, 0, 0, 7};
    }
    surface[0].previousPositionValid.w = 0;
    surface[1].objectMaterialValid[1]++;
    surface[2].sceneFlags[1]++;
    surface[3].previousPositionValid.z += 0.1f;
    const auto result = Reconstruct(frame, surface, previous);
    ASSERT_EQ(result.size(), 16u);
    for (uint32_t i = 0; i < 4; ++i) EXPECT_NEAR(result[i * 4], 1, 0.001f);
}
TEST_F(RayGameTest, ResetsHistoryWithoutChangingFreshTransportOrTreatingUncomputedPixelsAsBlack)
{
    std::array<renderer::RayGameTransportRecord, 4> frame{};
    std::array<renderer::RayReconstructionSurface, 4> surface{};
    std::array<renderer::RayReconstructionHistoryRecord, 4> previous{};
    for (uint32_t i = 0; i < 4; ++i) {
        surface[i] = Surface(i); previous[i].surface = surface[i];
        frame[i].diffuse = {1, 2, 3, 1}; previous[i].diffuse = {20, 0, 0, 7};
    }
    const auto result = Reconstruct(frame, surface, previous, true);
    ASSERT_EQ(result.size(), 16u);
    EXPECT_NEAR(result[0], 1, 0.001f); EXPECT_NEAR(result[1], 2, 0.001f); EXPECT_NEAR(result[2], 3, 0.001f);
    surface[0].objectMaterialValid[3] = 3;
    const auto border = Reconstruct(frame, surface, previous);
    EXPECT_NEAR(border[0], 1, 0.001f);
}
TEST_F(RayGameTest, ProductionGraphPreservesRawComponentSumAndIndependentViewHistories)
{
    auto scene = Scene();
    renderer::Camera camera; camera.m_position = {}; camera.m_aspect = 4; camera.m_backgroundColor = {0, 0, 0, 1};
    camera.m_projection = renderer::ProjectionMode::Orthographic; camera.m_orthoHeight = 2;
    auto* game = RenderScene(scene, camera, 100, 1, renderer::PathTracingProfile::GAME);
    ASSERT_NE(game, nullptr); ASSERT_TRUE(game->rayPath.game.reconstructed);
    EXPECT_EQ(game->rayPath.history.GetSampleCount(), 0u);
    const auto raw = ReadTable(game->rayPath.game.transport, 1);
    const auto diffuse = ReadTable(game->rayPath.game.transport, 2);
    const auto specular = ReadTable(game->rayPath.game.transport, 3);
    const auto emission = ReadTable(game->rayPath.game.transport, 4);
    const auto surface = ReadTable(game->rayPath.game.surface, 5);
    ASSERT_EQ(raw.size(), 16u); ASSERT_EQ(surface.size(), 16u);
    for (uint32_t channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(raw[channel], diffuse[channel] + specular[channel] + emission[channel], 0.002f);
        EXPECT_GT(diffuse[channel], 0); EXPECT_GT(specular[channel], 0);
    }
    EXPECT_NEAR(emission[0], 0.2f, 0.001f); EXPECT_NEAR(surface[0], 1, 0.001f);
    auto* reference = RenderScene(scene, camera, 101, 1, renderer::PathTracingProfile::REFERENCE);
    ASSERT_NE(reference, nullptr); EXPECT_EQ(reference->rayPath.history.GetSampleCount(), 1u);
    const auto baseline = ReadTexture(reference->rayPath.output);
    for (uint32_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(raw[channel], baseline[channel], 0.002f);
    ASSERT_NE(RenderScene(scene, camera, 100, 2, renderer::PathTracingProfile::GAME), nullptr);
    EXPECT_EQ(game->rayPath.game.frameSampleIndex, 2u);
    EXPECT_EQ(reference->rayPath.history.GetSampleCount(), 1u);
    const auto history = ReadTable(game->rayPath.game.reconstructionHistory[game->rayPath.game.historyReadIndex], 0);
    EXPECT_NEAR(history[0], 2, 0.001f); EXPECT_NEAR(history[1], 1, 0.001f);
}

TEST_F(RayGameTest, SwitchingProfilesInTheSameViewKeepsRawHistorySeparateAndRejectsTheGameFrameGap)
{
    auto scene = Scene();
    renderer::Camera camera; camera.m_position = {}; camera.m_aspect = 4; camera.m_backgroundColor = {0, 0, 0, 1};
    camera.m_projection = renderer::ProjectionMode::Orthographic; camera.m_orthoHeight = 2;
    auto* view = RenderScene(scene, camera, 102, 1, renderer::PathTracingProfile::GAME);
    ASSERT_NE(view, nullptr);
    ASSERT_NE(RenderScene(scene, camera, 102, 2, renderer::PathTracingProfile::REFERENCE), nullptr);
    EXPECT_EQ(view->rayPath.history.GetSampleCount(), 1u);
    ASSERT_NE(RenderScene(scene, camera, 102, 3, renderer::PathTracingProfile::GAME), nullptr);
    EXPECT_EQ(view->rayPath.history.GetSampleCount(), 1u);
    auto count = ReadTable(view->rayPath.game.reconstructionHistory[view->rayPath.game.historyReadIndex], 0);
    ASSERT_EQ(count.size(), 16u); EXPECT_NEAR(count[0], 1, 0.001f);
    ASSERT_NE(RenderScene(scene, camera, 102, 4, renderer::PathTracingProfile::GAME), nullptr);
    count = ReadTable(view->rayPath.game.reconstructionHistory[view->rayPath.game.historyReadIndex], 0);
    EXPECT_NEAR(count[0], 2, 0.001f);
    ASSERT_NE(RenderScene(scene, camera, 102, 5, renderer::PathTracingProfile::REFERENCE), nullptr);
    EXPECT_EQ(view->rayPath.history.GetSampleCount(), 2u);
}

TEST_F(RayGameTest, ConstantOpaqueWithAnInertAlphaCutoffKeepsRasterPrimaryAndAccumulatesGameHistory)
{
    renderer::Camera camera; camera.m_position = {}; camera.m_aspect = 4; camera.m_backgroundColor = {0, 0, 0, 1};
    camera.m_projection = renderer::ProjectionMode::Orthographic; camera.m_orthoHeight = 2;
    for (const float cutoff : {0.5f, 1.0f}) {
        auto scene = Scene(cutoff);
        const uint32_t viewKey = cutoff < 1 ? 105u : 106u;
        auto* view = RenderScene(scene, camera, viewKey, 1, renderer::PathTracingProfile::GAME);
        ASSERT_NE(view, nullptr); ASSERT_TRUE(view->rayPath.game.reconstructed);
        EXPECT_EQ(view->renderPlan.effectiveMode, renderer::RenderMode::PATH_TRACING);
        const auto surface = ReadTable(view->rayPath.game.surface, 5);
        ASSERT_EQ(surface.size(), 16u);
        for (uint32_t i = 0; i < 4; ++i) EXPECT_NEAR(surface[i * 4], 1, 0.001f);
        ASSERT_NE(RenderScene(scene, camera, viewKey, 2, renderer::PathTracingProfile::GAME), nullptr);
        const auto history = ReadTable(view->rayPath.game.reconstructionHistory[view->rayPath.game.historyReadIndex], 0);
        ASSERT_EQ(history.size(), 16u);
        for (uint32_t i = 0; i < 4; ++i) EXPECT_NEAR(history[i * 4], 2, 0.001f);
    }
}

TEST_F(RayGameTest, ReprojectsRigidPreviousWorldAndRejectsTheNewlyExposedPixel)
{
    auto scene = Scene();
    auto& item = scene->items[0];
    item.material.surface.emission = {};
    for (const size_t offset : {32u, 36u, 40u}) {
        const float zero = 0;
        std::memcpy(item.material.gbufferParams.data() + offset, &zero, sizeof(zero));
    }
    m_resources->Update(item.material.paramsBuffer, item.material.gbufferParams.data(), item.material.gbufferParams.size());
    renderer::Camera camera; camera.m_position = {}; camera.m_aspect = 4; camera.m_backgroundColor = {0, 0, 0, 1};
    camera.m_projection = renderer::ProjectionMode::Orthographic; camera.m_orthoHeight = 2;
    auto* view = RenderScene(scene, camera, 103, 1, renderer::PathTracingProfile::GAME);
    ASSERT_NE(view, nullptr);
    scene->objects[0].previousWorld = scene->objects[0].world;
    scene->objects[0].world = math::Matrix4::Translate({2, 0, 0});
    scene->objects[0].boundsCenter.x = 2;
    ASSERT_NE(RenderScene(scene, camera, 103, 2, renderer::PathTracingProfile::GAME), nullptr);
    EXPECT_EQ(view->rayPath.game.reconstructionData.resetHistory, 0u);
    const auto surface = ReadTable(view->rayPath.game.surface, 5);
    ASSERT_EQ(surface.size(), 16u);
    EXPECT_NEAR(surface[4], 1, 0.001f); EXPECT_NEAR(surface[5], 1, 0.001f);
    const auto counts = ReadTable(view->rayPath.game.reconstructionHistory[view->rayPath.game.historyReadIndex], 0);
    ASSERT_EQ(counts.size(), 16u);
    EXPECT_NEAR(counts[0], 1, 0.001f);
    EXPECT_NEAR(counts[4], 2, 0.001f); EXPECT_NEAR(counts[8], 2, 0.001f); EXPECT_NEAR(counts[12], 2, 0.001f);
}

TEST_F(RayGameTest, BoundedRasterDepthErrorDoesNotMoveTheVerifiedSpawnInsideThePrimarySurface)
{
    auto scene = Scene();
    scene->items[0].material.surface.emission = {};
    for (const size_t offset : {32u, 36u, 40u}) {
        const float zero = 0;
        std::memcpy(scene->items[0].material.gbufferParams.data() + offset, &zero, sizeof(zero));
    }
    m_resources->Update(scene->items[0].material.paramsBuffer, scene->items[0].material.gbufferParams.data(), 96);
    renderer::Camera camera; camera.m_position = {}; camera.m_aspect = 4; camera.m_backgroundColor = {0, 0, 0, 1};
    camera.m_projection = renderer::ProjectionMode::Orthographic; camera.m_orthoHeight = 2;
    /// @note Plane z=2 yields exact half depth=.5. The injected -2e-5 depth moves the reconstruction inward by 40 micrometers, below the unchanged match tolerance.
    camera.m_near = 1; camera.m_far = 3;
    auto* baseline = RenderScene(scene, camera, 107, 1, renderer::PathTracingProfile::GAME);
    ASSERT_NE(baseline, nullptr);
    const auto expected = ReadTable(baseline->rayPath.game.transport, 1);
    ASSERT_EQ(expected.size(), 16u);
    auto* perturbed = RenderScene(scene, camera, 108, 1, renderer::PathTracingProfile::GAME, nullptr, true);
    ASSERT_NE(perturbed, nullptr);
    const auto actual = ReadTable(perturbed->rayPath.game.transport, 1);
    const auto surface = ReadTable(perturbed->rayPath.game.surface, 5);
    ASSERT_EQ(actual.size(), 16u); ASSERT_EQ(surface.size(), 16u);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_GT(expected[i * 4], 0.5f);
        EXPECT_NEAR(surface[i * 4], 1, 0.001f);
        EXPECT_NEAR(surface[i * 4 + 2], 0.5f, 0.001f);
        for (uint32_t channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(actual[i * 4 + channel], expected[i * 4 + channel], 0.002f);
    }
}

TEST_F(RayGameTest, DisablingAnyRequiredRasterSurfaceOrReconstructionPassFallsBackTheWholeView)
{
    auto scene = Scene();
    renderer::Camera camera; camera.m_position = {}; camera.m_aspect = 4; camera.m_backgroundColor = {0, 0, 0, 1};
    camera.m_projection = renderer::ProjectionMode::Orthographic; camera.m_orthoHeight = 2;
    uint64_t frame = 1;
    for (const char* name : {"DeferredGBuffer", "RayGameTemporal", "RayGameSpatial"}) {
        auto* view = RenderScene(scene, camera, 104, frame++, renderer::PathTracingProfile::GAME);
        ASSERT_NE(view, nullptr); ASSERT_TRUE(view->rayPath.game.reconstructed);
        ASSERT_NE(RenderScene(scene, camera, 104, frame++, renderer::PathTracingProfile::GAME, name), nullptr);
        EXPECT_EQ(view->renderPlan.effectiveMode, renderer::RenderMode::RASTER);
    }
}

} /// @note namespace
} /// @note namespace fbzz::tests
