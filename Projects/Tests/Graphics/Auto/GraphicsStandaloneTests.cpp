/// @file    GraphicsStandaloneTests.cpp
/// @brief   Engine をリンクせず Graphics のパイプラインと GPU 描画を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
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
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
class GraphicsStandaloneTest : public testkit::Fixture {};

TEST_F(GraphicsStandaloneTest, ForwardAndDeferredAreAvailableWithoutEngine)
{
    EXPECT_EQ(GetModuleHandleW(L"FBZZEngine.dll"), nullptr);
    EXPECT_EQ(GetModuleHandleW(L"FBZZPhysics.dll"), nullptr);
    EXPECT_EQ(GetModuleHandleW(L"FBZZFluid.dll"), nullptr);
    for (const auto path : {renderer::OpaqueRenderPath::FORWARD, renderer::OpaqueRenderPath::DEFERRED}) {
        renderer::RenderPipeline pipeline;
        renderer::BuildGeometryPreparation(pipeline, true);
        renderer::BuildGeometryPipeline(pipeline, {path});
        const auto names = pipeline.RegisteredPassNames();
        EXPECT_NE(std::find(names.begin(), names.end(), "Shadow"), names.end());
        const bool deferred = path == renderer::OpaqueRenderPath::DEFERRED;
        EXPECT_EQ(std::find(names.begin(), names.end(), "DeferredLighting") != names.end(), deferred);
        EXPECT_EQ(std::find(names.begin(), names.end(), "ForwardOpaque") != names.end(), !deferred);
    }
}

TEST_F(GraphicsStandaloneTest, SnapshotOwnsEffectAndLightingValues)
{
    renderer::RenderScene source;
    source.lighting.punctualLights.emplace_back().intensity = 4;
    source.particles.emplace_back().runtime.particles.emplace_back().position = {1, 2, 3};
    source.customPost.emplace_back().parameters = {12, 34};
    const renderer::RenderScene snapshot = source;
    source.lighting.punctualLights.clear();
    source.particles.clear();
    source.customPost[0].parameters[0] = 99;
    EXPECT_FLOAT_EQ(snapshot.lighting.punctualLights[0].intensity, 4);
    EXPECT_VEC3_NEAR(snapshot.particles[0].runtime.particles[0].position, (math::Vector3{1, 2, 3}), 0.0f);
    EXPECT_EQ(snapshot.customPost[0].parameters[0], 12);
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
        resources.Reset();
    }
    bundle.imguiRenderer.reset();
    device.Shutdown();
}
}
}
