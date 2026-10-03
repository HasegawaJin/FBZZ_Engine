/// @file    HybridHistoryTests.cpp
/// @brief   Hybrid の実効照明方式変更と TAA 定数 snapshot を実 GPU で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/ViewPipeline.hpp>
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <filesystem>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

struct TaaReadback {
    math::Vector3 before;
    math::Vector3 after;
};

class HybridHistoryTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Hybrid history test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        auto& resources = *m_resources;
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        m_taaShader = resources.LoadShader((root / "PostProcess/AntiAliasing/TAA.hlsl").generic_string());
        m_copyShader = resources.LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(m_taaShader && m_copyShader && m_state);
        std::array<uint8_t, 3 * 3 * 4> current{};
        std::array<uint8_t, 3 * 3 * 4> previous{};
        for (uint32_t pixel = 0; pixel < 9; ++pixel) {
            const uint8_t value = (pixel & 1u) != 0 ? 255 : 0;
            for (uint32_t channel = 0; channel < 3; ++channel) current[pixel * 4 + channel] = value;
            current[pixel * 4 + 3] = 255;
            previous[pixel * 4] = 192;
            previous[pixel * 4 + 1] = 64;
            previous[pixel * 4 + 2] = 32;
            previous[pixel * 4 + 3] = 255;
        }
        current[16] = 64; current[17] = 128; current[18] = 192;
        m_current = resources.CreateTexture(current.data(), 3, 3);
        m_previous = resources.CreateTexture(previous.data(), 3, 3);
        m_source = resources.CreateRenderTarget(3, 3, {1, renderer::Format::RGBA16F, false});
        m_before = resources.CreateRenderTarget(3, 3, {1, renderer::Format::RGBA16F, false});
        m_depth = resources.CreateRenderTarget(3, 3, renderer::CameraDepthTargetDesc(0));
        renderer::PerFrameCB frame{};
        frame.invViewProjection = math::Matrix4::Identity();
        m_frameCB = resources.CreateConstantBuffer(sizeof(frame));
        resources.Update(m_frameCB, &frame, sizeof(frame));
        const auto post = renderer::MakeScreenPostProcCB(3, 3);
        m_postCB = resources.CreateConstantBuffer(sizeof(post));
        resources.Update(m_postCB, &post, sizeof(post));
        ASSERT_TRUE(m_current && m_previous && m_source && m_before && m_depth && m_frameCB && m_postCB);
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
        testkit::Fixture::TearDown();
    }

    TaaReadback Render(renderer::RenderViewResources& view, renderer::RenderMode mode,
        bool reflectionActive, uint64_t frameStamp, bool reflectionResolveActive = false,
        bool screenReflectionActive = false)
    {
        auto& resources = *m_resources;
        auto& device = *m_bundle.renderer;
        if (!view.advancedGraphicsCB) {
            view.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
            view.taaHistoryA = resources.CreateRenderTarget(3, 3, {1, renderer::Format::RGBA16F, false});
            view.taaHistoryB = resources.CreateRenderTarget(3, 3, {1, renderer::Format::RGBA16F, false});
        }
        view.width = view.height = 3;
        view.taaHistoryValid = true;
        view.renderPlan.effectiveMode = mode;
        view.renderPlan.failureReason = renderer::RenderPlanReason::NONE;
        view.prevViewProjection = math::Matrix4::Identity();
        view.invPrevViewProjection = math::Matrix4::Identity();
        renderer::RenderSettings settings;
        settings.taa.enabled = true;
        settings.taa.feedback = 0.8f;
        settings.postProcess.fxaaEnabled = false;
        renderer::Camera camera;
        renderer::RenderPassHandles handles;
        handles.frameCB = m_frameCB;
        handles.postprocCB = m_postCB;
        handles.advancedGraphicsCB = view.advancedGraphicsCB;
        handles.taaShader = m_taaShader;
        handles.taaPSO = m_state;
        handles.taaHistoryA = view.taaHistoryA;
        handles.taaHistoryB = view.taaHistoryB;
        renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
        context.experimentalRayTracingEnabled = true;
        context.frameStamp = frameStamp;
        context.width = context.height = 3;
        context.rayReflectionPassActive = reflectionActive;
        context.hybridReflectionResolveActive = reflectionResolveActive;
        context.ssrPassActive = screenReflectionActive;
        renderer::PrepareAdvancedConstants(context, view, {});
        EXPECT_FLOAT_EQ(view.advancedGraphicsSnapshot.taaFeedback, 0.8f);
        resources.AdvanceFrame();
        device.BeginFrame();
        m_frameOpen = true;
        Copy(m_source, m_current);
        Copy(view.taaHistoryA, m_previous);
        Copy(view.taaHistoryB, m_previous);
        device.SetRenderTarget(m_depth, resources);
        device.ClearDepth();
        device.SetRenderTarget(m_before, resources);
        renderer::DrawCall before;
        before.shader = m_taaShader;
        before.pipelineState = m_state;
        before.vertexCount = 3;
        before.constantBuffers[0] = m_frameCB;
        before.constantBuffers[5] = m_postCB;
        before.constantBuffers[8] = view.advancedGraphicsCB;
        before.textures[5] = resources.GetColorTexture(m_source, 0);
        before.textures[21] = resources.GetColorTexture(view.taaHistoryA, 0);
        before.textures[7] = resources.GetDepthTexture(m_depth);
        device.Submit(before, resources);
        /// @note TAA 直前の b8 更新は、先行 Submit の 0.8 feedback snapshot を書き換えてはならない。
        renderer::PrepareTaaProviderHistory(context, view);
        context.resourceRegistry.BindTarget("LDR", m_source);
        context.resourceRegistry.BindTarget("HDR", m_depth);
        context.resourceRegistry.BindTarget("Velocity", {});
        using Usage = renderer::RenderGraph::ResourceUsage;
        const std::vector<renderer::RenderGraph::ResourceAccess> accesses{
            {"LDR", Usage::Read}, {"HDR", Usage::Read}, {"Velocity", Usage::Read}};
        renderer::PassResources passResources(context.resourceRegistry, accesses, "TAA");
        context.passResources = &passResources;
        renderer::ExecuteTAAPass(context);
        context.passResources = nullptr;
        view.taaHistoryValid = true;
        device.SetRenderTarget({}, resources);
        device.EndFrame();
        m_frameOpen = false;
        return {Center(m_before), Center(view.taaHistoryB)};
    }

    void Stop(renderer::RenderViewResources& view, bool passOverride)
    {
        renderer::RenderSettings settings;
        settings.taa.enabled = passOverride;
        settings.postProcess.fxaaEnabled = false;
        if (passOverride) settings.passOverrides.push_back({"TAA", false});
        renderer::Camera camera;
        renderer::RenderPassHandles handles;
        renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, camera, settings, {}, ~0u, handles};
        context.experimentalRayTracingEnabled = true;
        renderer::RenderSharedResources shared;
        renderer::ResolvedRenderPlan plan;
        plan.failureReason = renderer::RenderPlanReason::NONE;
        renderer::BuildViewPipeline(view.pipeline, context, view, shared, {plan}, {});
        EXPECT_FALSE(view.taaHistoryValid);
        EXPECT_FALSE(view.taaProviderHistory.valid);
    }

    void ExpectCurrent(const math::Vector3& color)
    {
        EXPECT_VEC3_NEAR(color, (math::Vector3{64 / 255.0f, 128 / 255.0f, 192 / 255.0f}), 0.001f);
    }

    void ExpectBlended(const math::Vector3& color)
    {
        EXPECT_VEC3_NEAR(color, (math::Vector3{
            (192 * 0.8f + 64 * 0.2f) / 255,
            (64 * 0.8f + 128 * 0.2f) / 255,
            (32 * 0.8f + 192 * 0.2f) / 255}), 0.001f);
    }

private:
    void Copy(renderer::ResourceHandle<renderer::RenderTargetTag> output,
        renderer::ResourceHandle<renderer::TextureTag> input)
    {
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget(output, *m_resources);
        renderer::DrawCall draw;
        draw.shader = m_copyShader;
        draw.pipelineState = m_state;
        draw.vertexCount = 3;
        draw.textures[5] = input;
        device.Submit(draw, *m_resources);
    }

    math::Vector3 Center(renderer::ResourceHandle<renderer::RenderTargetTag> target)
    {
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(target, *m_resources, pixels, width, height));
        EXPECT_EQ(width, 3u); EXPECT_EQ(height, 3u); EXPECT_EQ(pixels.size(), 36u);
        if (pixels.size() != 36) return {};
        return {pixels[16], pixels[17], pixels[18]};
    }

    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }

    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
    renderer::ResourceHandle<renderer::ShaderTag> m_taaShader;
    renderer::ResourceHandle<renderer::ShaderTag> m_copyShader;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_state;
    renderer::ResourceHandle<renderer::TextureTag> m_current;
    renderer::ResourceHandle<renderer::TextureTag> m_previous;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_source;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_before;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_depth;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_postCB;
};

TEST_F(HybridHistoryTest, ProviderChangesAndRejectedReflectionResetGpuFeedbackWithoutChangingEarlierSubmits)
{
    renderer::RenderViewResources view;
    auto pixels = Render(view, renderer::RenderMode::HYBRID, true, 10);
    ExpectBlended(pixels.before); ExpectCurrent(pixels.after);
    const auto initialEpoch = view.taaProviderHistory.epoch;
    pixels = Render(view, renderer::RenderMode::HYBRID, true, 11);
    ExpectBlended(pixels.before); ExpectBlended(pixels.after);
    EXPECT_EQ(view.taaProviderHistory.epoch, initialEpoch);
    pixels = Render(view, renderer::RenderMode::HYBRID, false, 12);
    ExpectBlended(pixels.before); ExpectCurrent(pixels.after);
    EXPECT_GT(view.taaProviderHistory.epoch, initialEpoch);
    pixels = Render(view, renderer::RenderMode::RASTER, false, 13);
    ExpectBlended(pixels.before); ExpectCurrent(pixels.after);
    pixels = Render(view, renderer::RenderMode::RASTER, false, 14);
    ExpectBlended(pixels.after);
}

TEST_F(HybridHistoryTest, ViewEpochsFrameGapsAndDisabledTaaRemainIndependent)
{
    renderer::RenderViewResources gameView, sceneView;
    ExpectCurrent(Render(gameView, renderer::RenderMode::HYBRID, true, 1).after);
    ExpectCurrent(Render(sceneView, renderer::RenderMode::RASTER, false, 1).after);
    ExpectCurrent(Render(gameView, renderer::RenderMode::HYBRID, true, 3).after);
    ExpectBlended(Render(sceneView, renderer::RenderMode::RASTER, false, 2).after);
    for (const bool passOverride : {false, true}) {
        Stop(gameView, passOverride);
        ExpectCurrent(Render(gameView, renderer::RenderMode::HYBRID, true, passOverride ? 5 : 4).after);
        ExpectBlended(Render(sceneView, renderer::RenderMode::RASTER, false, passOverride ? 4 : 3).after);
    }
}

TEST_F(HybridHistoryTest, ScreenResolveAndActualSsrChangesResetFeedbackDuringRasterFallback)
{
    renderer::RenderViewResources view;
    uint64_t previousEpoch = 0;
    /// @note Effective Raster and unavailable RT stay fixed; only the selected resolver and actual SSR receipt change. Earlier recorded b8 values must remain at the original feedback.
    const auto frame = [&](uint64_t stamp, bool resolve, bool screen, bool changed) {
        const auto pixels = Render(view, renderer::RenderMode::RASTER, false, stamp, resolve, screen);
        ExpectBlended(pixels.before);
        if (changed) {
            ExpectCurrent(pixels.after);
            EXPECT_GT(view.taaProviderHistory.epoch, previousEpoch);
        } else {
            ExpectBlended(pixels.after);
            EXPECT_EQ(view.taaProviderHistory.epoch, previousEpoch);
        }
        EXPECT_EQ(view.taaProviderHistory.mode, renderer::RenderMode::RASTER);
        EXPECT_FALSE(view.taaProviderHistory.reflectionActive);
        EXPECT_EQ(view.taaProviderHistory.reflectionResolveActive, resolve);
        EXPECT_EQ(view.taaProviderHistory.screenReflectionActive, screen);
        previousEpoch = view.taaProviderHistory.epoch;
    };
    frame(10, false, false, true);
    frame(11, true, true, true);
    frame(12, true, true, false);
    frame(13, true, false, true);
    frame(14, true, false, false);
    frame(15, false, false, true);
}

} /// @note namespace
} /// @note namespace fbzz::tests
