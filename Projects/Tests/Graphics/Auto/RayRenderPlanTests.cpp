/// @file    RayRenderPlanTests.cpp
/// @brief   Engine をリンクせず明示選択の RT パイプラインと GPU 描画を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/AccelerationStructure.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Core/ILogSink.hpp>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
class RayRenderPlanTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        ASSERT_EQ(GetModuleHandleW(L"FBZZEngine.dll"), nullptr);
        ASSERT_EQ(GetModuleHandleW(L"FBZZPhysics.dll"), nullptr);
        ASSERT_EQ(GetModuleHandleW(L"FBZZFluid.dll"), nullptr);
    }
};

TEST_F(RayRenderPlanTest, HybridScreenFirstOverridesKeepCurrentReceiptsAndIndependentViews)
{
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    ASSERT_TRUE(SUCCEEDED(com.result));
    struct DiagnosticSink : core::ILogSink {
        uint32_t failures = 0;
        DiagnosticSink() { core::Logger::AddSink(this); }
        ~DiagnosticSink() { core::Logger::RemoveSink(this); }
        void OnLog(const core::LogEntry& entry) override {
            if (entry.message.find("  [WARNING] (id=") == std::string::npos
                && entry.message.find("  [ERROR] (id=") == std::string::npos
                && entry.message.find("  [CORRUPTION] (id=") == std::string::npos) return;
            /// @note Empty-view clears may lack optimized-clear metadata; only the known id820 performance warning is excluded.
            if (entry.message.find("  [WARNING] (id=820) ID3D12CommandList::ClearRenderTargetView: The application did not pass any clear value")
                != std::string::npos) return;
            ++failures;
            std::fprintf(stderr, "%s\n", entry.message.c_str());
        }
    } diagnostics;
    struct HiddenWindow {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Hybrid screen-first graph", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    } window;
    ASSERT_NE(window.handle, nullptr);
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, window.handle, 32, 32);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    if (!device.GetCapabilities().inlineRayQuery) GTEST_SKIP() << "Inline ray queries are unavailable";
    {
        renderer::ResourceManager resources(device);
        auto& rendering = resources.Rendering();
        rendering.SetExperimentalRayTracingEnabled(true);
        auto& shared = rendering.Shared();
        shared.frameCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        shared.objectCB = resources.CreateConstantBuffer(sizeof(renderer::PerObjectCB));
        shared.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        shared.postprocCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
        shared.shadowCB = resources.CreateConstantBuffer(sizeof(renderer::ShadowConstantsCB));
        shared.punctualShadowCB = resources.CreateConstantBuffer(sizeof(renderer::PunctualShadowConstantsCB));
        shared.defaultPSO = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        shared.postprocPSO = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        shared.shadowMapRT.Ensure(resources, 1, 1, 0);
        shared.punctualShadowRT.Ensure(resources, 1, 1, 0);
        shared.lightCookieRT.Ensure(resources, renderer::kLightCookieAtlasWidth,
            renderer::kLightCookieAtlasHeight);
        ASSERT_TRUE(shared.shadowMapRT.IsValid() && shared.punctualShadowRT.IsValid()
            && shared.lightCookieRT.IsValid());
        const std::string shaderRoot = FBZZ_GRAPHICS_SHADER_ROOT;
        shared.gbufferShader = resources.LoadShader(shaderRoot + "/Pipeline/Deferred/GBuffer.hlsl");
        shared.depthCopyShader = resources.LoadShader(shaderRoot + "/Pipeline/Deferred/DepthCopy.hlsl");
        shared.deferredLightingShader = resources.LoadShader(shaderRoot + "/Pipeline/Deferred/DeferredLighting.hlsl");
        shared.ssrShader = resources.LoadShader(shaderRoot + "/PostProcess/Reflections/SSR.cs.hlsl");
        shared.rayReflectionShader = resources.LoadShader(shaderRoot + "/RayTracing/RayReflection.cs.hlsl");
        shared.compositeShader = resources.LoadShader(shaderRoot + "/PostProcess/Color/Composite.hlsl");
        ASSERT_TRUE(shared.gbufferShader.IsValid() && shared.depthCopyShader.IsValid()
            && shared.deferredLightingShader.IsValid() && shared.ssrShader.IsValid()
            && shared.rayReflectionShader.IsValid() && shared.compositeShader.IsValid());
        renderer::RenderSettings settings;
        settings.pipeline = renderer::RenderingPipeline::Deferred;
        settings.modeRequest.mode = renderer::RenderMode::HYBRID;
        settings.modeRequest.rayReflection = true;
        settings.renderScale = 1;
        settings.ssr.enabled = true;
        settings.ibl.enabled = settings.shadowEnabled = settings.autoExposure.enabled = false;
        settings.postProcess.fog.enabled = settings.froxelFog.enabled = settings.volumetricLight.enabled = false;
        settings.postProcess.bloom.enabled = false;
        settings.shadow.mapResolution = 1;
        auto secondSettings = settings;
        auto& firstView = rendering.View(60);
        auto& secondView = rendering.View(61);
        const std::array views{&firstView, &secondView};
        const std::array outputs{resources.CreateRenderTarget(8, 8), resources.CreateRenderTarget(8, 8)};
        ASSERT_TRUE(outputs[0].IsValid() && outputs[1].IsValid());
        auto scene = std::make_shared<renderer::RenderScene>();
        scene->sceneGeneration = 812;
        renderer::Camera camera;
        camera.m_backgroundColor = {0, 0, 0, 1};
        for (uint32_t frame = 0; frame < 7; ++frame) {
            settings.passOverrides.clear();
            if (frame == 1) settings.passOverrides.push_back({"SSR", false});
            if (frame == 2) settings.passOverrides.push_back({"ReflectionSourceLighting", false});
            if (frame == 4) settings.passOverrides.push_back({"DeferredGBuffer", false});
            for (size_t viewIndex = 0; viewIndex < views.size(); ++viewIndex)
                ASSERT_TRUE(rendering.PrepareView(*views[viewIndex], device, outputs[viewIndex],
                    viewIndex == 0 ? settings : secondSettings));
            resources.AdvanceFrame();
            device.BeginFrame();
            for (size_t viewIndex = 0; viewIndex < views.size(); ++viewIndex) {
                SCOPED_TRACE(frame);
                SCOPED_TRACE(viewIndex);
                auto& view = *views[viewIndex];
                const auto& currentSettings = viewIndex == 0 ? settings : secondSettings;
                renderer::RenderPassHandles handles;
                rendering.BindPassHandles(view, handles);
                renderer::RenderPassContext context{{}, device, resources, camera, currentSettings,
                    outputs[viewIndex], ~0u, handles};
                context.experimentalRayTracingEnabled = true;
                context.renderScene = scene;
                const bool gbufferDisabled = viewIndex == 0 && frame == 4;
                const bool expectedLights = viewIndex == 1 || frame != 5;
                const bool expectedRay = expectedLights && !gbufferDisabled;
                context.rayLightsComplete = expectedLights;
                context.width = context.height = context.outputWidth = context.outputHeight = 8;
                context.chainOutputRT = outputs[viewIndex];
                context.frameStamp = resources.FrameStamp();
                context.punctualShadowResolution = 1;
                /// @note Engine's optimistic old flag must not survive a skipped SSR pass.
                context.ssrPassActive = context.hybridReflectionSourcePass = true;
                renderer::PrepareAdvancedConstants(context, view, {});
                const auto initial = renderer::PrepareViewRenderPlan(resources, device, currentSettings,
                    view, shared, handles, true);
                ASSERT_TRUE(initial.IsValid());
                const auto previousReflectionFrame = view.rayReflection.reconstruction.lastFrameStamp;
                const auto previousReflectionSurface = view.rayReflection.reconstruction.surfaceReadIndex;
                renderer::BuildViewPipeline(view.pipeline, context, view, shared, {initial}, {});
                ASSERT_EQ(view.renderPlan.effectiveMode, expectedRay
                    ? renderer::RenderMode::HYBRID : renderer::RenderMode::RASTER);
                const bool expectedSsr = viewIndex == 1 || (frame != 1 && frame != 2 && frame != 4);
                EXPECT_TRUE(context.hybridReflectionResolveActive);
                EXPECT_EQ(context.hybridReflectionSsrPlanned, expectedSsr);
                EXPECT_FALSE(context.ssrPassActive);
                EXPECT_FALSE(context.hybridReflectionSourcePass);
                ASSERT_TRUE(view.pipeline.Execute(context));
                EXPECT_EQ(context.rayReflectionPassActive, expectedRay);
                EXPECT_EQ(context.ssrPassActive, expectedSsr);
                EXPECT_FALSE(context.hybridReflectionSourcePass);
                if (gbufferDisabled) {
                    /// @note Cached RAW/storage may remain live, but the stopped producer must not publish a new RT/SSR result or history commit.
                    EXPECT_NE(resources.Get(view.rayReflection.output), nullptr);
                    EXPECT_FALSE(context.rayReflectionReconstructionPrepared);
                    EXPECT_FALSE(view.rayReflection.reconstruction.historyValid);
                    EXPECT_FALSE(view.rayReflection.reconstruction.rawSucceeded);
                    EXPECT_EQ(view.rayReflection.reconstruction.lastFrameStamp, previousReflectionFrame);
                    EXPECT_EQ(view.rayReflection.reconstruction.surfaceReadIndex, previousReflectionSurface);
                    EXPECT_FALSE(handles.rayReflectionResult);
                }
                std::vector<std::string> executed;
                for (const auto& profile : view.pipeline.LastReport().profiles) executed.push_back(profile.name);
                const auto find = [&](const char* name) { return std::find(executed.begin(), executed.end(), name); };
                const auto reflection = find("RayReflection"), lighting = find("DeferredLighting");
                ASSERT_NE(lighting, executed.end());
                EXPECT_EQ(find("DeferredGBuffer") != executed.end(), !gbufferDisabled);
                EXPECT_EQ(reflection != executed.end(), expectedRay);
                if (expectedRay) EXPECT_LT(reflection, lighting);
                EXPECT_EQ(std::count(executed.begin(), executed.end(), "Sky"), 1);
                EXPECT_EQ(std::count(executed.begin(), executed.end(), "VolumetricCloud"), 1);
                EXPECT_LT(lighting, find("VolumetricCloud"));
                EXPECT_EQ(find("SSR") != executed.end(), expectedSsr);
                EXPECT_EQ(find("ReflectionSourceLighting") != executed.end(), expectedSsr);
                if (expectedSsr) {
                    EXPECT_LT(find("ReflectionSourceLighting"), find("Sky"));
                    EXPECT_LT(find("Sky"), find("SSR"));
                    EXPECT_LT(find("SSR"), lighting);
                    if (expectedRay) EXPECT_LT(find("SSR"), reflection);
                }
            }
            EXPECT_NE(firstView.ssrResult, secondView.ssrResult);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
        }
    }
    bundle.imguiRenderer.reset();
    device.Shutdown();
    EXPECT_EQ(diagnostics.failures, 0u);
}

TEST_F(RayRenderPlanTest, ReferencePathKeepsAccumulatingAfterAnotherViewPreparesDerivedProbes)
{
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    ASSERT_TRUE(SUCCEEDED(com.result));
    struct DiagnosticSink : core::ILogSink {
        uint32_t failures = 0;
        DiagnosticSink() { core::Logger::AddSink(this); }
        ~DiagnosticSink() { core::Logger::RemoveSink(this); }
        void OnLog(const core::LogEntry& entry) override {
            if (entry.message.find("  [WARNING] (id=") == std::string::npos
                && entry.message.find("  [ERROR] (id=") == std::string::npos
                && entry.message.find("  [CORRUPTION] (id=") == std::string::npos) return;
            /// @note This fixture intentionally clears PathColor's target without optimized-clear metadata; id820 changes performance, not validation correctness.
            if (entry.message.find("  [WARNING] (id=820) ID3D12CommandList::ClearRenderTargetView: The application did not pass any clear value")
                != std::string::npos) return;
            ++failures;
            std::fprintf(stderr, "%s\n", entry.message.c_str());
            std::fflush(stderr);
        }
    } diagnostics;
    struct HiddenWindow {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Path view probe regression", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    } window;
    ASSERT_NE(window.handle, nullptr);
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, window.handle, 32, 32);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    if (!device.GetCapabilities().inlineRayQuery) GTEST_SKIP() << "Inline ray queries are unavailable";
    {
        renderer::ResourceManager resources(device);
        auto& rendering = resources.Rendering();
        rendering.SetExperimentalRayTracingEnabled(true);
        auto& gameView = rendering.View(10);
        auto& sceneView = rendering.View(11);
        const auto gameOutput = resources.CreateRenderTarget(8, 8);
        const auto sceneOutput = resources.CreateRenderTarget(8, 8);
        const auto probeCube = resources.CreateCubemapRenderTarget(1);
        const auto probeTexture = resources.GetCubemapTexture(probeCube);
        ASSERT_TRUE(gameOutput.IsValid() && sceneOutput.IsValid() && probeTexture.IsValid());
        renderer::RenderSettings settings;
        settings.renderScale = 1;
        settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
        settings.modeRequest.pathProfile = renderer::PathTracingProfile::REFERENCE;
        settings.ibl.enabled = false;
        settings.postProcess.fog.enabled = false;
        settings.froxelFog.enabled = false;
        settings.volumetricLight.enabled = false;
        settings.postProcess.bloom.enabled = false;
        settings.autoExposure.enabled = false;
        auto sceneSettings = settings;
        sceneSettings.modeRequest.mode = renderer::RenderMode::RASTER;
        ASSERT_TRUE(rendering.PrepareView(gameView, device, gameOutput, settings));
        ASSERT_TRUE(rendering.PrepareView(sceneView, device, sceneOutput, sceneSettings));
        auto& shared = rendering.Shared();
        shared.frameCB = resources.CreateConstantBuffer(256);
        shared.objectCB = resources.CreateConstantBuffer(256);
        shared.lightCB = resources.CreateConstantBuffer(256);
        shared.postprocCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
        shared.defaultPSO = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        shared.postprocPSO = shared.defaultPSO;
        const std::string shaderRoot = FBZZ_GRAPHICS_SHADER_ROOT;
        shared.rayPathShader = resources.LoadShader(shaderRoot + "/RayTracing/RayPathTrace.cs.hlsl");
        shared.rayPathResolveShader = resources.LoadShader(shaderRoot + "/RayTracing/RayPathResolve.hlsl");
        shared.rayReflectionShader = resources.LoadShader(shaderRoot + "/RayTracing/RayReflection.cs.hlsl");
        shared.copyColorShader = resources.LoadShader(shaderRoot + "/PostProcess/Color/CopyColor.hlsl");
        shared.compositeShader = shared.copyColorShader;
        ASSERT_TRUE(shared.rayPathShader.IsValid() && shared.rayPathResolveShader.IsValid()
            && shared.rayReflectionShader.IsValid() && shared.copyColorShader.IsValid());
        auto scene = std::make_shared<renderer::RenderScene>();
        scene->sceneGeneration = 456;
        renderer::Camera camera;
        camera.m_backgroundColor = {1, 2, 3, 1};
        renderer::Camera sceneCamera;
        renderer::RenderPassHandles handles;
        renderer::RenderPassHandles sceneHandles;
        renderer::RenderPassContext context{{}, device, resources, camera, settings, gameOutput, ~0u, handles};
        renderer::RenderPassContext sceneContext{{}, device, resources, sceneCamera, sceneSettings,
            sceneOutput, ~0u, sceneHandles};
        context.experimentalRayTracingEnabled = true;
        sceneContext.experimentalRayTracingEnabled = true;
        context.renderScene = scene;
        sceneContext.renderScene = scene;
        context.rayLightsComplete = true;
        context.width = context.height = context.outputWidth = context.outputHeight = 8;
        sceneContext.width = sceneContext.height = sceneContext.outputWidth = sceneContext.outputHeight = 8;
        context.lightData.lightDir = {0, -1, 0};
        context.lightData.lightIntensity = 0;
        renderer::AdvancedViewInput input;
        input.dynamicIblReady = true;
        input.dynamicIblMipCount = 1;
        /// @note SceneView completes shared raster probes before GameView prepares its next independent Path history.
        for (uint32_t frame = 0; frame < 4; ++frame) {
            resources.AdvanceFrame();
            device.BeginFrame();
            ASSERT_TRUE(rendering.PrepareView(sceneView, device, sceneOutput, sceneSettings));
            ASSERT_TRUE(rendering.PrepareView(gameView, device, gameOutput, settings));
            rendering.BindPassHandles(sceneView, sceneHandles);
            rendering.BindPassHandles(gameView, handles);
            sceneHandles.iblIrradiance = sceneHandles.iblPrefilter = probeTexture;
            handles.iblIrradiance = handles.iblPrefilter = probeTexture;
            input.reflectionProbeSelected = (frame & 1u) != 0;
            const float bakedIntensity = frame >= 2 ? 1.0f : 0.0f;
            uint32_t preparedProbeViews = 0;
            const auto prepareProbes = [&](renderer::AdvancedGraphicsCB& data) {
                ++preparedProbeViews;
                data.probeVolumes[0].intensity = bakedIntensity;
                data.probeVolumes[1].intensity = bakedIntensity * 2;
            };
            renderer::PrepareAdvancedConstants(sceneContext, sceneView, input, prepareProbes);
            EXPECT_EQ(gameView.rayPath.history.GetSampleCount(), frame);
            renderer::PrepareAdvancedConstants(context, gameView, input, prepareProbes);
            EXPECT_EQ(preparedProbeViews, 2u);
            EXPECT_TRUE(context.rayPathLightingSupported);
            EXPECT_EQ(context.rayHitLightingSupported, frame == 0);
            ++scene->snapshotSerial;
            const auto requestedPlan = renderer::PrepareViewRenderPlan(resources, device, settings,
                gameView, shared, handles, true);
            ASSERT_TRUE(requestedPlan.IsValid());
            renderer::BuildViewPipeline(gameView.pipeline, context, gameView, shared, {requestedPlan}, {});
            ASSERT_EQ(gameView.renderPlan.effectiveMode, renderer::RenderMode::PATH_TRACING);
            EXPECT_EQ(gameView.rayPath.constantsData.sampleBase, frame);
            EXPECT_EQ(gameView.rayPath.constantsData.resetHistory, frame == 0 ? 1u : 0u);
            EXPECT_EQ(sceneView.rayPath.history.GetSampleCount(), 0u);
            ASSERT_TRUE(gameView.pipeline.Execute(context));
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            EXPECT_EQ(gameView.rayPath.history.GetSampleCount(), frame + 1);
        }
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        ASSERT_TRUE(device.CaptureRenderTargetToLinearRGBA(gameView.hdr, resources, pixels, width, height));
        ASSERT_EQ(pixels.size(), 8u * 8u * 4u);
        EXPECT_NEAR(pixels[0], 1, 0.001f);
        EXPECT_NEAR(pixels[1], 2, 0.001f);
        EXPECT_NEAR(pixels[2], 3, 0.001f);

        /// @note Reflection hit lighting uses the complete ray table and explicit raw environment, independently of Raster probes.
        settings.modeRequest.mode = renderer::RenderMode::HYBRID;
        settings.modeRequest.rayReflection = true;
        input.reflectionProbeSelected = false;
        renderer::PrepareAdvancedConstants(context, gameView, input);
        ASSERT_TRUE(renderer::PrepareRayReflectionView(context, gameView, shared,
            {renderer::OpaqueRenderPath::DEFERRED}));
        input.reflectionProbeSelected = true;
        renderer::PrepareAdvancedConstants(context, gameView, input);
        EXPECT_TRUE(renderer::PrepareRayReflectionView(context, gameView, shared,
            {renderer::OpaqueRenderPath::DEFERRED}));
        EXPECT_TRUE(context.rayReflectionPassActive);
        input.reflectionProbeSelected = false;
        renderer::PrepareAdvancedConstants(context, gameView, input, [](renderer::AdvancedGraphicsCB& data) {
            data.probeVolumes[0].intensity = 1;
        });
        EXPECT_TRUE(renderer::PrepareRayReflectionView(context, gameView, shared,
            {renderer::OpaqueRenderPath::DEFERRED}));
        EXPECT_TRUE(gameView.rayReflectionCovered);

        settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
        for (uint32_t condition = 0; condition < 3; ++condition) {
            input.weatherWetness = condition == 0 ? 0.5f : 0;
            input.weatherPuddle = condition == 1 ? 0.5f : 0;
            context.lightData.pointLightCount = condition == 2 ? 1 : 0;
            context.rayLightsComplete = condition != 2;
            renderer::PrepareAdvancedConstants(context, gameView, input);
            EXPECT_FALSE(renderer::PrepareRayPathView(context, gameView, shared));
            EXPECT_FALSE(context.rayPathPassActive);
            const auto fallback = renderer::PrepareViewRenderPlan(resources, device, settings,
                gameView, shared, handles, true);
            EXPECT_EQ(fallback.effectiveMode, renderer::RenderMode::RASTER);
            EXPECT_EQ(settings.modeRequest.mode, renderer::RenderMode::PATH_TRACING);
        }
        input.weatherPuddle = 0;
        context.lightData.pointLightCount = 0;
        context.rayLightsComplete = true;
        renderer::PrepareAdvancedConstants(context, gameView, input);
        EXPECT_TRUE(renderer::PrepareRayPathView(context, gameView, shared));
        EXPECT_EQ(gameView.rayPath.history.GetSampleCount(), 0u);
        EXPECT_EQ(gameView.rayPath.constantsData.resetHistory, 1u);
        /// @note A complete Ray table replaces truncated Raster inputs and uploads t3/t5 through the production graph.
        context.rayLightsComplete = true;
        context.lightData.lightIntensity = 99;
        context.lightData.pointLightCount = 8;
        context.lightData.spotLightCount = 4;
        context.legacyShapedLightCount = 4;
        context.punctualLights.emplace_back();
        renderer::RayLightInput area;
        area.objectId = {scene->sceneGeneration, 20, 1};
        area.type = renderer::RayLightType::AREA;
        area.position = camera.m_position + camera.GetForward() * 3;
        area.direction = -camera.GetForward();
        area.tangent = camera.GetRight();
        area.bitangent = camera.GetUp();
        area.areaWidth = area.areaHeight = 20;
        area.color = {0.25f, 0.5f, 1};
        area.intensity = 2;
        renderer::RayLightInput point;
        point.objectId = {scene->sceneGeneration, 21, 1};
        point.position = camera.m_position;
        point.intensity = 3;
        context.rayLights = {area, point};
        for (uint32_t frame = 0; frame < 3; ++frame) {
            if (frame == 2) context.rayLights[1].intensity = 4;
            resources.AdvanceFrame();
            device.BeginFrame();
            ASSERT_TRUE(rendering.PrepareView(gameView, device, gameOutput, settings));
            rendering.BindPassHandles(gameView, handles);
            renderer::PrepareAdvancedConstants(context, gameView, input);
            const auto requested = renderer::PrepareViewRenderPlan(resources, device, settings,
                gameView, shared, handles, true);
            renderer::BuildViewPipeline(gameView.pipeline, context, gameView, shared, {requested}, {});
            ASSERT_EQ(gameView.renderPlan.effectiveMode, renderer::RenderMode::PATH_TRACING);
            EXPECT_EQ(gameView.rayPath.constantsData.emitterCount, 2u);
            EXPECT_EQ(gameView.rayPath.constantsData.deltaLightCount, 1u);
            EXPECT_EQ(gameView.rayPath.constantsData.sampleBase, frame == 1 ? 1u : 0u);
            EXPECT_FLOAT_EQ(gameView.rayPath.constantsData.lightRadiance.x, 0);
            ASSERT_TRUE(gameView.pipeline.Execute(context));
            device.SetRenderTarget({}, resources);
            device.EndFrame();
        }
        ASSERT_TRUE(device.CaptureRenderTargetToLinearRGBA(gameView.hdr, resources, pixels, width, height));
        for (size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
            EXPECT_NEAR(pixels[pixel * 4], 0.5f, 0.001f);
            EXPECT_NEAR(pixels[pixel * 4 + 1], 1, 0.001f);
            EXPECT_NEAR(pixels[pixel * 4 + 2], 2, 0.001f);
        }
        EXPECT_EQ(sceneView.rayPath.history.GetSampleCount(), 0u);
        context.rayLights[1].unsupportedFlags = renderer::RAY_LIGHT_UNSUPPORTED_COOKIE;
        EXPECT_FALSE(renderer::PrepareRayPathView(context, gameView, shared));
        context.rayLights[1].unsupportedFlags = 0;
        EXPECT_TRUE(renderer::PrepareRayPathView(context, gameView, shared));
        const auto advancedConstants = gameView.advancedGraphicsCB;
        gameView.advancedGraphicsCB = {};
        input.weatherWetness = 0.5f;
        renderer::PrepareAdvancedConstants(context, gameView, input);
        EXPECT_FALSE(context.rayPathLightingSupported);
        EXPECT_FALSE(context.rayHitLightingSupported);
        gameView.advancedGraphicsCB = advancedConstants;
    }
    bundle.imguiRenderer.reset();
    device.Shutdown();
    EXPECT_EQ(diagnostics.failures, 0u);
}

TEST_F(RayRenderPlanTest, RayResourcesAndPathReadbackAreAvailableWithoutEngine)
{
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    ASSERT_TRUE(SUCCEEDED(com.result));
    struct DiagnosticSink : core::ILogSink {
        DiagnosticSink() { core::Logger::AddSink(this); }
        ~DiagnosticSink() { core::Logger::RemoveSink(this); }
        void OnLog(const core::LogEntry& entry) override {
            std::fprintf(stderr, "%s\n", entry.message.c_str());
            std::fflush(stderr);
        }
    } diagnostics;
    struct HiddenWindow {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Ray render plan standalone test", WS_POPUP,
                                      0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    } window;
    ASSERT_NE(window.handle, nullptr);
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, window.handle, 64, 64);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    /// @note Hidden-window verification records multiple frames even after DXGI occlusion.
    device.SetRenderWhenOccluded(true);
    const auto capabilities = device.GetCapabilities();
    EXPECT_TRUE(capabilities.bindless);
    EXPECT_TRUE(!capabilities.inlineRayQuery || capabilities.rayTracingPipeline);
    {
        renderer::ResourceManager resources(device);
        const auto shader = resources.LoadShader(FBZZ_GRAPHICS_SMOKE_SHADER);
        const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        EXPECT_TRUE(shader.IsValid());
        const auto outputA = resources.CreateRenderTarget(320, 180);
        const auto outputB = resources.CreateRenderTarget(640, 360);
        auto& rendering = resources.Rendering();
        auto& viewA = rendering.View(1);
        auto& viewB = rendering.View(2);
        renderer::RenderSettings settings;
        settings.renderScale = 1.0f;
        ASSERT_TRUE(rendering.PrepareView(viewA, device, outputA, settings));
        ASSERT_TRUE(rendering.PrepareView(viewB, device, outputB, settings));
        ASSERT_NE(resources.Get(viewA.gbuffer), nullptr);
        EXPECT_EQ(resources.Get(viewA.gbuffer)->GetColorCount(), renderer::GBUFFER_COLOR_COUNT);
        EXPECT_TRUE(resources.GetColorTexture(viewA.gbuffer, 2).IsValid());
        EXPECT_NE(viewA.hdr, viewB.hdr);
        EXPECT_NE(viewA.exposureResult, viewB.exposureResult);
        EXPECT_NE(viewA.advancedGraphicsCB, viewB.advancedGraphicsCB);
        EXPECT_EQ(rendering.FindView(99), nullptr);
        auto& shared = rendering.Shared();
        shared.frameCB = resources.CreateConstantBuffer(256);
        shared.objectCB = resources.CreateConstantBuffer(256);
        shared.lightCB = resources.CreateConstantBuffer(256);
        shared.postprocCB = resources.CreateConstantBuffer(256);
        shared.defaultPSO = state;
        shared.postprocPSO = state;
        shared.compositeShader = shader;
        renderer::RenderPassHandles planHandles;
        settings.modeRequest.mode = renderer::RenderMode::HYBRID;
        settings.modeRequest.rayReflection = true;
        const auto planA = renderer::PrepareViewRenderPlan(resources, device, settings, viewA, shared, planHandles);
        ASSERT_TRUE(planA.IsValid());
        EXPECT_EQ(planA.requestedMode, renderer::RenderMode::HYBRID);
        EXPECT_EQ(planA.effectiveMode, renderer::RenderMode::RASTER);
        EXPECT_FALSE(planA.NeedsRayScene());
        settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
        const auto planB = renderer::PrepareViewRenderPlan(resources, device, settings, viewB, shared, planHandles);
        ASSERT_TRUE(planB.IsValid());
        EXPECT_EQ(planB.requestedMode, renderer::RenderMode::PATH_TRACING);
        EXPECT_EQ(viewA.renderPlan.requestedMode, renderer::RenderMode::HYBRID);
        EXPECT_EQ(settings.modeRequest.mode, renderer::RenderMode::PATH_TRACING);
        const auto oldHdrA = viewA.hdr;
        const auto hdrB = viewB.hdr;
        const auto exposureA = viewA.exposureResult;
        const auto cbA = viewA.advancedGraphicsCB;
        viewA.taaHistoryValid = true;
        viewA.taaFrameIndex = 5;
        viewA.exposureResetGeneration = 7;
        viewA.prevViewProjection.m[0][0] = 2.0f;
        viewA.rayReflection.gpu.ready = true;
        viewA.rayReflectionCovered = true;
        ASSERT_TRUE(rendering.PrepareView(viewA, device, outputB, settings));
        EXPECT_FALSE(viewA.renderPlan.IsValid());
        EXPECT_FALSE(viewA.rayReflection.gpu.ready);
        EXPECT_FALSE(viewA.rayReflectionCovered);
        EXPECT_EQ(viewB.renderPlan.requestedMode, renderer::RenderMode::PATH_TRACING);
        EXPECT_EQ(resources.Get(oldHdrA), nullptr);
        EXPECT_NE(viewA.hdr, oldHdrA);
        EXPECT_EQ(viewB.hdr, hdrB);
        EXPECT_EQ(viewA.exposureResult, exposureA);
        EXPECT_EQ(viewA.advancedGraphicsCB, cbA);
        EXPECT_EQ(viewA.exposureResetGeneration, 7u);
        EXPECT_EQ(viewA.taaFrameIndex, 5u);
        EXPECT_FALSE(viewA.taaHistoryValid);
        EXPECT_FLOAT_EQ(viewA.prevViewProjection.m[0][0], 2.0f);

        renderer::Camera camera;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, device, resources, camera, settings, outputB, ~0u, handles};
        context.width = viewA.width;
        context.height = viewA.height;
        context.outputWidth = viewA.nativeWidth;
        context.outputHeight = viewA.nativeHeight;
        context.chainOutputRT = outputB;
        {
            const math::Vector3 vertices[] = {{-1, -1, 3}, {0, 1, 3}, {1, -1, 3}};
            const math::Vector4 table[] = {{1, 2, 3, 4}};
            const auto vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(math::Vector3));
            const auto gpuTable = resources.CreateStructuredBuffer(table, 1, sizeof(math::Vector4));
            ASSERT_TRUE(vertexBuffer.IsValid() && gpuTable.IsValid());
            renderer::ResourceHandle<renderer::AccelerationStructureTag> bottomLevel;
            renderer::ResourceHandle<renderer::AccelerationStructureTag> topLevel;
            if (capabilities.inlineRayQuery) {
                renderer::AccelerationStructureDesc bottomDescription;
                bottomDescription.geometries.push_back({vertexBuffer, {}, 0, 3});
                bottomLevel = resources.CreateAccelerationStructure(bottomDescription);
                renderer::AccelerationStructureDesc topDescription;
                topDescription.kind = renderer::AccelerationStructureKind::TOP_LEVEL;
                topDescription.instances.push_back({bottomLevel});
                topLevel = resources.CreateAccelerationStructure(topDescription);
                ASSERT_TRUE(bottomLevel.IsValid() && topLevel.IsValid());
            }

            renderer::RenderPipeline typedPipeline;
            renderer::RenderGraph::ResourceDesc bufferDescription;
            bufferDescription.byteSize = sizeof(vertices);
            bufferDescription.stride = sizeof(math::Vector3);
            bufferDescription.external = true;
            typedPipeline.DeclareBuffer("Vertices", vertexBuffer, bufferDescription);
            auto tableDescription = bufferDescription;
            tableDescription.byteSize = sizeof(table);
            tableDescription.stride = sizeof(math::Vector4);
            typedPipeline.DeclareStructuredBuffer("GpuTable", gpuTable, tableDescription);
            renderer::RenderGraph::ResourceDesc accelerationDescription;
            accelerationDescription.external = true;
            typedPipeline.DeclareAccelerationStructure("TLAS", topLevel, accelerationDescription);
            bool typedPassRan = false;
            typedPipeline.AddRawPass("InspectTypedResources", std::vector<renderer::RenderGraph::ResourceAccess>{
                {"Vertices", renderer::RenderGraph::ResourceUsage::Read, renderer::RenderGraph::ResourceAccessPurpose::BUILD_INPUT},
                {"GpuTable", renderer::RenderGraph::ResourceUsage::Read, renderer::RenderGraph::ResourceAccessPurpose::SHADER_READ},
                {"TLAS", renderer::RenderGraph::ResourceUsage::Read, renderer::RenderGraph::ResourceAccessPurpose::TRACE_READ}
            }, [&](renderer::PassResources& passResources) {
                typedPassRan = true;
                EXPECT_EQ(passResources.Buffer("Vertices"), vertexBuffer);
                EXPECT_EQ(passResources.StructuredBuffer("GpuTable"), gpuTable);
                EXPECT_EQ(passResources.AccelerationStructure("TLAS"), topLevel);
            }, false);

            /// @note このパスは GPU work を記録せず登録の経路だけを確認する。AS build / trace は RayTracingTests が担う。
            resources.AdvanceFrame();
            device.BeginFrame();
            const bool typedPassSucceeded = typedPipeline.Execute(context);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            EXPECT_TRUE(typedPassSucceeded && typedPassRan);
            EXPECT_EQ(context.resourceRegistry.Buffer("Vertices"), vertexBuffer);
            EXPECT_EQ(context.resourceRegistry.StructuredBuffer("GpuTable"), gpuTable);
            EXPECT_EQ(context.resourceRegistry.AccelerationStructure("TLAS"), topLevel);
            const auto& lifetimes = typedPipeline.LastReport().lifetimes;
            const auto declaredVertices = std::find_if(lifetimes.begin(), lifetimes.end(),
                [](const renderer::RenderGraph::ResourceLifetime& lifetime) { return lifetime.name == "Vertices"; });
            ASSERT_NE(declaredVertices, lifetimes.end());
            EXPECT_EQ(declaredVertices->desc.byteSize, sizeof(vertices));
            EXPECT_EQ(declaredVertices->desc.stride, sizeof(math::Vector3));
            typedPipeline.ReleaseViewResources(resources);
            resources.Release(topLevel);
            resources.Release(bottomLevel);
            resources.Release(gpuTable);
            resources.Release(vertexBuffer);
        }
        std::vector<std::string> stages;
        renderer::ViewPipelineExtensions extensions;
        extensions.begin = [&]() { stages.push_back("begin"); };
        extensions.setup = [&]() { stages.push_back("setup"); };
        extensions.userPasses = [&](renderer::UserRenderPassInjectionPoint) { stages.push_back("user"); };
        extensions.depthDebug = [&]() { stages.push_back("depth"); };
        extensions.overlayDebug = [&](const char*) { stages.push_back("overlay"); };
        extensions.ui = [&]() { stages.push_back("ui"); };
        if (capabilities.inlineRayQuery) {
            auto pathScene = std::make_shared<renderer::RenderScene>();
            pathScene->sceneGeneration = 123;
            context.renderScene = pathScene;
            context.experimentalRayTracingEnabled = false;
            context.width = viewA.width;
            context.height = viewA.height;
            context.rayLightsComplete = true;
            context.rayPathLightingSupported = true;
            context.rayHitLightingSupported = true;
            camera.m_clearMode = renderer::CameraClearMode::SolidColor;
            settings.ibl.enabled = false;
            settings.postProcess.fog.enabled = false;
            settings.froxelFog.enabled = false;
            settings.volumetricLight.enabled = false;
            settings.modeRequest.pathProfile = renderer::PathTracingProfile::REFERENCE;
            settings.viewMode = renderer::ViewMode::RayHitDistance;
            EXPECT_FALSE(renderer::PrepareRayDebugView(context, viewA, shared));
            settings.viewMode = renderer::ViewMode::Lit;
            settings.modeRequest.mode = renderer::RenderMode::HYBRID;
            settings.modeRequest.rayReflection = true;
            EXPECT_FALSE(renderer::PrepareRayReflectionView(context, viewA, shared,
                {renderer::OpaqueRenderPath::DEFERRED}));
            EXPECT_FALSE(renderer::PrepareRayReflectionReconstruction(context, viewA, shared));
            settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
            EXPECT_FALSE(renderer::PrepareRayPathView(context, viewA, shared));
            EXPECT_EQ(shared.rayGeometry.BottomLevelCount(), 0u);
            EXPECT_FALSE(viewA.rayDebug.output.IsValid());
            EXPECT_FALSE(viewA.rayDebug.constants.IsValid());
            EXPECT_FALSE(viewA.rayReflection.output.IsValid());
            EXPECT_FALSE(viewA.rayPath.historyBuffer.IsValid());
            EXPECT_FALSE(shared.rayDebugShader.IsValid());
            EXPECT_FALSE(shared.rayReflectionShader.IsValid());
            EXPECT_FALSE(shared.rayReflectionReconstructionShader.IsValid());
            EXPECT_FALSE(shared.rayPathShader.IsValid());
            rendering.SetExperimentalRayTracingEnabled(true);
            context.experimentalRayTracingEnabled = true;
            settings.renderScale = 1.0f;
            settings.postProcess.bloom.enabled = false;
            settings.autoExposure.enabled = false;
            settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
            settings.modeRequest.pathProfile = renderer::PathTracingProfile::REFERENCE;
            ASSERT_TRUE(rendering.PrepareView(viewA, device, outputA, settings));
            context.width = viewA.width;
            context.height = viewA.height;
            context.outputRT = outputA;
            context.outputWidth = viewA.nativeWidth;
            context.outputHeight = viewA.nativeHeight;
            context.lightData.lightDir = {0, -1, 0};
            context.lightData.lightIntensity = 0;
            camera.m_backgroundColor = {1, 2, 3, 1};
            shared.rayPathShader = resources.LoadShader(std::string(FBZZ_GRAPHICS_SHADER_ROOT) + "/RayTracing/RayPathTrace.cs.hlsl");
            shared.rayPathResolveShader = resources.LoadShader(std::string(FBZZ_GRAPHICS_SHADER_ROOT) + "/RayTracing/RayPathResolve.hlsl");
            shared.copyColorShader = resources.LoadShader(std::string(FBZZ_GRAPHICS_SHADER_ROOT) + "/PostProcess/Color/CopyColor.hlsl");
            shared.compositeShader = shared.copyColorShader;
            resources.Release(shared.postprocCB);
            shared.postprocCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
            ASSERT_TRUE(shared.rayPathShader.IsValid() && shared.rayPathResolveShader.IsValid());
            ASSERT_TRUE(shared.copyColorShader.IsValid());
            stages.clear();
            const auto initialPathPlan = renderer::PrepareViewRenderPlan(resources, device, settings, viewA, shared, handles, true);
            ASSERT_TRUE(initialPathPlan.IsValid());
            renderer::BuildViewPipeline(viewA.pipeline, context, viewA, shared, {initialPathPlan}, extensions);
            ASSERT_EQ(viewA.renderPlan.effectiveMode, renderer::RenderMode::PATH_TRACING);
            EXPECT_EQ(stages, (std::vector<std::string>{"begin", "overlay", "ui"}));
            const auto pathNames = viewA.pipeline.RegisteredPassNames();
            for (const char* name : {"RayPathTrace", "RayPathColor", "RayPathResolve", "Composite"})
                EXPECT_NE(std::find(pathNames.begin(), pathNames.end(), name), pathNames.end());
            for (const char* name : {"DeferredLighting", "ForwardOpaque", "Shadow", "Sky", "SSR", "TAA", "GTAO"})
                EXPECT_EQ(std::find(pathNames.begin(), pathNames.end(), name), pathNames.end());
            /// @note 深度 0 の背景でも Color が cull されず、前フレームの HDR を全画素で置換することを実行で確認する。
            resources.AdvanceFrame();
            device.BeginFrame();
            device.SetRenderTarget(viewA.hdr, resources);
            device.Clear({9, 0, 0, 1});
            EXPECT_TRUE(viewA.pipeline.Execute(context));
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            const auto& pathProfiles = viewA.pipeline.LastReport().profiles;
            const auto colorPass = std::find_if(pathProfiles.begin(), pathProfiles.end(),
                [](const renderer::RenderGraph::PassProfile& profile) { return profile.name == "RayPathColor"; });
            const auto resolvePass = std::find_if(pathProfiles.begin(), pathProfiles.end(),
                [](const renderer::RenderGraph::PassProfile& profile) { return profile.name == "RayPathResolve"; });
            ASSERT_NE(colorPass, pathProfiles.end());
            ASSERT_NE(resolvePass, pathProfiles.end());
            EXPECT_LT(colorPass, resolvePass);
            std::vector<float> pathPixels;
            uint32_t pathWidth = 0, pathHeight = 0;
            ASSERT_TRUE(device.CaptureRenderTargetToLinearRGBA(viewA.hdr, resources, pathPixels, pathWidth, pathHeight));
            ASSERT_EQ(pathPixels.size(), static_cast<size_t>(pathWidth) * pathHeight * 4);
            ASSERT_GT(pathPixels.size(), 0u);
            const size_t centerPixel = static_cast<size_t>(pathHeight / 2) * pathWidth + pathWidth / 2;
            for (const size_t pixel : {size_t{0}, centerPixel, pathPixels.size() / 4 - 1}) {
                EXPECT_NEAR(pathPixels[pixel * 4], 1, 0.001f);
                EXPECT_NEAR(pathPixels[pixel * 4 + 1], 2, 0.001f);
                EXPECT_NEAR(pathPixels[pixel * 4 + 2], 3, 0.001f);
            }
            EXPECT_EQ(viewA.rayPath.history.GetSampleCount(), 1u);
            ASSERT_TRUE(viewA.rayPath.history.Commit(1));
            pathScene->snapshotSerial += 1;
            settings.postProcess.exposure += 1.0f;
            ASSERT_TRUE(renderer::PrepareRayPathView(context, viewA, shared));
            EXPECT_EQ(viewA.rayPath.history.GetSampleCount(), 2u);
            EXPECT_EQ(viewA.rayPath.constantsData.resetHistory, 0u);
            camera.m_position.x += 1.0f;
            ASSERT_TRUE(renderer::PrepareRayPathView(context, viewA, shared));
            EXPECT_EQ(viewA.rayPath.history.GetSampleCount(), 0u);
            EXPECT_EQ(viewA.rayPath.constantsData.resetHistory, 1u);
            ASSERT_TRUE(rendering.PrepareView(viewA, device, outputA, settings));
            EXPECT_FALSE(viewA.rayPath.gpu.ready);
            EXPECT_FALSE(viewA.rayPathCovered);
            EXPECT_EQ(renderer::PrepareViewRenderPlan(resources, device, settings, viewA, shared, handles, true).effectiveMode,
                renderer::RenderMode::RASTER);
            viewA.pipeline.BeginBuild();
            const auto rasterHdr = viewA.hdr;
            const auto pathAccumulation = viewA.rayPath.historyBuffer;
            const auto pathConstants = viewA.rayPath.constants;
            const auto pathShader = shared.rayPathShader;
            const auto pathResolveShader = shared.rayPathResolveShader;
            ASSERT_TRUE(pathAccumulation.IsValid() && pathConstants.IsValid());
            rendering.SetExperimentalRayTracingEnabled(false);
            context.experimentalRayTracingEnabled = false;
            EXPECT_EQ(resources.Get(pathAccumulation), nullptr);
            EXPECT_EQ(resources.Get(pathConstants), nullptr);
            EXPECT_EQ(resources.Get(pathShader), nullptr);
            EXPECT_EQ(resources.Get(pathResolveShader), nullptr);
            EXPECT_FALSE(viewA.rayPath.historyBuffer.IsValid());
            EXPECT_FALSE(shared.rayPathShader.IsValid());
            EXPECT_FALSE(shared.rayPathResolveShader.IsValid());
            EXPECT_EQ(viewA.hdr, rasterHdr);
            EXPECT_NE(resources.Get(rasterHdr), nullptr);
            const auto disabledPlan = renderer::PrepareViewRenderPlan(resources, device, settings,
                viewA, shared, handles);
            EXPECT_TRUE(disabledPlan.IsValid());
            EXPECT_EQ(disabledPlan.fallbackReason, renderer::RenderPlanReason::DEVELOPER_MODE_REQUIRED);
            EXPECT_EQ(disabledPlan.effectiveMode, renderer::RenderMode::RASTER);
            EXPECT_EQ(settings.modeRequest.mode, renderer::RenderMode::PATH_TRACING);
        }

        const auto releasedHdr = viewA.hdr;
        resources.ReleaseRenderView(1);
        EXPECT_EQ(resources.Get(releasedHdr), nullptr);
        EXPECT_EQ(resources.Get(exposureA), nullptr);
        EXPECT_EQ(resources.Get(cbA), nullptr);
        EXPECT_NE(resources.Get(hdrB), nullptr);
        resources.Release(outputB);
        EXPECT_EQ(resources.Get(hdrB), nullptr);
        EXPECT_FALSE(rendering.View(2).hdr.IsValid());

        renderer::ResourceManager otherResources(device);
        EXPECT_NE(&resources.Rendering(), &otherResources.Rendering());
        resources.Rendering().View(3).taaHistoryValid = true;
        resources.Reset();
        EXPECT_FALSE(resources.Rendering().View(3).taaHistoryValid);
        EXPECT_FALSE(resources.Rendering().View(3).hdr.IsValid());
    }
    bundle.imguiRenderer.reset();
    device.Shutdown();
    const auto shutdownCapabilities = device.GetCapabilities();
    EXPECT_FALSE(shutdownCapabilities.bindless);
    EXPECT_FALSE(shutdownCapabilities.inlineRayQuery);
    EXPECT_FALSE(shutdownCapabilities.rayTracingPipeline);
}
}
}
