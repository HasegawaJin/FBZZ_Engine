/// @file    RayReflectionReconstructionTests.cpp
/// @brief   Linear HDR reflection reconstruction, confidence and history boundaries on DX12.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayReflectionResources.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Passes/RayTracing/RayReflectionReconstructionPass.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

inline constexpr uint32_t kExtent = 5;
inline constexpr uint32_t kPixelCount = kExtent * kExtent;
inline constexpr uint32_t kCenter = kPixelCount / 2;
inline constexpr uint32_t kHalfExtent = (kExtent + 1u) / 2u;
using FramePixels = std::array<math::Vector4, kPixelCount>;
using HalfFramePixels = std::array<math::Vector4, kHalfExtent * kHalfExtent>;
using SurfacePixels = std::array<renderer::RayReflectionSurface, kPixelCount>;

enum class StageFailure : uint8_t {
    NONE,
    RAW_NOT_RECORDED,
    MISSING_TEMPORAL,
    NON_COMPUTE_TEMPORAL,
    NON_COMPUTE_SPATIAL,
};

class RayReflectionReconstructionTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Reflection reconstruction test", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 32, 32);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        m_fillShader = m_resources->LoadShader((root / "../../Projects/Tests/Graphics/Shaders/RayReflectionRaw.hlsl").lexically_normal().generic_string());
        m_copyShader = m_resources->LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_reconstructionShader = m_resources->LoadShader((root / "RayTracing/RayReflectionReconstruction.cs.hlsl").generic_string());
        m_gbufferShader = m_resources->LoadShader((root / "../../Projects/Tests/Graphics/Shaders/RayReflectionGBuffer.hlsl").lexically_normal().generic_string());
        m_pipeline = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_fillConstants = m_resources->CreateConstantBuffer(2 * sizeof(math::Vector4));
        m_raw = m_resources->CreateRenderTarget(kExtent, kExtent, {1, renderer::Format::RGBA16F, false});
        m_halfRaw = m_resources->CreateRenderTarget(kHalfExtent, kHalfExtent, {1, renderer::Format::RGBA16F, false});
        m_readback = m_resources->CreateRenderTarget(kExtent, kExtent, {1, renderer::Format::RGBA16F, false});
        m_gbuffer = m_resources->CreateRenderTarget(kExtent, kExtent,
            renderer::CameraDepthTargetDesc(renderer::GBUFFER_COLOR_COUNT));
        const std::array<uint8_t, 4> transparent{};
        m_transparent = m_resources->CreateTexture(transparent.data(), 1, 1);
        ASSERT_TRUE(m_fillShader && m_copyShader && m_reconstructionShader && m_gbufferShader
            && m_pipeline && m_fillConstants && m_raw && m_halfRaw && m_readback && m_gbuffer && m_transparent);
        m_camera.m_position = {};
        m_camera.m_aspect = 1;
        m_camera.m_projection = renderer::ProjectionMode::Orthographic;
        m_camera.m_orthoHeight = static_cast<float>(kExtent);
        m_camera.m_near = 0.1f;
        m_camera.m_far = 20;
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

    FramePixels Uniform(const math::Vector4& color) const
    {
        FramePixels result;
        result.fill(color);
        return result;
    }

    void Begin(const FramePixels& pixels)
    {
        m_resources->AdvanceFrame();
        auto& device = *m_bundle.renderer;
        device.BeginFrame();
        m_frameOpen = true;
        FillRaw(m_raw, pixels.data(), kExtent);
    }

    void FillRaw(renderer::ResourceHandle<renderer::RenderTargetTag> target,
        const math::Vector4* pixels, uint32_t extent)
    {
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget(target, *m_resources);
        for (uint32_t pixel = 0; pixel < extent * extent; ++pixel) {
            device.SetViewport(pixel % extent, pixel / extent, 1, 1);
            renderer::DrawCall draw;
            draw.pipelineState = m_pipeline;
            draw.vertexCount = 3;
            if (pixels[pixel].w == 0) {
                draw.shader = m_copyShader;
                draw.textures[5] = m_transparent;
            } else {
                const std::array<math::Vector4, 2> constants{
                    pixels[pixel], math::Vector4{}};
                m_resources->Update(m_fillConstants, constants.data(), sizeof(constants));
                draw.shader = m_fillShader;
                draw.constantBuffers[0] = m_fillConstants;
            }
            device.Submit(draw, *m_resources);
        }
        device.SetRenderTarget({}, *m_resources);
    }

    FramePixels Finish(renderer::ResourceHandle<renderer::TextureTag> output, bool captureResult = true)
    {
        auto& device = *m_bundle.renderer;
        if (!captureResult) {
            device.EndFrame();
            m_frameOpen = false;
            return {};
        }
        device.SetRenderTarget(m_readback, *m_resources);
        renderer::DrawCall copy;
        copy.shader = m_copyShader;
        copy.pipelineState = m_pipeline;
        copy.vertexCount = 3;
        copy.textures[5] = output;
        device.Submit(copy, *m_resources);
        device.SetRenderTarget({}, *m_resources);
        device.EndFrame();
        m_frameOpen = false;
        std::vector<float> linear;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_readback, *m_resources, linear, width, height));
        EXPECT_EQ(width, kExtent);
        EXPECT_EQ(height, kExtent);
        EXPECT_EQ(linear.size(), 4 * kPixelCount);
        FramePixels result{};
        if (linear.size() == 4 * kPixelCount)
            for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
                result[pixel] = {linear[4 * pixel], linear[4 * pixel + 1], linear[4 * pixel + 2], linear[4 * pixel + 3]};
        return result;
    }

    void ExpectColor(const math::Vector4& value, const math::Vector4& expected) const
    {
        /// @note Inputs and history are linear HDR; only the final RGBA16F readback permits half-storage rounding.
        EXPECT_NEAR(value.x, expected.x, 0.02f);
        EXPECT_NEAR(value.y, expected.y, 0.02f);
        EXPECT_NEAR(value.z, expected.z, 0.02f);
        EXPECT_NEAR(value.w, expected.w, 0.001f);
    }

    float Variance(const FramePixels& pixels) const
    {
        float mean = 0, variance = 0;
        for (const auto& pixel : pixels) mean += pixel.x / kPixelCount;
        for (const auto& pixel : pixels) variance += (pixel.x - mean) * (pixel.x - mean) / kPixelCount;
        return variance;
    }

    SurfacePixels Surfaces(float roughness = 0.8f) const
    {
        SurfacePixels result{};
        for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel) {
            auto& surface = result[pixel];
            surface.positionDepth = {static_cast<float>(pixel % kExtent) - 2,
                2 - static_cast<float>(pixel / kExtent), 3, 3};
            surface.normalRoughness = {0, 0, -1, roughness};
            surface.geometricNormalOffset = {0, 0, -1, 0.0001f};
            surface.objectMaterialValid = {7, 9, 1, 1};
        }
        return result;
    }

    SurfacePixels MotionSurfaces(bool thin, float cameraX = 0) const
    {
        auto result = Surfaces(thin ? 0.0f : 0.045f);
        for (auto& surface : result) {
            surface.positionDepth.x += cameraX;
            surface.objectMaterialValid[3] = thin ? 2u : 1u;
            surface.motion.terminalPositionParameter = {surface.positionDepth.x, surface.positionDepth.y,
                thin ? 6.0f : 1.0f, thin ? 1.5f : 0.0f};
            surface.motion.objectPrimitiveKind = {21, 4, 3, thin ? 2u : 1u};
        }
        return result;
    }

    void MotionCamera(renderer::RayReflectionViewResources& view, float cameraX)
    {
        auto& data = view.reconstruction.constantsData;
        const auto& committed = view.reconstruction.committedCamera;
        data.previousCameraPosition = committed.cameraPosition;
        data.previousCameraRight = committed.cameraRight;
        data.previousCameraUp = committed.cameraUp;
        data.previousCameraForward = committed.cameraForward;
        data.cameraPosition = {cameraX, 0, 0, 1};
        data.cameraRight = {1, 0, 0, 2.5f};
        data.cameraUp = {0, 1, 0, 2.5f};
        data.cameraForward = {0, 0, 1, 0};
        data.nearDistance = 0.1f; data.farDistance = 20;
        data.constantEnvironmentRadiance = {0, 0, 0, 1};
        data.cameraMotion = view.reconstruction.historyValid && committed.cameraPosition.x != cameraX ? 1u : 0u;
    }

    FramePixels Reconstruct(renderer::RayReflectionViewResources& view, const FramePixels& pixels,
        const SurfacePixels& surfaces, bool reset = false, bool temporalAllowed = true,
        StageFailure failure = StageFailure::NONE, const FramePixels* gbufferValues = nullptr,
        bool captureResult = true, bool useCameraConstants = false, const HalfFramePixels* halfRawValues = nullptr)
    {
        auto& reconstruction = view.reconstruction;
        if (!reconstruction.surfaces[0]) {
            for (auto& surface : reconstruction.surfaces)
                surface = m_resources->CreateRWStructuredBuffer(nullptr, kPixelCount, sizeof(surfaces[0]));
            for (auto& history : reconstruction.histories)
                history = m_resources->CreateRWStructuredBuffer(nullptr, kPixelCount, sizeof(renderer::RayReflectionHistoryRecord));
            reconstruction.output = m_resources->CreateComputeTexture(kExtent, kExtent);
            reconstruction.constants = m_resources->CreateConstantBuffer(sizeof(renderer::RayReflectionReconstructionConstants));
            EXPECT_TRUE(reconstruction.surfaces[0] && reconstruction.surfaces[1] && reconstruction.histories[0] && reconstruction.histories[1]
                && reconstruction.output && reconstruction.constants);
            reconstruction.width = reconstruction.height = kExtent;
        }
        view.output = m_resources->GetColorTexture(m_raw, 0);
        view.width = view.height = kExtent;
        reconstruction.prepared = true;
        reconstruction.rawSucceeded = failure != StageFailure::RAW_NOT_RECORDED;
        reconstruction.temporalSucceeded = false;
        auto& data = reconstruction.constantsData;
        if (!useCameraConstants) {
            data.cameraPosition = {0, 0, 0, 1};
            data.cameraRight = {1, 0, 0, 2.5f};
            data.cameraUp = {0, 1, 0, 2.5f};
            data.cameraForward = {0, 0, 1, 0};
            data.cameraMotion = 0;
        }
        data.width = data.height = kExtent;
        data.resetHistory = reset || !reconstruction.historyValid;
        data.temporalAllowed = temporalAllowed;
        data.hasGBuffer = gbufferValues != nullptr;
        data.traceWidth = data.traceHeight = halfRawValues ? kHalfExtent : kExtent;
        data.resolutionDivisor = halfRawValues ? 2u : 1u;
        Begin(pixels);
        if (halfRawValues) FillRaw(m_halfRaw, halfRawValues->data(), kHalfExtent);
        if (gbufferValues) {
            auto& device = *m_bundle.renderer;
            device.SetRenderTarget(m_gbuffer, *m_resources);
            for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel) {
                device.SetViewport(pixel % kExtent, pixel / kExtent, 1, 1);
                const std::array<float, 8> constants{0.5f, (*gbufferValues)[pixel].x,
                    (*gbufferValues)[pixel].y, 0, 0, 0, -1, 0};
                m_resources->Update(m_fillConstants, constants.data(), sizeof(constants));
                renderer::DrawCall draw;
                draw.shader = m_gbufferShader;
                draw.pipelineState = m_pipeline;
                draw.constantBuffers[0] = m_fillConstants;
                draw.vertexCount = 3;
                device.Submit(draw, *m_resources);
            }
            device.SetRenderTarget({}, *m_resources);
        }
        const auto currentSurface = reconstruction.surfaces[reconstruction.surfaceReadIndex ^ 1u];
        m_resources->Update(currentSurface, surfaces.data(), sizeof(surfaces));
        m_handles.rayReflectionResult = view.output;
        renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources,
            m_camera, m_settings, {}, ~0u, m_handles};
        context.experimentalRayTracingEnabled = true;
        context.width = context.height = kExtent;
        context.frameStamp = ++m_frameStamp;
        context.taaJitterNdcX = data.currentJitterX;
        context.taaJitterNdcY = data.currentJitterY;
        context.rayReflectionPassActive = true;
        context.resourceRegistry.BindTexture("RayReflectionRaw", view.output);
        if (halfRawValues)
            context.resourceRegistry.BindTexture("RayReflectionHalfRaw", m_resources->GetColorTexture(m_halfRaw, 0));
        context.resourceRegistry.BindTarget("GBuffer", m_gbuffer);
        context.resourceRegistry.BindTexture("RayReflectionResult", reconstruction.output);
        context.resourceRegistry.BindStructuredBuffer("RayReflectionSurface", currentSurface);
        context.resourceRegistry.BindStructuredBuffer("RayReflectionSurfacePrevious",
            reconstruction.surfaces[reconstruction.surfaceReadIndex]);
        context.resourceRegistry.BindStructuredBuffer("RayReflectionHistory", reconstruction.histories[reconstruction.surfaceReadIndex ^ 1u]);
        context.resourceRegistry.BindStructuredBuffer("RayReflectionHistoryPrevious", reconstruction.histories[reconstruction.surfaceReadIndex]);
        const auto temporalShader = failure == StageFailure::MISSING_TEMPORAL
            ? renderer::ResourceHandle<renderer::ShaderTag>{}
            : failure == StageFailure::NON_COMPUTE_TEMPORAL ? m_copyShader : m_reconstructionShader;
        const auto spatialShader = failure == StageFailure::NON_COMPUTE_SPATIAL ? m_copyShader : m_reconstructionShader;
        renderer::RayReflectionReconstructionPass temporal(view, temporalShader, false);
        renderer::RayReflectionReconstructionPass spatial(view, spatialShader, true);
        const auto execute = [&](renderer::RayReflectionReconstructionPass& pass) {
            renderer::PassBuilder builder;
            pass.Setup(builder, context);
            renderer::PassResources resources(context.resourceRegistry, builder.Accesses(), pass.Name());
            context.passResources = &resources;
            pass.Execute(resources, context);
            context.passResources = nullptr;
        };
        execute(temporal);
        execute(spatial);
        EXPECT_TRUE(context.rayReflectionPassActive);
        if (failure != StageFailure::NONE) EXPECT_EQ(m_handles.rayReflectionResult, view.output);
        else EXPECT_EQ(m_handles.rayReflectionResult, reconstruction.output);
        return Finish(m_handles.rayReflectionResult, captureResult);
    }

    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::Camera m_camera;
    renderer::RenderSettings m_settings;
    renderer::RenderPassHandles m_handles;
    renderer::ResourceHandle<renderer::ShaderTag> m_copyShader;
    renderer::ResourceHandle<renderer::ShaderTag> m_reconstructionShader;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_raw;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_halfRaw;
    uint64_t m_frameStamp = 0;

private:
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.find("  [WARNING] (id=") != std::string::npos
            || entry.message.find("  [ERROR] (id=") != std::string::npos
            || entry.message.find("  [CORRUPTION] (id=") != std::string::npos) ++m_validationFailures;
    }

    renderer::ResourceHandle<renderer::ShaderTag> m_fillShader;
    renderer::ResourceHandle<renderer::ShaderTag> m_gbufferShader;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_fillConstants;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_readback;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gbuffer;
    renderer::ResourceHandle<renderer::TextureTag> m_transparent;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
    uint32_t m_validationFailures = 0;
};

TEST_F(RayReflectionReconstructionTest, StationaryMirrorArithmeticMeanPreservesHdrEnergyAndReducesVariance)
{
    renderer::RayReflectionViewResources view;
    const auto surfaces = Surfaces(0.045f);
    FramePixels result{};
    for (uint32_t frame = 0; frame < 32; ++frame) {
        FramePixels raw;
        for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
            raw[pixel] = ((pixel + frame) & 1u) ? math::Vector4{4, 12, 28, 1} : math::Vector4{12, 4, 4, 1};
        result = Reconstruct(view, raw, surfaces);
        EXPECT_TRUE(view.reconstruction.historyValid);
        if (frame == 0) EXPECT_GT(Variance(result), 15);
        if ((frame & 1u) != 0)
            for (const auto& pixel : result) ExpectColor(pixel, {8, 8, 16, 1});
    }
    EXPECT_LT(Variance(result), 0.0001f);
}

TEST_F(RayReflectionReconstructionTest, HalfTransportReconstructsCoherentRoughCellsIncludingOddExtentEdges)
{
    renderer::RayReflectionViewResources view;
    view.reconstruction.constantsData.spatialRadius = 0;
    HalfFramePixels half{};
    for (uint32_t cell = 0; cell < half.size(); ++cell) {
        const float value = static_cast<float>(cell + 1u);
        half[cell] = {value, 2 * value, 4 * value, 1};
    }
    const auto gbuffer = Uniform({0.8f, 0, 0, 0});
    const auto result = Reconstruct(view, Uniform({}), Surfaces(), false, true,
        StageFailure::NONE, &gbuffer, true, false, &half);
    ASSERT_TRUE(view.reconstruction.historyValid);
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel) {
        const uint32_t cell = pixel / kExtent / 2u * kHalfExtent + pixel % kExtent / 2u;
        ExpectColor(result[pixel], half[cell]);
    }
}

TEST_F(RayReflectionReconstructionTest, HalfTransportRejectsWholeCellsWithDifferentPrimaryIdentityOrRasterSurface)
{
    HalfFramePixels half{};
    half.fill({8, 16, 32, 1});
    for (uint32_t mismatch = 0; mismatch < 11u; ++mismatch) {
        SCOPED_TRACE(mismatch);
        renderer::RayReflectionViewResources view;
        view.reconstruction.constantsData.spatialRadius = 0;
        auto surfaces = Surfaces();
        auto gbuffer = Uniform({0.8f, 0, 0, 0});
        auto& different = surfaces[18];
        if (mismatch == 0) ++different.objectMaterialValid[0];
        else if (mismatch == 1) ++different.objectMaterialValid[1];
        else if (mismatch == 2) ++different.objectMaterialValid[2];
        else if (mismatch == 3) different.objectMaterialValid[3] = 2;
        else if (mismatch == 4) different.normalRoughness = {1, 0, 0, 0.8f};
        else if (mismatch == 5) different.geometricNormalOffset = {1, 0, 0, 0.0001f};
        else if (mismatch == 6) { different.positionDepth.z += 1; different.positionDepth.w += 1; }
        else if (mismatch == 7) different.normalRoughness.w = 0.1f;
        else if (mismatch == 8) different.objectMaterialValid[3] = 0;
        else if (mismatch == 9) gbuffer[18].x = 0.3f;
        else gbuffer[18].y = 1;
        const auto result = Reconstruct(view, Uniform({}), surfaces, false, true,
            StageFailure::NONE, &gbuffer, true, false, &half);
        ASSERT_TRUE(view.reconstruction.historyValid);
        for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel) {
            const uint32_t x = pixel % kExtent, y = pixel / kExtent;
            const bool rejectedCell = x >= 2u && x <= 3u && y >= 2u && y <= 3u;
            ExpectColor(result[pixel], rejectedCell ? math::Vector4{} : math::Vector4{8, 16, 32, 1});
        }
    }
}

TEST_F(RayReflectionReconstructionTest, HalfTransportKeepsUncomputedCoverageSeparateFromValidBlackHistory)
{
    renderer::RayReflectionViewResources view;
    view.reconstruction.constantsData.spatialRadius = 0;
    const auto surfaces = Surfaces();
    const auto gbuffer = Uniform({0.8f, 0, 0, 0});
    HalfFramePixels half{};
    half.fill({8, 16, 32, 1});
    (void)Reconstruct(view, Uniform({}), surfaces, false, true, StageFailure::NONE, &gbuffer, true, false, &half);
    half[4] = {};
    const auto missing = Reconstruct(view, Uniform({}), surfaces, false, true,
        StageFailure::NONE, &gbuffer, true, false, &half);
    half[4] = {0, 0, 0, 1};
    const auto black = Reconstruct(view, Uniform({}), surfaces, false, true,
        StageFailure::NONE, &gbuffer, true, false, &half);
    half[4] = {16, 32, 64, 1};
    const auto continued = Reconstruct(view, Uniform({}), surfaces, false, true,
        StageFailure::NONE, &gbuffer, true, false, &half);
    for (const uint32_t pixel : {12u, 13u, 17u, 18u}) {
        ExpectColor(missing[pixel], {});
        ExpectColor(black[pixel], {0, 0, 0, 1});
        ExpectColor(continued[pixel], {8, 16, 32, 1});
    }
}

TEST_F(RayReflectionReconstructionTest, HalfTransportPreservesSharpAndDielectricFullRawAndNeverInventsTheirCoverage)
{
    renderer::RayReflectionViewResources view;
    view.reconstruction.constantsData.spatialRadius = 0;
    auto surfaces = Surfaces();
    auto gbuffer = Uniform({0.8f, 0, 0, 0});
    auto full = Uniform({});
    full[0] = {2, 4, 8, 1};
    surfaces[0].normalRoughness.w = gbuffer[0].x = 0.045f;
    full[1] = {3, 6, 12, 2};
    surfaces[1].objectMaterialValid[3] = 2;
    full[12] = {7, 9, 11, 1};
    surfaces[4].normalRoughness.w = gbuffer[4].x = 0.045f;
    surfaces[8].objectMaterialValid[3] = 2;
    HalfFramePixels half{};
    half.fill({128, 256, 512, 1});
    const auto result = Reconstruct(view, full, surfaces, false, true,
        StageFailure::NONE, &gbuffer, true, false, &half);
    ExpectColor(result[0], full[0]);
    ExpectColor(result[1], full[1]);
    ExpectColor(result[12], full[12]);
    ExpectColor(result[4], {});
    ExpectColor(result[8], {});
}

TEST_F(RayReflectionReconstructionTest, HistoryLimitUsesBoundedEmaAfterThirtyTwoArithmeticSamples)
{
    renderer::RayReflectionViewResources view;
    const auto surfaces = Surfaces(0.045f);
    for (uint32_t frame = 0; frame < 32; ++frame)
        Reconstruct(view, Uniform({8, 8, 16, 1}), surfaces);
    const auto result = Reconstruct(view, Uniform({40, 24, 48, 1}), surfaces);
    for (const auto& pixel : result) ExpectColor(pixel, {9, 8.5f, 17, 1});
}

TEST_F(RayReflectionReconstructionTest, DielectricTemporalKindsStayIsolatedAndNeverBlurSpatially)
{
    renderer::RayReflectionViewResources view;
    const auto opaqueSurfaces = Surfaces(0.8f);
    const auto initial = Reconstruct(view, Uniform({64, 32, 16, 1}), opaqueSurfaces);
    for (const auto& pixel : initial) ExpectColor(pixel, {64, 32, 16, 1});
    auto glassSurfaces = opaqueSurfaces;
    for (auto& surface : glassSurfaces) surface.objectMaterialValid[3] = 2;
    FramePixels glass;
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
        glass[pixel] = (pixel & 1u) ? math::Vector4{2, 4, 8, 2} : math::Vector4{16, 8, 4, 2};
    const auto current = Reconstruct(view, glass, glassSurfaces);
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel) ExpectColor(current[pixel], glass[pixel]);
    const auto next = Reconstruct(view, Uniform({4, 8, 16, 2}), glassSurfaces);
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
        ExpectColor(next[pixel], (pixel & 1u) ? math::Vector4{3, 6, 12, 2} : math::Vector4{10, 8, 10, 2});
    const auto restored = Reconstruct(view, Uniform({8, 4, 2, 1}), opaqueSurfaces);
    for (const auto& pixel : restored) ExpectColor(pixel, {8, 4, 2, 1});
    const auto continued = Reconstruct(view, Uniform({16, 8, 4, 1}), opaqueSurfaces);
    for (const auto& pixel : continued) ExpectColor(pixel, {12, 6, 3, 1});
}

TEST_F(RayReflectionReconstructionTest, StaticGlassUsesThirtyTwoArithmeticSamplesThenBoundedEma)
{
    renderer::RayReflectionViewResources view;
    auto surfaces = Surfaces();
    for (auto& surface : surfaces) surface.objectMaterialValid[3] = 2;
    FramePixels result{};
    for (uint32_t frame = 0; frame < 32; ++frame) {
        FramePixels raw;
        for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
            raw[pixel] = ((pixel + frame) & 1u) ? math::Vector4{4, 12, 28, 2} : math::Vector4{12, 4, 4, 2};
        result = Reconstruct(view, raw, surfaces);
        EXPECT_TRUE(view.reconstruction.historyValid);
        if (frame == 0) EXPECT_GT(Variance(result), 15);
        if ((frame & 1u) != 0)
            for (const auto& pixel : result) ExpectColor(pixel, {8, 8, 16, 2});
    }
    EXPECT_LT(Variance(result), 0.0001f);
    result = Reconstruct(view, Uniform({40, 24, 48, 2}), surfaces);
    for (const auto& pixel : result) ExpectColor(pixel, {9, 8.5f, 17, 2});
}

TEST_F(RayReflectionReconstructionTest, GlassRejectsInvalidCurrentSamplesKindMismatchResetAndUnknownTemporalContent)
{
    renderer::RayReflectionViewResources view;
    auto surfaces = Surfaces();
    for (auto& surface : surfaces) surface.objectMaterialValid[3] = 2;
    Reconstruct(view, Uniform({64, 32, 16, 2}), surfaces);
    auto raw = Uniform({64, 32, 16, 2});
    raw[kCenter] = {};
    const auto hole = Reconstruct(view, raw, surfaces);
    ExpectColor(hole[kCenter], {});
    raw = Uniform({});
    raw[kCenter] = {16, 8, 4, 2};
    const auto reappeared = Reconstruct(view, raw, surfaces);
    ExpectColor(reappeared[kCenter], raw[kCenter]);
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
        if (pixel != kCenter) ExpectColor(reappeared[pixel], {});
    /// @note A raw kind1 with kind2 metadata must clear the history, not become a dielectric temporal sample.
    Reconstruct(view, Uniform({64, 32, 16, 1}), surfaces);
    const auto matched = Reconstruct(view, Uniform({32, 16, 8, 2}), surfaces);
    for (const auto& pixel : matched) ExpectColor(pixel, {32, 16, 8, 2});
    const auto reset = Reconstruct(view, Uniform({8, 4, 2, 2}), surfaces, true);
    for (const auto& pixel : reset) ExpectColor(pixel, {8, 4, 2, 2});
    const auto forbidden = Reconstruct(view, Uniform({16, 8, 4, 2}), surfaces, false, false);
    for (const auto& pixel : forbidden) ExpectColor(pixel, {16, 8, 4, 2});
}

TEST_F(RayReflectionReconstructionTest, HybridGlassSubsetPreparesTypedGraphAndRejectsUnsupportedOrUnrecordedResults)
{
    if (!m_bundle.renderer->GetCapabilities().inlineRayQuery) GTEST_SKIP();
    auto scene = std::make_shared<renderer::RenderScene>();
    scene->sceneGeneration = 77;
    renderer::RenderObject object;
    object.sourceIndex = 7; object.sourceGeneration = 9; object.itemCount = 1;
    scene->objects.push_back(object);
    std::array<renderer::Vertex, 3> vertices{};
    vertices[0].position = {-1, -1, 3}; vertices[1].position = {0, 1, 3}; vertices[2].position = {1, -1, 3};
    for (auto& vertex : vertices) { vertex.normal = {0, 0, -1}; vertex.tangent = {1, 0, 0}; }
    const std::array<uint32_t, 3> indices{0, 1, 2};
    renderer::RenderMeshItem item;
    item.vertexBuffer = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
    item.indexBuffer = m_resources->CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
    ASSERT_NE(m_resources->Get(item.vertexBuffer), nullptr);
    ASSERT_NE(m_resources->Get(item.indexBuffer), nullptr);
    item.vertexContentVersion = m_resources->Get(item.vertexBuffer)->GetContentVersion();
    item.indexContentVersion = m_resources->Get(item.indexBuffer)->GetContentVersion();
    item.vertexCount = item.indexCount = 3; item.vertexStride = sizeof(renderer::Vertex);
    item.material.valid = true;
    item.material.capabilities.gbufferEquivalentShader = true;
    item.material.rayCapabilities = {true, renderer::RayOpacity::OPAQUE_SURFACE};
    item.material.surface.baseColor = {1, 1, 1, 1};
    item.material.surface.roughness = 0;
    item.material.surface.solidDielectricSupported = true;
    item.material.surface.issue = renderer::SurfaceMaterialIssue::NONE;
    item.material.surface.dielectric.transmission = 1;
    scene->items.push_back(item);
    const auto gbuffer = m_resources->CreateRenderTarget(kExtent, kExtent,
        renderer::CameraDepthTargetDesc(renderer::GBUFFER_COLOR_COUNT));
    ASSERT_TRUE(gbuffer);
    renderer::RenderViewResources view;
    renderer::RenderSharedResources shared;
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    shared.rayReflectionShader = m_resources->LoadShader((root / "RayTracing/RayReflection.cs.hlsl").generic_string());
    shared.rayReflectionReconstructionShader = m_reconstructionShader;
    ASSERT_TRUE(shared.rayReflectionShader);
    m_settings.modeRequest.mode = renderer::RenderMode::HYBRID;
    m_settings.modeRequest.rayReflection = true;
    renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources,
        m_camera, m_settings, {}, ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = kExtent;
    context.renderScene = scene;
    context.rayLightsComplete = true;
    const renderer::OpaqueRenderPlan deferred{renderer::OpaqueRenderPath::DEFERRED};
    Begin(Uniform({0, 0, 0, 0}));
    context.frameStamp = m_resources->FrameStamp();
    ASSERT_TRUE(renderer::PrepareRayReflectionView(context, view, shared, deferred));
    EXPECT_TRUE(view.rayReflectionCovered);
    EXPECT_TRUE(context.rayReflectionPassActive);
    EXPECT_TRUE(view.rayReflection.reconstruction.prepared);
    /// @note The closed-solid authoring precondition belongs to transport; this case isolates resource planning and an intentional no-op receipt.
    shared.rayReflectionShader = m_copyShader;
    renderer::RenderPipeline pipeline;
    renderer::RenderGraph::ResourceDesc imported;
    imported.width = imported.height = kExtent;
    imported.colorCount = renderer::GBUFFER_COLOR_COUNT;
    imported.withDepth = imported.external = true;
    imported.transient = imported.allowAliasing = false;
    pipeline.DeclareTarget("GBuffer", gbuffer, imported);
    renderer::BuildRayReflectionPipeline(pipeline, view, shared, context);
    pipeline.SetOutputs({"RayReflectionResult"});
    ASSERT_TRUE(pipeline.Execute(context));
    EXPECT_EQ(pipeline.LastReport().profiles.size(), 3u);
    EXPECT_FALSE(context.rayReflectionPassActive);
    EXPECT_FALSE(view.rayReflection.reconstruction.historyValid);
    EXPECT_EQ(m_handles.rayReflectionResult, view.rayReflection.output);
    (void)Finish(view.rayReflection.output, false);

    /// @note A live old GBuffer is not a current surface when its named producer is disabled.
    view.rayReflection.reconstruction.historyValid = true;
    context.rayReflectionPassActive = true;
    m_handles.rayReflectionResult = view.rayReflection.output;
    m_settings.passOverrides.push_back({"DeferredGBuffer", false});
    EXPECT_FALSE(renderer::PrepareRayReflectionView(context, view, shared, deferred));
    EXPECT_FALSE(view.rayReflectionCovered);
    EXPECT_FALSE(context.rayReflectionPassActive);
    EXPECT_FALSE(context.rayReflectionReconstructionPrepared);
    EXPECT_FALSE(view.rayReflection.reconstruction.historyValid);
    EXPECT_FALSE(view.rayReflection.reconstruction.rawSucceeded);
    EXPECT_FALSE(m_handles.rayReflectionResult);
    m_settings.passOverrides.clear();
    ASSERT_TRUE(renderer::PrepareRayReflectionView(context, view, shared, deferred));
    EXPECT_TRUE(view.rayReflectionCovered);
    EXPECT_TRUE(context.rayReflectionReconstructionPrepared);
    EXPECT_EQ(view.rayReflection.reconstruction.constantsData.resetHistory, 1u);

    auto& surface = scene->items[0].material.surface;
    for (uint32_t unsupported = 0; unsupported < 3; ++unsupported) {
        surface.roughness = unsupported == 0 ? 1.1f : 0;
        surface.dielectric.transmission = unsupported == 1 ? 0.5f : 1;
        surface.dielectric.thinWalled = unsupported == 2;
        surface.dielectric.attenuationColor = unsupported == 2 ? math::Vector3{0.5f, 1, 1} : math::Vector3{1, 1, 1};
        view.rayReflection.reconstruction.historyValid = true;
        context.rayReflectionPassActive = true;
        m_handles.rayReflectionResult = view.rayReflection.output;
        EXPECT_FALSE(renderer::PrepareRayReflectionView(context, view, shared, deferred));
        EXPECT_FALSE(view.rayReflectionCovered);
        EXPECT_FALSE(context.rayReflectionPassActive);
        EXPECT_FALSE(view.rayReflection.reconstruction.historyValid);
        EXPECT_FALSE(m_handles.rayReflectionResult);
        EXPECT_FALSE(view.rayReflection.scene.surfaceDiagnostics.empty());
    }
}

TEST_F(RayReflectionReconstructionTest, NormalizedGBufferSnapshotsBelongToTheirViewAndManagerAcrossHandleCollisions)
{
    renderer::ResourceManager firstManager(*m_bundle.renderer);
    renderer::ResourceManager secondManager(*m_bundle.renderer);
    renderer::RenderResources first(firstManager), second(secondManager);
    renderer::RenderPassHandles firstHandles, secondHandles, otherViewHandles;
    auto& firstView = first.View(11);
    auto& secondView = second.View(11);
    first.BindPassHandles(firstView, firstHandles);
    second.BindPassHandles(secondView, secondHandles);
    const auto firstBuffer = firstHandles.gbufferMaterialCB;
    const auto secondBuffer = secondHandles.gbufferMaterialCB;
    ASSERT_TRUE(firstBuffer && secondBuffer);
    /// @note ResourceHandle has no Manager identity; a former static draw-CB would incorrectly reuse this equal numeric pair.
    EXPECT_EQ(firstBuffer, secondBuffer);
    EXPECT_NE(firstManager.Get(firstBuffer), secondManager.Get(secondBuffer));
    first.BindPassHandles(firstView, firstHandles);
    EXPECT_EQ(firstHandles.gbufferMaterialCB, firstBuffer);
    second.BindPassHandles(second.View(12), otherViewHandles);
    EXPECT_NE(otherViewHandles.gbufferMaterialCB, secondBuffer);
    first.ReleaseView(11);
    EXPECT_EQ(firstManager.Get(firstBuffer), nullptr);
    EXPECT_NE(secondManager.Get(secondBuffer), nullptr);
    EXPECT_NE(secondManager.Get(otherViewHandles.gbufferMaterialCB), nullptr);
    first.BindPassHandles(first.View(11), firstHandles);
    EXPECT_NE(firstHandles.gbufferMaterialCB, firstBuffer);
    EXPECT_NE(firstManager.Get(firstHandles.gbufferMaterialCB), nullptr);
    second.BindPassHandles(secondView, secondHandles);
    EXPECT_EQ(secondHandles.gbufferMaterialCB, secondBuffer);
    first.ReleaseView(11);
    second.ReleaseView(11);
    second.ReleaseView(12);
}

TEST_F(RayReflectionReconstructionTest, PingPongHdrHistoryRetainsBoundedEmaAcrossTwoHundredFiftySixFrames)
{
    renderer::RayReflectionViewResources view;
    const auto surfaces = Surfaces(0.045f);
    std::array<renderer::ResourceHandle<renderer::StructuredBufferTag>, 2> surfaceHandles{};
    std::array<renderer::ResourceHandle<renderer::StructuredBufferTag>, 2> historyHandles{};
    float expected = 0;
    for (uint32_t frame = 1; frame <= 256; ++frame) {
        const float value = static_cast<float>(frame) / 8;
        expected += (value - expected) / static_cast<float>(std::min(frame, 32u));
        const bool capture = frame == 32 || frame == 33 || frame == 256;
        /// @note Intermediate frames submit normally; readback checkpoints verify HDR arithmetic, not elapsed time.
        const auto result = Reconstruct(view, Uniform({value, value / 2, value / 4, 1}), surfaces,
            false, true, StageFailure::NONE, nullptr, capture);
        auto& state = view.reconstruction;
        ASSERT_TRUE(state.historyValid);
        EXPECT_EQ(state.surfaceReadIndex, frame & 1u);
        EXPECT_EQ(state.lastFrameStamp, frame);
        if (frame == 1) {
            surfaceHandles = state.surfaces;
            historyHandles = state.histories;
        } else {
            EXPECT_EQ(state.surfaces, surfaceHandles);
            EXPECT_EQ(state.histories, historyHandles);
        }
        if (capture)
            for (const auto& pixel : result) ExpectColor(pixel, {expected, expected / 2, expected / 4, 1});
    }
    EXPECT_GT(expected, 27);
}

TEST_F(RayReflectionReconstructionTest, SpatialFilteringReducesRoughNoiseButDoesNotBlurMirrorSamples)
{
    renderer::RayReflectionViewResources view;
    FramePixels raw;
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
        raw[pixel] = (pixel & 1u) ? math::Vector4{4, 12, 28, 1} : math::Vector4{12, 4, 4, 1};
    const auto mirror = Reconstruct(view, raw, Surfaces(0.39f), true);
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel) ExpectColor(mirror[pixel], raw[pixel]);
    const auto rough = Reconstruct(view, raw, Surfaces(0.8f), true);
    EXPECT_LT(Variance(rough), Variance(raw) * 0.5f);
    for (const auto& pixel : rough) {
        EXPECT_TRUE(std::isfinite(pixel.x));
        EXPECT_GE(pixel.x, 4);
        EXPECT_LE(pixel.x, 12);
        EXPECT_NEAR(pixel.w, 1, 0.001f);
    }
    const auto continued = Reconstruct(view, Uniform({0, 0, 0, 1}), Surfaces(0.8f));
    auto rawMean = raw;
    for (auto& pixel : rawMean) { pixel.x /= 2; pixel.y /= 2; pixel.z /= 2; }
    renderer::RayReflectionViewResources reference;
    const auto expected = Reconstruct(reference, rawMean, Surfaces(0.8f), true);
    /// @note Spatial radiance is an output only; the next Temporal mean must use the unfiltered per-pixel history.
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel) ExpectColor(continued[pixel], expected[pixel]);
}

TEST_F(RayReflectionReconstructionTest, SpatialFilterStopsAtFullOwnerMaterialNormalRoughnessAndDepthBoundaries)
{
    renderer::RayReflectionViewResources view;
    for (uint32_t boundary = 0; boundary < 7; ++boundary) {
        auto raw = Uniform({64, 32, 16, 1});
        raw[kCenter] = {8, 4, 2, 1};
        auto surfaces = Surfaces();
        for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel) {
            if (pixel == kCenter) continue;
            auto& surface = surfaces[pixel];
            switch (boundary) {
            case 0: ++surface.objectMaterialValid[0]; break;
            case 1: ++surface.objectMaterialValid[1]; break;
            case 2: ++surface.objectMaterialValid[2]; break;
            case 3: surface.normalRoughness = {1, 0, 0, 0.8f}; break;
            case 4: surface.normalRoughness.w = 0.4f; break;
            case 5: surface.positionDepth.z += 1; surface.positionDepth.w += 1; break;
            default: surface.geometricNormalOffset = {1, 0, 0, 0.0001f}; break;
            }
        }
        SCOPED_TRACE(boundary);
        const auto result = Reconstruct(view, raw, surfaces, true);
        ExpectColor(result[kCenter], raw[kCenter]);
    }
}

TEST_F(RayReflectionReconstructionTest, InvalidAlphaPixelsAreNeverFilledFromHistoryOrSpatialNeighbours)
{
    renderer::RayReflectionViewResources view;
    const auto surfaces = Surfaces();
    Reconstruct(view, Uniform({64, 32, 16, 1}), surfaces);
    auto raw = Uniform({64, 32, 16, 1});
    raw[kCenter] = {};
    const auto hole = Reconstruct(view, raw, surfaces);
    ExpectColor(hole[kCenter], {});
    raw = Uniform({});
    raw[kCenter] = {16, 8, 4, 1};
    const auto reappeared = Reconstruct(view, raw, surfaces);
    ExpectColor(reappeared[kCenter], raw[kCenter]);
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
        if (pixel != kCenter) ExpectColor(reappeared[pixel], {});
    raw[kCenter] = {64, 32, 16, 1};
    const auto isolated = Reconstruct(view, raw, surfaces, true);
    ExpectColor(isolated[kCenter], raw[kCenter]);
    for (uint32_t pixel = 0; pixel < kPixelCount; ++pixel)
        if (pixel != kCenter) ExpectColor(isolated[pixel], {});
}

TEST_F(RayReflectionReconstructionTest, RasterRoughnessAndMetallicBoundariesStopSameMaterialSpatialReuse)
{
    renderer::RayReflectionViewResources view;
    auto raw = Uniform({64, 32, 16, 1});
    raw[kCenter] = {8, 4, 2, 1};
    const auto surfaces = Surfaces();
    auto gbuffer = Uniform({0.8f, 0, 0, 0});
    const auto smoothBoundary = Reconstruct(view, raw, surfaces, true, true, StageFailure::NONE, &gbuffer);
    EXPECT_GT(smoothBoundary[kCenter].x, raw[kCenter].x + 1);
    for (uint32_t boundary = 0; boundary < 2; ++boundary) {
        gbuffer = Uniform(boundary == 0 ? math::Vector4{0.8f, 1, 0, 0} : math::Vector4{0.4f, 0, 0, 0});
        gbuffer[kCenter] = {0.8f, 0, 0, 0};
        SCOPED_TRACE(boundary);
        const auto result = Reconstruct(view, raw, surfaces, true, true, StageFailure::NONE, &gbuffer);
        ExpectColor(result[kCenter], raw[kCenter]);
    }
}

TEST_F(RayReflectionReconstructionTest, ChangedSurfaceAndExplicitResetUseOnlyCurrentHdrSamples)
{
    renderer::RayReflectionViewResources view;
    const auto initial = Surfaces(0.045f);
    for (uint32_t boundary = 0; boundary < 6; ++boundary) {
        Reconstruct(view, Uniform({64, 32, 16, 1}), initial, true);
        auto changed = initial;
        for (auto& surface : changed) {
            switch (boundary) {
            case 0: ++surface.objectMaterialValid[0]; break;
            case 1: ++surface.objectMaterialValid[1]; break;
            case 2: ++surface.objectMaterialValid[2]; break;
            case 3: surface.normalRoughness = {1, 0, 0, 0.045f}; break;
            case 4: surface.normalRoughness.w = 0.3f; break;
            default: surface.positionDepth.z += 1; surface.positionDepth.w += 1; break;
            }
        }
        SCOPED_TRACE(boundary);
        const auto current = Reconstruct(view, Uniform({8, 4, 2, 1}), changed);
        for (const auto& pixel : current) ExpectColor(pixel, {8, 4, 2, 1});
    }
    Reconstruct(view, Uniform({64, 32, 16, 1}), initial, true);
    auto current = Reconstruct(view, Uniform({8, 4, 2, 1}), initial, true);
    for (const auto& pixel : current) ExpectColor(pixel, {8, 4, 2, 1});
    current = Reconstruct(view, Uniform({16, 8, 4, 1}), initial, false, false);
    for (const auto& pixel : current) ExpectColor(pixel, {16, 8, 4, 1});
}

TEST_F(RayReflectionReconstructionTest, SeparateViewsNeverShareTemporalRadianceOrHistoryIndices)
{
    renderer::RayReflectionViewResources game, scene;
    const auto surfaces = Surfaces(0.045f);
    Reconstruct(game, Uniform({4, 8, 12, 1}), surfaces);
    Reconstruct(scene, Uniform({40, 80, 120, 1}), surfaces);
    const auto gameResult = Reconstruct(game, Uniform({12, 16, 20, 1}), surfaces);
    const auto sceneResult = Reconstruct(scene, Uniform({120, 160, 200, 1}), surfaces);
    for (const auto& pixel : gameResult) ExpectColor(pixel, {8, 12, 16, 1});
    for (const auto& pixel : sceneResult) ExpectColor(pixel, {80, 120, 160, 1});
    EXPECT_NE(game.reconstruction.histories[0], scene.reconstruction.histories[0]);
    EXPECT_NE(game.reconstruction.histories[1], scene.reconstruction.histories[1]);
    EXPECT_NE(game.reconstruction.surfaces[0], scene.reconstruction.surfaces[0]);
    EXPECT_NE(game.reconstruction.surfaces[1], scene.reconstruction.surfaces[1]);
    EXPECT_NE(game.reconstruction.output, scene.reconstruction.output);
}

TEST_F(RayReflectionReconstructionTest, StaticTaaJitterRetainsHdrHistoryWithoutAcceptingAnotherDepthLayer)
{
    renderer::RayReflectionViewResources view;
    auto surfaces = Surfaces(0.045f);
    Reconstruct(view, Uniform({4, 8, 12, 1}), surfaces);
    view.reconstruction.constantsData.previousJitterX = 0;
    view.reconstruction.constantsData.currentJitterX = 0.004f;
    for (auto& surface : surfaces) surface.positionDepth.x -= 0.01f;
    const auto jittered = Reconstruct(view, Uniform({12, 16, 20, 1}), surfaces);
    for (const auto& pixel : jittered) ExpectColor(pixel, {8, 12, 16, 1});
    view.reconstruction.constantsData.previousJitterX = 0.004f;
    for (auto& surface : surfaces) {
        surface.positionDepth.z += 0.01f;
        surface.positionDepth.w += 0.01f;
    }
    const auto anotherLayer = Reconstruct(view, Uniform({32, 16, 8, 1}), surfaces);
    for (const auto& pixel : anotherLayer) ExpectColor(pixel, {32, 16, 8, 1});
}

TEST_F(RayReflectionReconstructionTest, MissingOrRejectedDispatchKeepsRawReflectionAndInvalidatesHistory)
{
    renderer::RayReflectionViewResources view;
    for (const uint32_t kind : {1u, 2u}) {
        auto surfaces = Surfaces(0.045f);
        for (auto& surface : surfaces) surface.objectMaterialValid[3] = kind;
        const auto alpha = static_cast<float>(kind);
        SCOPED_TRACE(kind);
        for (const auto failure : {StageFailure::RAW_NOT_RECORDED, StageFailure::MISSING_TEMPORAL,
            StageFailure::NON_COMPUTE_TEMPORAL, StageFailure::NON_COMPUTE_SPATIAL}) {
            SCOPED_TRACE(static_cast<uint32_t>(failure));
            Reconstruct(view, Uniform({64, 32, 16, alpha}), surfaces, true);
            ASSERT_TRUE(view.reconstruction.historyValid);
            const auto readIndex = view.reconstruction.surfaceReadIndex;
            const auto frameStamp = view.reconstruction.lastFrameStamp;
            const auto failed = Reconstruct(view, Uniform({8, 4, 2, alpha}), surfaces, false, true, failure);
            EXPECT_FALSE(view.reconstruction.historyValid);
            EXPECT_EQ(view.reconstruction.surfaceReadIndex, readIndex);
            EXPECT_EQ(view.reconstruction.lastFrameStamp, frameStamp);
            for (const auto& pixel : failed) ExpectColor(pixel, {8, 4, 2, alpha});
            const auto recovered = Reconstruct(view, Uniform({16, 8, 4, alpha}), surfaces);
            for (const auto& pixel : recovered) ExpectColor(pixel, {16, 8, 4, alpha});
        }
    }
}

TEST_F(RayReflectionReconstructionTest, MetadataOverwriteAndFailedReceiptsNeverCommitOrReusePartiallyUpdatedHistory)
{
    renderer::RayReflectionViewResources view;
    const auto surfaces = Surfaces(0.045f);
    auto changedSurfaces = surfaces;
    for (auto& surface : changedSurfaces) {
        ++surface.objectMaterialValid[1];
        surface.positionDepth.z += 1;
        surface.positionDepth.w += 1;
    }
    for (const auto failure : {StageFailure::RAW_NOT_RECORDED, StageFailure::NON_COMPUTE_TEMPORAL,
        StageFailure::NON_COMPUTE_SPATIAL}) {
        SCOPED_TRACE(static_cast<uint32_t>(failure));
        Reconstruct(view, Uniform({64, 32, 16, 1}), surfaces, true);
        const auto committed = Reconstruct(view, Uniform({32, 16, 8, 1}), surfaces);
        for (const auto& pixel : committed) ExpectColor(pixel, {48, 24, 12, 1});
        const auto readIndex = view.reconstruction.surfaceReadIndex;
        const auto frameStamp = view.reconstruction.lastFrameStamp;
        const auto handles = view.reconstruction.surfaces;
    const auto history = view.reconstruction.histories;
        const auto failed = Reconstruct(view, Uniform({8, 4, 2, 1}), changedSurfaces,
            false, true, failure);
        for (const auto& pixel : failed) ExpectColor(pixel, {8, 4, 2, 1});
        EXPECT_FALSE(view.reconstruction.historyValid);
        EXPECT_EQ(view.reconstruction.surfaceReadIndex, readIndex);
        EXPECT_EQ(view.reconstruction.lastFrameStamp, frameStamp);
        EXPECT_EQ(view.reconstruction.surfaces, handles);
        EXPECT_EQ(view.reconstruction.histories, history);
        /// @note A rejected Spatial receipt may follow a current-history Temporal write; recovery must discard that uncommitted mean.
        const auto recovered = Reconstruct(view, Uniform({16, 8, 4, 1}), surfaces);
        for (const auto& pixel : recovered) ExpectColor(pixel, {16, 8, 4, 1});
        EXPECT_TRUE(view.reconstruction.historyValid);
        EXPECT_EQ(view.reconstruction.surfaceReadIndex, readIndex ^ 1u);
        const auto continued = Reconstruct(view, Uniform({24, 12, 6, 1}), surfaces);
        for (const auto& pixel : continued) ExpectColor(pixel, {20, 10, 5, 1});
    }
}

TEST_F(RayReflectionReconstructionTest, PreparationReusesExactSizedSplitResourcesAndInvalidatesHistoryOnResizeOrLostStorage)
{
    renderer::RenderViewResources view;
    renderer::RenderSharedResources shared;
    shared.rayReflectionReconstructionShader = m_reconstructionShader;
    view.rayReflection.scene.sceneGeneration = 42;
    view.rayReflection.pathScene.contentRevision = 1;
    renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources,
        m_camera, m_settings, {}, ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = kExtent;
    context.frameStamp = 1;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    auto& state = view.rayReflection.reconstruction;
    const auto surfaces = state.surfaces;
    const auto history = state.histories;
    const auto output = state.output;
    const auto constants = state.constants;
    size_t metadataBytes = 0;
    for (auto handle : history) {
        ASSERT_NE(m_resources->Get(handle), nullptr);
        EXPECT_EQ(m_resources->Get(handle)->GetStride(), 16u);
        metadataBytes += m_resources->Get(handle)->GetSize();
    }
    for (const auto handle : surfaces) {
        ASSERT_NE(m_resources->Get(handle), nullptr);
        EXPECT_EQ(m_resources->Get(handle)->GetStride(), 96u);
        EXPECT_EQ(m_resources->Get(handle)->GetElementCount(), kPixelCount);
        metadataBytes += m_resources->Get(handle)->GetSize();
    }
    /// @note Exact logical payload excludes resource alignment, RAW/output textures, and constants; no performance result is inferred.
    EXPECT_EQ(metadataBytes, static_cast<size_t>(kPixelCount) * 224);
    const auto populated = Reconstruct(view.rayReflection, Uniform({64, 32, 16, 1}), Surfaces(0.045f), true);
    for (const auto& pixel : populated) ExpectColor(pixel, {64, 32, 16, 1});
    state.historyValid = true;
    state.lastFrameStamp = context.frameStamp++;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 0u);
    EXPECT_EQ(state.surfaces, surfaces);
    EXPECT_EQ(state.histories, history);
    EXPECT_EQ(state.output, output);
    EXPECT_EQ(state.constants, constants);

    state.historyValid = true;
    state.surfaceReadIndex = 1;
    state.lastFrameStamp = context.frameStamp++;
    context.width = kExtent + 2;
    context.height = kExtent + 1;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_FALSE(state.historyValid);
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.surfaceReadIndex, 0u);
    EXPECT_EQ(state.constants, constants);
    for (auto handle : history) EXPECT_EQ(m_resources->Get(handle), nullptr);
    EXPECT_EQ(m_resources->Get(output), nullptr);
    for (const auto handle : surfaces) EXPECT_EQ(m_resources->Get(handle), nullptr);
    for (const auto handle : state.surfaces) {
        ASSERT_NE(m_resources->Get(handle), nullptr);
        EXPECT_EQ(m_resources->Get(handle)->GetElementCount(), context.width * context.height);
        EXPECT_EQ(m_resources->Get(handle)->GetStride(), 96u);
    }
    for (auto handle : state.histories) {
        ASSERT_NE(m_resources->Get(handle), nullptr);
        EXPECT_EQ(m_resources->Get(handle)->GetElementCount(), context.width * context.height);
        EXPECT_EQ(m_resources->Get(handle)->GetStride(), 16u);
    }

    const auto resizedHistory = state.histories;
    const auto resizedSurfaces = state.surfaces;
    const auto resizedOutput = state.output;
    m_resources->Release(state.surfaces[1]);
    state.historyValid = true;
    state.surfaceReadIndex = 1;
    state.lastFrameStamp = context.frameStamp++;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_FALSE(state.historyValid);
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.surfaceReadIndex, 0u);
    for (auto handle : resizedHistory) EXPECT_EQ(m_resources->Get(handle), nullptr);
    EXPECT_EQ(m_resources->Get(resizedOutput), nullptr);
    for (const auto handle : resizedSurfaces) EXPECT_EQ(m_resources->Get(handle), nullptr);
    EXPECT_EQ(state.constants, constants);
    for (uint32_t malformed = 0; malformed < 4; ++malformed) {
        SCOPED_TRACE(malformed);
        auto& invalid = malformed < 2 ? state.surfaces[malformed] : state.histories[malformed - 2];
        m_resources->Release(invalid);
        const bool wrongStride = (malformed & 1u) != 0;
        const uint32_t expectedStride = malformed < 2 ? 96u : 16u;
        invalid = m_resources->CreateRWStructuredBuffer(nullptr,
            context.width * context.height - (wrongStride ? 0u : 1u), wrongStride ? expectedStride * 2 : expectedStride);
        ASSERT_TRUE(invalid);
        const auto malformedSurfaces = state.surfaces;
        const auto malformedHistory = state.histories;
        const auto malformedOutput = state.output;
        state.historyValid = true;
        state.surfaceReadIndex = 1;
        state.lastFrameStamp = context.frameStamp++;
        ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
        EXPECT_FALSE(state.historyValid);
        EXPECT_EQ(state.constantsData.resetHistory, 1u);
        EXPECT_EQ(state.surfaceReadIndex, 0u);
        EXPECT_EQ(state.constants, constants);
        for (auto handle : malformedHistory) EXPECT_EQ(m_resources->Get(handle), nullptr);
        EXPECT_EQ(m_resources->Get(malformedOutput), nullptr);
        for (const auto handle : malformedSurfaces) EXPECT_EQ(m_resources->Get(handle), nullptr);
        for (const auto handle : state.surfaces) {
            ASSERT_NE(m_resources->Get(handle), nullptr);
            EXPECT_EQ(m_resources->Get(handle)->GetElementCount(), context.width * context.height);
            EXPECT_EQ(m_resources->Get(handle)->GetStride(), 96u);
        }
        for (auto handle : state.histories) {
            ASSERT_NE(m_resources->Get(handle), nullptr);
            EXPECT_EQ(m_resources->Get(handle)->GetElementCount(), context.width * context.height);
            EXPECT_EQ(m_resources->Get(handle)->GetStride(), 16u);
        }
    }
    state.historyValid = true;
    state.lastFrameStamp = context.frameStamp++;
    context.width = context.height = kExtent;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_FALSE(state.historyValid);
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    const auto resized = Reconstruct(view.rayReflection, Uniform({16, 8, 4, 1}), Surfaces(0.045f));
    for (const auto& pixel : resized) ExpectColor(pixel, {16, 8, 4, 1});
    const auto continued = Reconstruct(view.rayReflection, Uniform({24, 12, 6, 1}), Surfaces(0.045f));
    for (const auto& pixel : continued) ExpectColor(pixel, {20, 10, 5, 1});
}

TEST_F(RayReflectionReconstructionTest, PreparationRejectsChangedContentFrameGapsAndUnknownIblButValidatesCameraMotion)
{
    renderer::RenderViewResources view;
    renderer::RenderSharedResources shared;
    shared.rayReflectionReconstructionShader = m_reconstructionShader;
    view.rayReflection.scene.sceneGeneration = 42;
    view.rayReflection.pathScene.contentRevision = 1;
    renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources,
        m_camera, m_settings, {}, ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = kExtent;
    context.frameStamp = 1;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    auto& state = view.rayReflection.reconstruction;
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    /// @note Arrange a previously committed GPU receipt; this case isolates CPU preparation keys from the shader averaging cases.
    state.historyValid = true;
    state.committedCamera = state.constantsData;
    state.lastFrameStamp = 1;
    context.frameStamp = 2;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 0u);
    for (uint32_t change = 0; change < 6; ++change) {
        state.historyValid = true;
        state.lastFrameStamp = context.frameStamp++;
        switch (change) {
        case 0: ++view.rayReflection.scene.sceneGeneration; break;
        case 1: ++view.rayReflection.pathScene.contentRevision; break;
        case 2: context.cullingMask ^= 1u; break;
        case 3: m_camera.m_position.x += 0.25f; break;
        case 4: m_camera.m_orthoHeight += 0.5f; break;
        default:
            view.rayReflection.constantEnvironmentKnown = true;
            view.rayReflection.constantEnvironmentRadiance = {};
            break;
        }
        SCOPED_TRACE(change);
        ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
        EXPECT_EQ(state.constantsData.resetHistory, change == 3 ? 0u : 1u);
        EXPECT_EQ(state.constantsData.cameraMotion, change == 3 ? 1u : 0u);
    }
    state.historyValid = true;
    state.lastFrameStamp = context.frameStamp++;
    context.taaJitterNdcX = 0.001f;
    context.taaJitterNdcY = -0.001f;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 0u);
    EXPECT_FLOAT_EQ(state.constantsData.currentJitterX, context.taaJitterNdcX);
    state.historyValid = true;
    state.lastFrameStamp = context.frameStamp;
    context.frameStamp += 2;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    state.historyValid = true;
    state.lastFrameStamp = context.frameStamp++;
    const auto shaderVersion = m_resources->GetShaderVersion();
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    ASSERT_TRUE(m_resources->ReloadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string()));
    EXPECT_GT(m_resources->GetShaderVersion(), shaderVersion);
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    state.historyValid = true;
    state.lastFrameStamp = context.frameStamp++;
    const auto iblCube = m_resources->CreateCubemapRenderTarget(2);
    ASSERT_TRUE(iblCube);
    m_handles.iblPrefilter = m_resources->GetCubemapTexture(iblCube);
    m_handles.iblIrradiance = m_handles.iblPrefilter;
    m_handles.iblBrdfLut = m_resources->GetColorTexture(m_raw, 0);
    m_handles.advancedGraphicsCB = m_resources->CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
    view.advancedGraphicsSnapshotValid = true;
    view.advancedGraphicsSnapshot.iblIntensity = 1;
    view.rayReflection.constantEnvironmentKnown = false;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
    const auto commitNextFrame = [&]() {
        state.historyValid = true;
        state.lastFrameStamp = context.frameStamp++;
    };
    commitNextFrame();
    view.rayReflection.constantEnvironmentKnown = true;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    commitNextFrame();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 0u);
    EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    const auto unrelatedCube = m_resources->CreateCubemapRenderTarget(4);
    ASSERT_TRUE(unrelatedCube);
    /// @note Actual known environment lighting does not consume Raster probe radiance, provenance, or ambient fallback.
    for (uint32_t change = 0; change < 4; ++change) {
        commitNextFrame();
        switch (change) {
        case 0:
            view.advancedGraphicsSnapshot.iblIntensity = 17;
            view.advancedGraphicsSnapshot.iblDiffuseScale = 0.5f;
            view.advancedGraphicsSnapshot.iblSpecularScale = 8;
            view.advancedGraphicsSnapshot.iblMaxMipLevel = 4;
            break;
        case 1: context.lightData.ambientColor = {3, 4, 5}; break;
        case 2: m_handles.iblPrefilter = m_resources->GetCubemapTexture(unrelatedCube); break;
        default: view.advancedGraphicsSnapshotValid = false; break;
        }
        SCOPED_TRACE(change);
        ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
        EXPECT_EQ(state.constantsData.resetHistory, 0u);
        EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    }
    commitNextFrame();
    view.rayReflection.constantEnvironmentRadiance = {1, 2, 3};
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    commitNextFrame();
    view.rayReflection.constantEnvironmentKnown = false;
    view.advancedGraphicsSnapshotValid = true;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
    m_settings.passOverrides.push_back({"RayReflectionTemporal", false});
    EXPECT_FALSE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_FALSE(state.historyValid);
    EXPECT_FALSE(context.rayReflectionReconstructionPrepared);
}

TEST_F(RayReflectionReconstructionTest, CameraMotionUsesActualTerminalCorrespondenceForOpaqueAndThinGlass)
{
    for (bool thin : {false, true}) {
        SCOPED_TRACE(thin);
        renderer::RayReflectionViewResources view;
        const float transmission = thin ? 12.0f / 13.0f : 1.0f;
        const float kind = thin ? 2.0f : 1.0f;
        auto old = Uniform({0, 0, 0, kind});
        for (uint32_t index = 0; index < kPixelCount; ++index) {
            const float value = static_cast<float>(10 * (index % kExtent) + 1) * transmission;
            old[index] = {value, value / 2, value / 4, kind};
        }
        MotionCamera(view, 0);
        Reconstruct(view, old, MotionSurfaces(thin), true, true, StageFailure::NONE, nullptr, false, true);
        MotionCamera(view, 1);
        const auto current = Uniform({5 * transmission, 2.5f * transmission, 1.25f * transmission, kind});
        const auto moved = Reconstruct(view, current, MotionSurfaces(thin, 1), false, true,
            StageFailure::NONE, nullptr, true, true);
        /// @note The center maps to the old x+1 terminal (31), not its old receiver index (21); the left-behind history would yield 13 rather than18.
        ExpectColor(moved[kCenter], {18 * transmission, 9 * transmission, 4.5f * transmission, kind});
        ExpectColor(moved[kCenter + 2], current[kCenter + 2]);
        EXPECT_TRUE(view.reconstruction.historyValid);
        EXPECT_EQ(view.reconstruction.surfaceReadIndex, 0u);
    }
}

TEST_F(RayReflectionReconstructionTest, MovingHistoryRejectsChangedTerminalAndUnprovenMultipathWithoutFillingHoles)
{
    for (bool thin : {false, true}) for (uint32_t change = 0; change < 6; ++change) {
        SCOPED_TRACE(thin);
        SCOPED_TRACE(change);
        renderer::RayReflectionViewResources view;
        const float kind = thin ? 2.0f : 1.0f;
        MotionCamera(view, 0);
        Reconstruct(view, Uniform({64, 32, 16, kind}), MotionSurfaces(thin), true, true,
            StageFailure::NONE, nullptr, false, true);
        auto changed = MotionSurfaces(thin, 1);
        for (auto& surface : changed) {
            switch (change) {
            case 0: ++surface.motion.objectPrimitiveKind[0]; break;
            case 1: ++surface.motion.objectPrimitiveKind[1]; break;
            case 2: ++surface.motion.objectPrimitiveKind[2]; break;
            case 3: surface.motion.objectPrimitiveKind[3] = 0; break;
            case 4: surface.positionDepth.z += 0.1f; break;
            default: surface.objectMaterialValid[3] = 0; break;
            }
        }
        MotionCamera(view, 1);
        const auto raw = Uniform(change == 5 ? math::Vector4{} : math::Vector4{8, 4, 2, kind});
        const auto result = Reconstruct(view, raw, changed, false, true, StageFailure::NONE, nullptr, true, true);
        ExpectColor(result[kCenter], raw[kCenter]);
    }
}

TEST_F(RayReflectionReconstructionTest, HalfRoundedThinDemodulationFailureRetainsFiniteRawHistory)
{
    renderer::RayReflectionViewResources view;
    MotionCamera(view, 0);
    view.reconstruction.constantsData.constantEnvironmentRadiance = {32, 0, 0, 1};
    /// @note At IOR1.5, thin reflection is 1/13; half storage rounds 32/13 below its exact reflected contribution.
    const auto first = Uniform({32.0f / 13.0f, 4, 2, 2});
    Reconstruct(view, first, MotionSurfaces(true), true, true, StageFailure::NONE, nullptr, false, true);
    MotionCamera(view, 0);
    view.reconstruction.constantsData.constantEnvironmentRadiance = {32, 0, 0, 1};
    const auto result = Reconstruct(view, Uniform({32.0f / 13.0f, 8, 6, 2}), MotionSurfaces(true),
        false, true, StageFailure::NONE, nullptr, true, true);
    ExpectColor(result[kCenter], {32.0f / 13.0f, 6, 4, 2});
}

TEST_F(RayReflectionReconstructionTest, FailedMovingStagesDiscardPartialHistoryBeforeTheNextSuccessfulFrame)
{
    for (auto failure : {StageFailure::RAW_NOT_RECORDED, StageFailure::NON_COMPUTE_TEMPORAL,
        StageFailure::NON_COMPUTE_SPATIAL}) {
        SCOPED_TRACE(static_cast<int>(failure));
        renderer::RayReflectionViewResources view;
        MotionCamera(view, 0);
        Reconstruct(view, Uniform({64, 32, 16, 1}), MotionSurfaces(false), true, true,
            StageFailure::NONE, nullptr, false, true);
        const auto committed = view.reconstruction.surfaceReadIndex;
        const auto stamp = view.reconstruction.lastFrameStamp;
        MotionCamera(view, 1);
        const auto rejected = Reconstruct(view, Uniform({8, 4, 2, 1}), MotionSurfaces(false, 1), false, true,
            failure, nullptr, true, true);
        ExpectColor(rejected[kCenter], {8, 4, 2, 1});
        EXPECT_FALSE(view.reconstruction.historyValid);
        EXPECT_EQ(view.reconstruction.surfaceReadIndex, committed);
        EXPECT_EQ(view.reconstruction.lastFrameStamp, stamp);
        MotionCamera(view, 1);
        const auto recovered = Reconstruct(view, Uniform({12, 6, 3, 1}), MotionSurfaces(false, 1), false, true,
            StageFailure::NONE, nullptr, true, true);
        ExpectColor(recovered[kCenter], {12, 6, 3, 1});
    }
}

TEST_F(RayReflectionReconstructionTest, HistoryBudgetRetiresAnAllocatedGroupAndQualityChangesResetItsEstimator)
{
    renderer::RenderViewResources view;
    renderer::RenderSharedResources shared;
    shared.rayReflectionReconstructionShader = m_reconstructionShader;
    view.rayReflection.scene.sceneGeneration = 42;
    view.rayReflection.pathScene.contentRevision = 1;
    renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources,
        m_camera, m_settings, {}, ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = kExtent;
    context.frameStamp = 1;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    auto& state = view.rayReflection.reconstruction;
    const auto oldSurfaces = state.surfaces;
    const auto oldHistories = state.histories;
    const auto oldOutput = state.output;
    state.historyValid = true; state.lastFrameStamp = 1; state.committedCamera = state.constantsData;
    context.frameStamp = 2;
    m_settings.hybridQuality.historyLimit = 16;
    m_settings.hybridQuality.spatialRadius = 2;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.constantsData.historyLimit, 16u);
    EXPECT_EQ(state.constantsData.spatialRadius, 2u);
    m_settings.hybridQuality.maxHistoryMiB = 1;
    context.width = context.height = 128;
    EXPECT_FALSE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_FALSE(state.historyValid);
    EXPECT_FALSE(state.prepared);
    EXPECT_FALSE(state.output);
    for (auto handle : oldSurfaces) EXPECT_EQ(m_resources->Get(handle), nullptr);
    for (auto handle : oldHistories) EXPECT_EQ(m_resources->Get(handle), nullptr);
    EXPECT_EQ(m_resources->Get(oldOutput), nullptr);
    context.width = context.height = kExtent;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
}

TEST_F(RayReflectionReconstructionTest, ConsumedGiRequiresLiveImmutablePublicationAndExactProbeContent)
{
    renderer::RenderViewResources view;
    renderer::RenderSharedResources shared;
    shared.rayReflectionReconstructionShader = m_reconstructionShader;
    view.rayReflection.scene.sceneGeneration = 42;
    view.rayReflection.pathScene.contentRevision = 1;
    view.rayReflection.constantEnvironmentKnown = true;
    view.rayReflection.diffuseIndirectEnabled = true;
    view.advancedGraphicsSnapshotValid = true;
    view.advancedGraphicsSnapshot.iblIntensity = 1;
    view.advancedGraphicsSnapshot.iblDiffuseScale = 1;
    m_handles.advancedGraphicsCB = m_resources->CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
    const auto cube = m_resources->CreateCubemapRenderTarget(2);
    ASSERT_TRUE(cube && m_handles.advancedGraphicsCB);
    m_handles.iblIrradiance = m_resources->GetCubemapTexture(cube);
    ASSERT_NE(m_resources->Get(m_handles.iblIrradiance), nullptr);
    EXPECT_EQ(m_resources->Get(m_handles.iblIrradiance)->GetContentVersion(), 0u);
    renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources,
        m_camera, m_settings, {}, ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = kExtent;
    context.frameStamp = 1;
    context.iblIrradiancePublication = {m_handles.iblIrradiance, m_resources.get(), m_resources->GetResetVersion()};
    auto& state = view.rayReflection.reconstruction;
    const auto next = [&]() {
        state.historyValid = true;
        state.lastFrameStamp = context.frameStamp++;
        state.committedCamera = state.constantsData;
    };
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 0u);
    next();
    view.advancedGraphicsSnapshot.iblDiffuseScale = 0.5f;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    next();
    context.iblIrradiancePublication.owner = nullptr;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
    context.iblIrradiancePublication.owner = m_resources.get();
    next();
    view.advancedGraphicsSnapshot.probeVolumes[0].intensity = 1;
    m_handles.lightProbeSH[0] = m_handles.iblIrradiance;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    /// @note Writable SH is not made immutable by an unrelated cube publication, even under a known black environment.
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
    next();
    const std::array<uint8_t, 4> immutablePixel{255, 255, 255, 255};
    const auto immutableProbe = m_resources->CreateTexture(immutablePixel.data(), 1, 1);
    ASSERT_TRUE(immutableProbe);
    m_handles.lightProbeSH[0] = immutableProbe;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    next();
    view.advancedGraphicsSnapshot.probeVolumes[0].normalBias += 0.01f;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    next();
    m_resources->Release(m_handles.iblIrradiance);
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
}

} /// @note namespace
} /// @note namespace fbzz::tests
