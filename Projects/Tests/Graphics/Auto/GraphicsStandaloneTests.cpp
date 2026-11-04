/// @file    GraphicsStandaloneTests.cpp
/// @brief   Engine をリンクせず Graphics のパイプラインと GPU 描画を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/BuiltinShader.hpp>
#include <Graphics/Renderer/Material.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Math/Matrix4.hpp>
#include <Graphics/Pipeline/EnvironmentResources.hpp>
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/RenderPassCapture.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/RuntimePackageValidation.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Core/ILogSink.hpp>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
class GraphicsStandaloneTest : public testkit::Fixture {};

TEST_F(GraphicsStandaloneTest, EmbeddedErrorShadersDrawWithoutExternalFilesAndRespectSkinning)
{
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    ASSERT_TRUE(SUCCEEDED(com.result));
    struct HiddenWindow {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Embedded error shaders", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    } window;
    ASSERT_NE(window.handle, nullptr);
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, window.handle, 32, 32);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    struct RejectExternalShaders {
        RejectExternalShaders() {
            renderer::SetShaderPathResolver([](const std::filesystem::path&) {
                return std::filesystem::path{};
            });
        }
        ~RejectExternalShaders() { renderer::SetShaderPathResolver(nullptr); }
    } rejectExternalShaders;
    {
        renderer::ResourceManager resources(device);
        const auto output = resources.CreateRenderTarget(16, 16);
        const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        const std::array<math::Matrix4, 3> camera{
            math::Matrix4::Identity(), math::Matrix4::Identity(), math::Matrix4::Identity()};
        const auto cameraBuffer = resources.CreateConstantBuffer(sizeof(camera));
        const auto world = math::Matrix4::Identity();
        const auto objectBuffer = resources.CreateConstantBuffer(sizeof(world));
        const auto paletteBuffer = resources.CreateConstantBuffer(sizeof(math::Matrix4) * 128);
        const std::array<math::Vector3, 3> positions{{{-1, -1, 0.5f}, {-1, 3, 0.5f}, {3, -1, 0.5f}}};
        std::array<renderer::Vertex, 3> vertices{};
        std::array<renderer::SkinnedVertex, 3> skinned{};
        for (size_t i = 0; i < vertices.size(); ++i) {
            vertices[i].position = positions[i];
            skinned[i].position = positions[i];
            skinned[i].boneWeights[0] = 1;
        }
        const auto staticBuffer = resources.CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
        const auto skinnedBuffer = resources.CreateVertexBuffer(skinned.data(), sizeof(skinned), sizeof(renderer::SkinnedVertex));
        const uint32_t indices[] = {0, 1, 2};
        const auto indexBuffer = resources.CreateIndexBuffer(indices, 3);
        ASSERT_TRUE(output.IsValid() && state.IsValid() && cameraBuffer.IsValid() && objectBuffer.IsValid()
            && paletteBuffer.IsValid() && staticBuffer.IsValid() && skinnedBuffer.IsValid() && indexBuffer.IsValid());
        for (uint32_t variant = 0; variant < 3; ++variant) {
            SCOPED_TRACE(variant);
            const auto* material = resources.GetErrorMaterial(variant != 0);
            ASSERT_NE(material, nullptr);
            EXPECT_FALSE(resources.GetShaderCapabilities(material->shader).SupportsGBuffer());
            std::array<math::Matrix4, 128> palette;
            palette.fill(math::Matrix4::Identity());
            /// @note 変形後の全頂点を画面外へ送り、スキニングを無視した診断 shader を検出する。
            if (variant == 2) palette[0].m[0][3] = 4;
            resources.AdvanceFrame();
            device.BeginFrame();
            device.SetRenderTarget(output, resources);
            device.Clear({0, 0, 0, 1});
            resources.Update(cameraBuffer, camera.data(), sizeof(camera));
            resources.Update(objectBuffer, &world, sizeof(world));
            resources.Update(paletteBuffer, palette.data(), sizeof(palette));
            renderer::DrawCall draw;
            draw.vertexBuffer = variant == 0 ? staticBuffer : skinnedBuffer;
            draw.indexBuffer = indexBuffer;
            draw.indexCount = 3;
            draw.vertexCount = 3;
            draw.shader = material->shader;
            draw.pipelineState = state;
            draw.constantBuffers[0] = cameraBuffer;
            draw.constantBuffers[1] = objectBuffer;
            draw.constantBuffers[7] = paletteBuffer;
            device.Submit(draw, resources);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            ASSERT_TRUE(device.CaptureRenderTargetToLinearRGBA(output, resources, pixels, width, height));
            ASSERT_EQ(width, 16u);
            ASSERT_EQ(height, 16u);
            const size_t center = (8u * width + 8u) * 4u;
            EXPECT_NEAR(pixels[center], variant == 2 ? 0.0f : 1.0f, 0.001f);
            EXPECT_NEAR(pixels[center + 1], 0.0f, 0.001f);
            EXPECT_NEAR(pixels[center + 2], variant == 2 ? 0.0f : 1.0f, 0.001f);
        }
        resources.Reset();
    }
    device.Shutdown();
}

TEST_F(GraphicsStandaloneTest, GpuProfilerKeepsPhysicalSubmissionAndViewPlanProvenance)
{
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    ASSERT_TRUE(SUCCEEDED(com.result));
    struct HiddenWindow {
        HWND handle = CreateWindowExW(0, L"STATIC", L"GPU profiler provenance", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    } window;
    ASSERT_NE(window.handle, nullptr);
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, window.handle, 32, 32);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        const auto output = resources.CreateRenderTarget(16, 16);
        ASSERT_TRUE(output.IsValid());
        renderer::Camera camera;
        renderer::RenderSettings settings;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, device, resources, camera, settings, output, ~0u, handles};
        renderer::RenderPipeline first, second;
        renderer::RenderPassCapture capture;
        renderer::ResolvedRenderPlan plan;
        plan.failureReason = renderer::RenderPlanReason::NONE;
        uint64_t previousPhysicalSerial = 0;
        uint64_t firstGeneration = 0;
        for (uint32_t frame = 0; frame < 4; ++frame) {
            SCOPED_TRACE(frame);
            resources.AdvanceFrame();
            device.BeginFrame();
            first.BeginBuild();
            second.BeginBuild();
            for (auto* pipeline : {&first, &second}) {
                pipeline->AddRawPass("SameName", std::vector<renderer::RenderGraph::ResourceAccess>{}, [&] {
                    device.SetRenderTarget(output, resources);
                    device.Clear({0.2f, 0.3f, 0.4f, 1.0f});
                }, false);
                pipeline->SetGpuProfilerHooks(
                    [&](std::string_view name) { device.GpuProfBeginPass(name.data()); },
                    [&](std::string_view name) { device.GpuProfEndPass(name.data()); });
            }
            if (frame >= 3) {
                first.AddRawPass("Extra", std::vector<renderer::RenderGraph::ResourceAccess>{}, [] {}, false);
            }
            if (frame >= 2)
                plan.reflection.fallbackReason = renderer::RenderPlanReason::RAY_COVERAGE_INCOMPLETE;
            renderer::GpuProfilerViewMetadata view;
            view.applicationFrameSerial = resources.FrameStamp();
            view.sceneGeneration = 3;
            view.resourceEpoch = resources.GetResetVersion();
            view.outputId = output.id;
            view.outputGeneration = output.gen;
            view.width = 16;
            view.height = 16;
            view.viewId = 1;
            ASSERT_TRUE(first.Execute(context, &capture, &view, &plan));
            const auto firstSource = first.LastGpuProfilerView();
            if (frame == 0) firstGeneration = firstSource.planGeneration;
            if (frame == 1) EXPECT_EQ(firstSource.planGeneration, firstGeneration);
            if (frame == 2) EXPECT_GT(firstSource.planGeneration, firstGeneration);
            if (frame == 3) EXPECT_GT(firstSource.planGeneration, firstGeneration + 1);
            /// @note Legacy view calls and a second view must not reset or publish the recording query range.
            device.GpuProfEndFrame();
            device.GpuProfCollect();
            device.GpuProfBeginFrame();
            for (const auto& pass : device.GpuProfGetSnapshot().passes)
                EXPECT_LT(pass.metadata.applicationFrameSerial, resources.FrameStamp());
            view.viewId = 2;
            ASSERT_TRUE(second.Execute(context, nullptr, &view, &plan));
            const auto secondSource = second.LastGpuProfilerView();
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            /// @note Readback verifies correctness and completes submission; no timing value or performance threshold is asserted.
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            ASSERT_TRUE(device.CaptureRenderTargetToLinearRGBA(output, resources, pixels, width, height));
            device.GpuProfCollect();
            const auto& snapshot = device.GpuProfGetSnapshot();
            ASSERT_TRUE(snapshot.supported && snapshot.available && snapshot.complete);
            ASSERT_GT(snapshot.physicalFrameSerial, previousPhysicalSerial);
            previousPhysicalSerial = snapshot.physicalFrameSerial;
            ASSERT_EQ(snapshot.passes.size(), frame >= 3 ? 3u : 2u);
            EXPECT_EQ(snapshot.passes.front().metadata, firstSource);
            EXPECT_EQ(snapshot.passes.back().metadata, secondSource);
            EXPECT_EQ(snapshot.passes.front().name, "SameName");
            EXPECT_EQ(snapshot.passes.back().name, "SameName");
            for (const auto& pass : snapshot.passes) {
                EXPECT_TRUE(pass.available);
                EXPECT_EQ(pass.physicalFrameSerial, snapshot.physicalFrameSerial);
                EXPECT_EQ(pass.deviceEpoch, snapshot.deviceEpoch);
            }
            EXPECT_FALSE(snapshot.totalGpuTimeAvailable);
            EXPECT_FALSE(snapshot.preparationGpuTimeAvailable);
            EXPECT_FALSE(snapshot.perQueueGpuTimeAvailable);
            EXPECT_EQ(device.GpuProfGetResults().size(), snapshot.passes.size());
        }
        first.BeginBuild();
        first.AddRawPass("CycleA", {"B"}, {"A"}, [] {}, false);
        first.AddRawPass("CycleB", {"A"}, {"B"}, [] {}, false);
        renderer::GpuProfilerViewMetadata failedView;
        failedView.width = failedView.height = 16;
        EXPECT_FALSE(first.Execute(context, &capture, &failedView, &plan));
        EXPECT_EQ(first.LastGpuProfilerView().planGeneration, 0u);
        EXPECT_TRUE(capture.Passes().empty());
        resources.Release(output);
        resources.Reset();
    }
    device.Shutdown();
}

TEST_F(GraphicsStandaloneTest, CubeFacesKeepDepthAcrossRebindsAndClearEachSelectedMip)
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
            /// @note The ordinary readback target lacks optimized-clear metadata; id820 is not a correctness failure.
            if (entry.message.find("  [WARNING] (id=820) ID3D12CommandList::ClearRenderTargetView: The application did not pass any clear value")
                != std::string::npos) return;
            ++failures;
            std::fprintf(stderr, "%s\n", entry.message.c_str());
            std::fflush(stderr);
        }
    } diagnostics;
    struct HiddenWindow {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Cube depth regression", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    } window;
    ASSERT_NE(window.handle, nullptr);
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, window.handle, 32, 32);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        const auto cube = resources.CreateCubemapRenderTarget(16, 2);
        const auto cubeTexture = resources.GetCubemapTexture(cube);
        const auto output = resources.CreateRenderTarget(16, 16);
        const std::filesystem::path shaderRoot = FBZZ_GRAPHICS_SHADER_ROOT;
        const auto shader = resources.LoadShader(
            (shaderRoot / "../../Projects/Tests/Graphics/Shaders/CubeDepth.hlsl").generic_string());
        const uint32_t indices[] = {0, 1, 2};
        const auto indexBuffer = resources.CreateIndexBuffer(indices, 3);
        const auto constants = resources.CreateConstantBuffer(sizeof(math::Vector4) * 2);
        const auto depthOn = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        const auto depthOff = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(cube.IsValid() && cubeTexture.IsValid() && output.IsValid() && shader.IsValid()
            && indexBuffer.IsValid() && constants.IsValid() && depthOn.IsValid() && depthOff.IsValid());
        renderer::DrawCall draw;
        draw.shader = shader;
        draw.indexBuffer = indexBuffer;
        draw.indexCount = 3;
        draw.constantBuffers[0] = constants;
        const auto submitColor = [&](const math::Vector3& color, float depth) {
            const std::array<math::Vector4, 2> data{{{color.x, color.y, color.z, depth}, {}}};
            resources.Update(constants, data.data(), sizeof(data));
            device.Submit(draw, resources);
        };
        resources.AdvanceFrame();
        device.BeginFrame();
        draw.pipelineState = depthOn;
        for (uint32_t mip = 0; mip < 2; ++mip) {
            for (uint32_t face = 0; face < 6; ++face) {
                device.SetRenderTargetFace(cube, face, mip, resources);
                device.Clear({});
                /// @note Increasing depth makes uncleared shared depth reject the next face's geometry.
                const float nearDepth = 0.1f + 0.1f * static_cast<float>(face);
                const float farDepth = nearDepth + 0.05f;
                submitColor({1, 0, 0}, nearDepth);
                if (face == 0 && mip == 0) {
                    static_cast<void>(device.BeginAsyncCompute(resources));
                    device.EndAsyncCompute();
                }
                /// @note Returning from a preview or command-list split must preserve depth, not implicitly clear it.
                device.SetRenderTarget(output, resources);
                device.SetRenderTargetFace(cube, face, mip, resources);
                submitColor({0, 1, 0}, farDepth);
                if ((face & 1u) != 0) {
                    device.ClearDepth();
                    submitColor({0, 0, 1}, farDepth);
                }
                if (face == 5) {
                    draw.pipelineState = depthOff;
                    submitColor({1, 1, 0}, 0.95f);
                    draw.pipelineState = depthOn;
                    submitColor({1, 1, 1}, 0.9f);
                }
            }
        }
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        const std::array<math::Vector3, 6> directions{{
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
        draw.pipelineState = depthOff;
        draw.textures[0] = cubeTexture;
        for (uint32_t mip = 0; mip < 2; ++mip) {
            for (uint32_t face = 0; face < 6; ++face) {
                SCOPED_TRACE("face=" + std::to_string(face) + " mip=" + std::to_string(mip));
                const auto& direction = directions[face];
                const std::array<math::Vector4, 2> data{{{},
                    {direction.x, direction.y, direction.z, static_cast<float>(mip)}}};
                resources.AdvanceFrame();
                device.BeginFrame();
                device.SetRenderTarget(output, resources);
                device.Clear({});
                resources.Update(constants, data.data(), sizeof(data));
                device.Submit(draw, resources);
                device.SetRenderTarget({}, resources);
                device.EndFrame();
                std::vector<float> pixels;
                uint32_t width = 0, height = 0;
                ASSERT_TRUE(device.CaptureRenderTargetToLinearRGBA(output, resources, pixels, width, height));
                ASSERT_EQ(pixels.size(), static_cast<size_t>(width) * height * 4);
                ASSERT_GT(pixels.size(), 0u);
                const size_t center = (static_cast<size_t>(height / 2) * width + width / 2) * 4;
                const math::Vector3 expected = face == 5 ? math::Vector3{1, 1, 0}
                    : (face & 1u) ? math::Vector3{0, 0, 1} : math::Vector3{1, 0, 0};
                EXPECT_NEAR(pixels[center], expected.x, 0.001f);
                EXPECT_NEAR(pixels[center + 1], expected.y, 0.001f);
                EXPECT_NEAR(pixels[center + 2], expected.z, 0.001f);
            }
        }
    }
    bundle.imguiRenderer.reset();
    device.Shutdown();
    EXPECT_EQ(diagnostics.failures, 0u);
}

TEST_F(GraphicsStandaloneTest, DrawsAndReadsBackWithoutEngine)
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
        HWND handle = CreateWindowExW(0, L"STATIC", L"Graphics standalone test", WS_POPUP,
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
    {
        renderer::ResourceManager resources(device);
        const auto shader = resources.LoadShader(FBZZ_GRAPHICS_SMOKE_SHADER);
        const auto target = resources.CreateRenderTarget(64, 64);
        const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        EXPECT_TRUE(shader.IsValid());
        EXPECT_TRUE(target.IsValid());
        if (shader.IsValid() && target.IsValid() && state.IsValid()) {
            resources.AdvanceFrame();
            device.BeginFrame();
            device.SetRenderTarget(target, resources);
            device.Clear({0, 0, 0, 1});
            renderer::DrawCall draw;
            draw.shader = shader;
            draw.pipelineState = state;
            draw.vertexCount = 3;
            device.Submit(draw, resources);
            FBZZ_LOG_INFO("GraphicsStandalone: draw submitted");
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            FBZZ_LOG_INFO("GraphicsStandalone: frame ended");
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            const bool captured = device.CaptureRenderTargetToLinearRGBA(target, resources, pixels, width, height);
            EXPECT_TRUE(captured);
            if (captured && width == 64 && height == 64 && pixels.size() == 64 * 64 * 4) {
                EXPECT_GT(pixels[(32 * 64 + 32) * 4], 0.9f);
                EXPECT_LT(pixels[(32 * 64 + 32) * 4 + 1], 0.1f);
                EXPECT_LT(pixels[0], 0.1f);
            } else {
                ADD_FAILURE() << "Unexpected readback dimensions: " << width << "x" << height;
            }
        }
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
        /// @note 構成の準備に必要な実 GPU 資源。ここではパス登録を検証し、シーンの描画は実行しない。
        auto& shared = rendering.Shared();
        shared.frameCB = resources.CreateConstantBuffer(256);
        shared.objectCB = resources.CreateConstantBuffer(256);
        shared.lightCB = resources.CreateConstantBuffer(256);
        shared.postprocCB = resources.CreateConstantBuffer(256);
        shared.defaultPSO = state;
        shared.postprocPSO = state;
        shared.compositeShader = shader;
        renderer::RenderPassHandles planHandles;
        const auto planA = renderer::PrepareViewRenderPlan(resources, device, settings, viewA, shared, planHandles);
        ASSERT_TRUE(planA.IsValid());
        EXPECT_EQ(planA.requestedMode, renderer::RenderMode::RASTER);
        EXPECT_EQ(planA.effectiveMode, renderer::RenderMode::RASTER);
        const auto planB = renderer::PrepareViewRenderPlan(resources, device, settings, viewB, shared, planHandles);
        ASSERT_TRUE(planB.IsValid());
        EXPECT_EQ(planB.requestedMode, renderer::RenderMode::RASTER);
        EXPECT_EQ(viewA.renderPlan.requestedMode, renderer::RenderMode::RASTER);
        EXPECT_EQ(settings.modeRequest.mode, renderer::RenderMode::RASTER);
        const auto oldHdrA = viewA.hdr;
        const auto hdrB = viewB.hdr;
        const auto exposureA = viewA.exposureResult;
        const auto cbA = viewA.advancedGraphicsCB;
        viewA.taaHistoryValid = true;
        viewA.taaFrameIndex = 5;
        viewA.exposureResetGeneration = 7;
        viewA.prevViewProjection.m[0][0] = 2.0f;
        auto resizeSettings = settings;
        resizeSettings.autoExposure.enabled = true;
        ASSERT_TRUE(rendering.PrepareView(viewA, device, outputB, resizeSettings));
        EXPECT_FALSE(viewA.renderPlan.IsValid());
        EXPECT_EQ(viewB.renderPlan.requestedMode, renderer::RenderMode::RASTER);
        EXPECT_EQ(resources.Get(oldHdrA), nullptr);
        EXPECT_NE(viewA.hdr, oldHdrA);
        EXPECT_EQ(viewB.hdr, hdrB);
        EXPECT_EQ(viewA.exposureResult, exposureA);
        EXPECT_EQ(viewA.advancedGraphicsCB, cbA);
        EXPECT_EQ(viewA.exposureResetGeneration, 7u);
        EXPECT_EQ(viewA.taaFrameIndex, 5u);
        EXPECT_FALSE(viewA.taaHistoryValid);
        EXPECT_FLOAT_EQ(viewA.prevViewProjection.m[0][0], 2.0f);

        /// @note ホスト拡張の挿入順と露出・ポスト処理の順序は移設で変えない。
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
            bool typedPassRan = false;
            typedPipeline.AddRawPass("InspectTypedResources", std::vector<renderer::RenderGraph::ResourceAccess>{
                {"Vertices", renderer::RenderGraph::ResourceUsage::Read, renderer::RenderGraph::ResourceAccessPurpose::BUILD_INPUT},
                {"GpuTable", renderer::RenderGraph::ResourceUsage::Read, renderer::RenderGraph::ResourceAccessPurpose::SHADER_READ}
            }, [&](renderer::PassResources& passResources) {
                typedPassRan = true;
                EXPECT_EQ(passResources.Buffer("Vertices"), vertexBuffer);
                EXPECT_EQ(passResources.StructuredBuffer("GpuTable"), gpuTable);
            }, false);

            /// @note このパスは GPU work を記録せず buffer と structured buffer の登録経路を確認する。
            resources.AdvanceFrame();
            device.BeginFrame();
            const bool typedPassSucceeded = typedPipeline.Execute(context);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            EXPECT_TRUE(typedPassSucceeded && typedPassRan);
            EXPECT_EQ(context.resourceRegistry.Buffer("Vertices"), vertexBuffer);
            EXPECT_EQ(context.resourceRegistry.StructuredBuffer("GpuTable"), gpuTable);
            const auto& lifetimes = typedPipeline.LastReport().lifetimes;
            const auto declaredVertices = std::find_if(lifetimes.begin(), lifetimes.end(),
                [](const renderer::RenderGraph::ResourceLifetime& lifetime) { return lifetime.name == "Vertices"; });
            ASSERT_NE(declaredVertices, lifetimes.end());
            EXPECT_EQ(declaredVertices->desc.byteSize, sizeof(vertices));
            EXPECT_EQ(declaredVertices->desc.stride, sizeof(math::Vector3));
            typedPipeline.ReleaseViewResources(resources);
            resources.Release(gpuTable);
            resources.Release(vertexBuffer);
        }
        std::vector<std::string> stages;
        renderer::ViewPipelineExtensions extensions;
        extensions.begin = [&]() { stages.push_back("begin"); };
        extensions.setup = [&]() { stages.push_back("setup"); };
        extensions.userPasses = [&](renderer::UserRenderPassInjectionPoint point) {
            stages.push_back("user");
            if (point == renderer::UserRenderPassInjectionPoint::AfterOpaque)
                viewA.pipeline.AddRawPass("UserAfterOpaque", {"HDR"}, {"HDR"}, []() {});
            if (point == renderer::UserRenderPassInjectionPoint::AfterTransparent)
                viewA.pipeline.AddRawPass("UserAfterTransparent", {"HDR"}, {"HDR"}, []() {});
        };
        extensions.depthDebug = [&]() { stages.push_back("depth"); };
        extensions.overlayDebug = [&](const char*) { stages.push_back("overlay"); };
        extensions.ui = [&]() { stages.push_back("ui"); };
        const auto preparedPlan = renderer::PrepareViewRenderPlan(
            resources, device, settings, viewA, shared, planHandles);
        ASSERT_TRUE(preparedPlan.IsValid());
        renderer::BuildViewPipeline(viewA.pipeline, context, viewA, shared, { preparedPlan }, extensions);
        EXPECT_EQ(stages, (std::vector<std::string>{"begin", "setup", "user", "user", "depth", "user", "overlay", "ui"}));
        const auto names = viewA.pipeline.RegisteredPassNames();
        const auto position = [&](const char* name) { return std::find(names.begin(), names.end(), name); };
        const char* transparent = preparedPlan.rasterPlan.UsesDeferredLighting() ? "DeferredForwardTransparent" : "ForwardTransparent";
        EXPECT_NE(position("UserAfterOpaque"), names.end());
        EXPECT_NE(position(transparent), names.end());
        EXPECT_NE(position("WaterForward"), names.end());
        EXPECT_LT(position("UserAfterOpaque"), position(transparent));
        EXPECT_LT(position(transparent), position("WaterForward"));
        EXPECT_LT(position("WaterForward"), position("UserAfterTransparent"));
        EXPECT_LT(position("UserAfterTransparent"), position("Composite"));
        EXPECT_LT(position("AutoExposure"), position("Bloom"));
        EXPECT_LT(position("Bloom"), position("Composite"));
        viewA.pipeline.BeginBuild();

        /// @note 倍率を下げたとき、拡大元や filter が欠けた経路を有効な復帰先とみなさない。
        settings.renderScale = 0.5f;
        const auto scaledOutput = resources.CreateRenderTarget(1280, 720);
        ASSERT_TRUE(rendering.PrepareView(viewA, device, scaledOutput, settings));
        EXPECT_TRUE(viewA.needsUpscale);
        EXPECT_EQ(viewA.nativeWidth, 1280u);
        EXPECT_EQ(viewA.width, 640u);
        auto scalePlan = renderer::PrepareViewRenderPlan(resources, device, settings, viewA, shared, planHandles);
        EXPECT_FALSE(scalePlan.IsValid());
        shared.copyColorShader = shader;
        scalePlan = renderer::PrepareViewRenderPlan(resources, device, settings, viewA, shared, planHandles);
        EXPECT_TRUE(scalePlan.IsValid());
        resources.Release(viewA.upscaleSrc);
        scalePlan = renderer::PrepareViewRenderPlan(resources, device, settings, viewA, shared, planHandles);
        EXPECT_FALSE(scalePlan.IsValid());
        EXPECT_EQ(scalePlan.failureReason, renderer::RenderPlanReason::RASTER_PIPELINE_UNAVAILABLE);
        renderer::BuildViewPipeline(viewA.pipeline, context, viewA, shared, { scalePlan }, extensions);
        EXPECT_TRUE(viewA.pipeline.RegisteredPassNames().empty());
        ASSERT_TRUE(rendering.PrepareView(viewA, device, outputB, settings));
        ASSERT_TRUE(renderer::PrepareViewRenderPlan(resources, device, settings, viewA, shared, planHandles).IsValid());
        resources.Release(scaledOutput);

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
}
}
}
