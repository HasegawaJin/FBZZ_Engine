/// @file    ManyLightShaderTests.cpp
/// @brief   本番クラスタCSと共通PBR経路で大量ライトの予算・光量・受光点の画質を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <Graphics/Renderer/LightSystem.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Core/Util/FileSystem.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
std::filesystem::path ResolveManyLightShader(const std::filesystem::path& requested)
{
    const auto path = requested.generic_string();
    constexpr std::string_view PREFIX = "Assets/Shaders/";
    return path.starts_with(PREFIX) ? std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT) / path.substr(PREFIX.size()) : requested;
}
}
class ManyLightShaderTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        Fixture::SetUp();
        renderer::SetShaderPathResolver(ResolveManyLightShader);
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Many light GPU test", WS_POPUP, 0, 0, 64, 64,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        m_probe = m_resources->LoadShader("Assets/Shaders/Rendering/ManyLightProbe.hlsl");
        m_cull = m_resources->LoadShader("Assets/Shaders/Pipeline/Clustered/ClusterLightCull.cs.hlsl");
        m_state = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_target = m_resources->CreateRenderTarget(64, 64, {1, renderer::Format::RGBA32F, false});
        m_frame = m_resources->CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        m_cluster = m_resources->CreateConstantBuffer(sizeof(renderer::ClusterConstantsCB));
        m_light = m_resources->CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
        m_shadow = m_resources->CreateConstantBuffer(sizeof(renderer::PunctualShadowConstantsCB));
        m_advanced = m_resources->CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
        m_point = m_resources->CreateConstantBuffer(sizeof(math::Vector4) * 2u);
        m_indices = m_resources->CreateRWStructuredBuffer(nullptr, renderer::kClusterCount * renderer::kClusterStride, sizeof(uint32_t));
        ASSERT_TRUE(m_probe.IsValid() && m_cull.IsValid() && m_state.IsValid() && m_target.IsValid() && m_indices.IsValid());
    }
    void TearDown() override
    {
        if (m_open) m_bundle.renderer->EndFrame();
        if (m_resources) m_resources->Reset();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        renderer::SetShaderPathResolver(nullptr);
        Fixture::TearDown();
    }
    std::array<double, 4> Render(const std::vector<renderer::PunctualLightGPU>& lights,
        renderer::ClusterLightMode mode, uint32_t samples = 8, uint32_t phase = 0,
        float metallic = 0.0f, float roughness = 0.5f, bool pbrGuiding = true)
    {
        auto& resources = *m_resources;
        const auto buffer = resources.CreateStructuredBuffer(lights.data(), static_cast<uint32_t>(lights.size()), sizeof(renderer::PunctualLightGPU));
        renderer::Camera camera;
        camera.m_position = {0, 1, -3};
        camera.LookAt({0, 0, 2});
        camera.m_projection = renderer::ProjectionMode::Orthographic;
        camera.m_orthoHeight = 0.1f;
        camera.m_aspect = 1;
        camera.m_near = 0.1f;
        camera.m_far = 40;
        const auto frame = renderer::MakeCameraFrameCB(camera, 0, 0);
        resources.Update(m_frame, &frame, sizeof(frame));
        renderer::ClusterConstantsCB cluster{};
        cluster.clusterTilePx[0] = 64.0f / renderer::kClusterGridX;
        cluster.clusterTilePx[1] = 64.0f / renderer::kClusterGridY;
        cluster.clusterSliceScale = renderer::kClusterGridZ / std::log(400.0f);
        cluster.clusterSliceBias = -std::log(0.1f) * cluster.clusterSliceScale;
        cluster.clusterLightMode = static_cast<uint32_t>(mode);
        cluster.punctualLightCount = static_cast<uint32_t>(lights.size());
        cluster.manyLightControl = mode == renderer::ClusterLightMode::SAMPLED ? (phase << 6u) | samples : 0u;
        resources.Update(m_cluster, &cluster, sizeof(cluster));
        const renderer::LightConstantsCB light{};
        const renderer::PunctualShadowConstantsCB shadow{};
        const renderer::AdvancedGraphicsCB advanced{};
        const std::array<math::Vector4, 2> point{{{0, 0, 2, 0},
            {metallic, roughness, 0.7f, pbrGuiding ? 1.0f : 0.0f}}};
        resources.Update(m_light, &light, sizeof(light));
        resources.Update(m_shadow, &shadow, sizeof(shadow));
        resources.Update(m_advanced, &advanced, sizeof(advanced));
        resources.Update(m_point, &point, sizeof(point));
        resources.AdvanceFrame();
        m_bundle.renderer->BeginFrame(); m_open = true;
        if (renderer::UsesClusterLightIndices(mode)) {
            renderer::ComputeCall call;
            call.shader = m_cull;
            call.constantBuffers[0] = m_frame;
            call.constantBuffers[9] = m_cluster;
            call.srvBuffers[14] = buffer;
            call.uavBuffers[0] = m_indices;
            call.dispatchX = (renderer::kClusterCount + 63u) / 64u;
            m_bundle.renderer->Dispatch(call, resources);
        }
        m_bundle.renderer->SetRenderTarget(m_target, resources);
        m_bundle.renderer->Clear({0, 0, 0, 0});
        renderer::DrawCall draw;
        draw.shader = m_probe; draw.pipelineState = m_state; draw.vertexCount = 3;
        draw.constantBuffers[0] = m_frame; draw.constantBuffers[3] = m_light;
        draw.constantBuffers[8] = m_advanced; draw.constantBuffers[9] = m_cluster;
        draw.constantBuffers[12] = m_shadow; draw.constantBuffers[13] = m_point;
        draw.psBuffers[0] = buffer; draw.psBuffers[1] = m_indices;
        m_bundle.renderer->Submit(draw, resources);
        m_bundle.renderer->EndFrame(); m_open = false;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_target, resources, m_pixels, width, height));
        EXPECT_EQ(width, 64u);
        EXPECT_EQ(height, 64u);
        std::array<double, 4> mean{};
        for (size_t pixel = 0; pixel < m_pixels.size() / 4u; ++pixel)
            for (size_t channel = 0; channel < 4; ++channel) {
                EXPECT_TRUE(std::isfinite(m_pixels[pixel * 4u + channel]));
                mean[channel] += m_pixels[pixel * 4u + channel] / (64.0 * 64.0);
            }
        resources.Release(buffer);
        return mean;
    }
    double RelativeImageError(const std::vector<float>& reference) const
    {
        EXPECT_EQ(m_pixels.size(), reference.size());
        double error = 0;
        double energy = 0;
        for (size_t pixel = 0; pixel < (std::min)(m_pixels.size(), reference.size()) / 4u; ++pixel)
            for (size_t channel = 0; channel < 3; ++channel) {
                const double expected = reference[pixel * 4u + channel];
                const double delta = m_pixels[pixel * 4u + channel] - expected;
                error += delta * delta;
                energy += expected * expected;
            }
        return std::sqrt(error / (std::max)(energy, 1.0e-12));
    }
    static renderer::PunctualLightGPU Point(float intensity = 1)
    {
        renderer::PunctualLightGPU light{};
        light.position = {0, 2, 2}; light.range = 20;
        light.color = {0.5f, 0.7f, 1}; light.intensity = intensity;
        light.shadowIndex = light.cookieIndex = -1;
        return light;
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ResourceHandle<renderer::ShaderTag> m_probe, m_cull;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_state;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_frame, m_cluster, m_light, m_shadow, m_advanced, m_point;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_indices;
    std::vector<float> m_pixels;
    HWND m_window = nullptr; HRESULT m_com = E_FAIL; bool m_open = false;
};

TEST_F(ManyLightShaderTest, SmallLightSetsMatchExactLighting)
{
    const std::vector<renderer::PunctualLightGPU> lights(4, Point());
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
    const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(sampled[channel], exact[channel], 0.001);
    EXPECT_NEAR(sampled[3], 4, 0.001);
}
TEST_F(ManyLightShaderTest, ThousandEqualLightsKeepTotalEnergyWithEightEvaluations)
{
    const std::vector<renderer::PunctualLightGPU> lights(1000, Point(0.01f));
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
    const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED);
    EXPECT_GT(exact[0], 0.01);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(sampled[channel], exact[channel], exact[channel] * 0.005 + 0.001);
    EXPECT_NEAR(sampled[3], 8, 0.001);
}
TEST_F(ManyLightShaderTest, UnequalPoolStrataPreserveEnergyAndRespectTheBudget)
{
    const std::vector<renderer::PunctualLightGPU> lights(13, Point());
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
    for (const uint32_t budget : {1u, 7u, 32u}) {
        const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED, budget);
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(sampled[channel], exact[channel], 0.001);
        EXPECT_NEAR(sampled[3], (std::min)(budget, 13u), 0.001);
    }
}
TEST_F(ManyLightShaderTest, SharedPreparationCoversPartialAndMaximumLightBatches)
{
    for (uint32_t count : {65u, renderer::kMaxPunctualLights}) {
        SCOPED_TRACE(count);
        const std::vector<renderer::PunctualLightGPU> lights(count, Point(0.01f));
        const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
        const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED);
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(sampled[channel], exact[channel], exact[channel] * 0.005 + 0.001);
        EXPECT_NEAR(sampled[3], 8, 0.001);
    }
}
TEST_F(ManyLightShaderTest, ImportantLastLightIsNotDiscardedAndStablePhaseReproducesTheImage)
{
    std::vector<renderer::PunctualLightGPU> lights(1000, Point(0.001f));
    lights.back().intensity = 100;
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
    const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED);
    const auto repeated = Render(lights, renderer::ClusterLightMode::SAMPLED);
    for (size_t channel = 0; channel < 3; ++channel) {
        EXPECT_NEAR(sampled[channel], exact[channel], exact[channel] * 0.06 + 0.001);
        EXPECT_NEAR(repeated[channel], sampled[channel], 0.0001);
    }
    std::rotate(lights.begin(), lights.end() - 1, lights.end());
    const auto first = Render(lights, renderer::ClusterLightMode::SAMPLED, 8, 1);
    for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(first[channel], exact[channel], exact[channel] * 0.06 + 0.001);
}
TEST_F(ManyLightShaderTest, ReceiverDistanceGuidingKeepsNearLightsWithoutGrain)
{
    std::vector<renderer::PunctualLightGPU> lights(32, Point());
    for (size_t index = 0; index < lights.size(); ++index)
        lights[index].position.y = index % 4u == 3u ? 2.0f : 15.0f;
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
    const auto reference = m_pixels;
    for (uint32_t phase : {0u, 7u, 31u}) {
        SCOPED_TRACE(phase);
        const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED, 8, phase);
        const double error = RelativeImageError(reference);
        RecordProperty("relativeImageError" + std::to_string(phase), error);
        EXPECT_LT(error, 0.2);
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(sampled[channel], exact[channel], exact[channel] * 0.02);
        EXPECT_NEAR(sampled[3], 8, 0.001);
    }
    const auto first = m_pixels;
    Render(lights, renderer::ClusterLightMode::SAMPLED, 8, 31);
    EXPECT_EQ(m_pixels, first);
    Render(lights, renderer::ClusterLightMode::SAMPLED, 32);
    EXPECT_LT(RelativeImageError(reference), 1.0e-5);
}
TEST_F(ManyLightShaderTest, ReceiverConeGuidingAvoidsWastingSamplesOnAwayFacingSpots)
{
    std::vector<renderer::PunctualLightGPU> lights(32, Point());
    for (size_t index = 0; index < lights.size(); ++index) {
        auto& light = lights[index];
        light.type = static_cast<uint32_t>(renderer::PunctualLightType::Spot);
        light.direction = index % 4u == 3u ? math::Vector3{0, -1, 0} : math::Vector3{0, 1, 0};
        light.innerCos = 0.98f;
        light.outerCos = 0.9f;
    }
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
    const auto reference = m_pixels;
    const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED);
    const double error = RelativeImageError(reference);
    RecordProperty("relativeImageError", error);
    EXPECT_LT(error, 0.2);
    for (size_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(sampled[channel], exact[channel], exact[channel] * 0.02);
    EXPECT_NEAR(sampled[3], 8, 0.001);
    for (auto& light : lights) light.direction = {0, 1, 0};
    Render(lights, renderer::ClusterLightMode::SAMPLED);
    for (size_t pixel = 0; pixel < m_pixels.size() / 4u; ++pixel)
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT_EQ(m_pixels[pixel * 4u + channel], 0.0f);
}
TEST_F(ManyLightShaderTest, LargePoolsRetainNearbyLightsAmongEqualPowerDistantLights)
{
    std::vector<renderer::PunctualLightGPU> lights(1024, Point());
    for (size_t index = 0; index < lights.size(); ++index)
        lights[index].position.y = index % 128u == 127u ? 2.0f : 15.0f;
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
    const auto reference = m_pixels;
    for (uint32_t phase : {0u, 7u, 31u}) {
        SCOPED_TRACE(phase);
        const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED, 32, phase);
        const double error = RelativeImageError(reference);
        RecordProperty("relativeImageError" + std::to_string(phase), error);
        EXPECT_LT(error, 0.2);
        for (size_t channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(sampled[channel], exact[channel], exact[channel] * 0.15);
        EXPECT_NEAR(sampled[3], 32, 0.001);
    }
}
TEST_F(ManyLightShaderTest, SurfaceGuidingAvoidsSamplesBehindTheReceiver)
{
    std::vector<renderer::PunctualLightGPU> lights(32, Point());
    for (size_t index = 0; index < lights.size(); ++index)
        lights[index].position.y = index % 4u == 3u ? 2.0f : -2.0f;
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear);
    const auto reference = m_pixels;
    const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED);
    const double error = RelativeImageError(reference);
    RecordProperty("relativeImageError", error);
    EXPECT_LT(error, 0.2);
    for (size_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(sampled[channel], exact[channel], exact[channel] * 0.02);
    EXPECT_NEAR(sampled[3], 8, 0.001);
}
TEST_F(ManyLightShaderTest, MaterialGuidingPreservesMetalHighlightsAtTheSameLightBudget)
{
    std::vector<renderer::PunctualLightGPU> lights(32, Point());
    const float reflectedZ = 10.0f / std::sqrt(26.0f);
    const float reflectedY = 2.0f / std::sqrt(26.0f);
    for (size_t index = 0; index < lights.size(); ++index)
        lights[index].position = {0, reflectedY, 2.0f + (index % 4u == 3u ? reflectedZ : -reflectedZ)};
    const auto exact = Render(lights, renderer::ClusterLightMode::Linear, 8, 0, 1.0f, 0.18f);
    const auto reference = m_pixels;
    Render(lights, renderer::ClusterLightMode::SAMPLED, 8, 0, 1.0f, 0.18f, false);
    const double geometricError = RelativeImageError(reference);
    const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED, 8, 0, 1.0f, 0.18f);
    const double materialError = RelativeImageError(reference);
    RecordProperty("geometricRelativeImageError", geometricError);
    RecordProperty("materialRelativeImageError", materialError);
    EXPECT_GT(geometricError, 0.4);
    EXPECT_LT(materialError, 0.2);
    for (size_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(sampled[channel], exact[channel], exact[channel] * 0.02);
    EXPECT_NEAR(sampled[3], 8, 0.001);
}
TEST_F(ManyLightShaderTest, TemporalPoolSequenceConvergesWithoutChangingTotalColorEnergy)
{
    std::vector<renderer::PunctualLightGPU> lights(1000, Point(0.01f));
    for (size_t index = 0; index < lights.size(); ++index)
        lights[index].color = index < 500u ? math::Vector3{1, 0, 0} : math::Vector3{0, 0, 1};
    Render(lights, renderer::ClusterLightMode::Linear);
    const auto reference = m_pixels;
    std::vector<float> average(reference.size(), 0.0f);
    for (uint32_t phase = 0; phase < 32u; ++phase) {
        const auto sampled = Render(lights, renderer::ClusterLightMode::SAMPLED, 32, phase);
        EXPECT_NEAR(sampled[3], 32, 0.001);
        for (size_t index = 0; index < average.size(); ++index)
            average[index] += m_pixels[index] / 32.0f;
    }
    m_pixels = std::move(average);
    const double error = RelativeImageError(reference);
    RecordProperty("temporalRelativeImageError", error);
    EXPECT_LT(error, 0.004);
}
} /// @note namespace fbzz::tests
