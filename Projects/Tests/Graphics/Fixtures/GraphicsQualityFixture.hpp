/// @file    GraphicsQualityFixture.hpp
/// @brief   本番シェーダーの DX12 読み戻しに共通の資源と前提を用意する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once
#include <TestKit/TestKit.hpp>
#include <TestKit/Graphics/ShaderPath.hpp>
#include <TestKit/TempDir.hpp>
#include <Core/ILogSink.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace fbzz::tests {
namespace r = renderer;
struct GraphicsQualityPlatformState;
struct QualityConstants { uint32_t mode = 0; float value = 0; uint32_t blocked = 0; float reserved = 0; };
inline std::filesystem::path ResolveQualityShader(const std::filesystem::path& requested)
{
    return testkit::ResolveShaderPath(requested, FBZZ_GRAPHICS_SHADER_ROOT);
}
class GraphicsQualityShaderTest : public testkit::Fixture, private core::ILogSink {
protected:
    GraphicsQualityShaderTest();
    ~GraphicsQualityShaderTest() override;
    void SetUp() override;
    void TearDown() override;
    void Begin()
    {
        m_resources->AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_open = true;
        const auto camera = r::MakeCameraFrameCB(m_camera, 0, 0);
        m_resources->Update(m_frame, &camera, sizeof(camera));
        const auto post = r::MakeScreenPostProcCB(8, 8);
        m_resources->Update(m_post, &post, sizeof(post));
        m_resources->Update(m_advanced, &m_data, sizeof(m_data));
        const r::TaaCycleCB cycle{};
        m_resources->Update(m_taaCycle, &cycle, sizeof(cycle));
    }
    void End()
    {
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame(); m_open = false;
    }
    r::DrawCall Draw(r::ResourceHandle<r::ShaderTag> shader)
    {
        r::DrawCall draw; draw.shader = shader; draw.pipelineState = m_opaque; draw.vertexCount = 3;
        draw.constantBuffers[0] = m_frame;
        draw.constantBuffers[2] = shader == m_taa ? m_taaCycle : m_quality;
        draw.constantBuffers[5] = m_post; draw.constantBuffers[8] = m_advanced;
        return draw;
    }
    void Probe(r::ResourceHandle<r::RenderTargetTag> target, QualityConstants constants)
    {
        m_resources->Update(m_quality, &constants, sizeof(constants));
        m_bundle.renderer->SetRenderTarget(target, *m_resources);
        auto draw = Draw(m_probe);
        draw.textures[17] = m_resources->GetCubemapTexture(m_cube);
        draw.textures[18] = m_white; draw.textures[22] = m_sh;
        draw.indirectReadTextures[0] = m_visibility;
        m_bundle.renderer->Submit(draw, *m_resources);
    }
    math::Vector4 Read()
    {
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_output, *m_resources, pixels, width, height)
            || width != 8 || height != 8) { ADD_FAILURE() << "Readback failed"; return {}; }
        const auto i = (4 * 8 + 4) * 4;
        return {pixels[i],pixels[i+1],pixels[i+2],pixels[i+3]};
    }
    math::Vector4 Taa(float historyDepth, bool reactive = false, float previousNdcDepth = -1.0f,
                     bool sampled = false)
    {
        const auto camera = r::MakeCameraFrameCB(m_camera, 0, 0);
        m_data.taaFeedback = 0.9f; m_data.prevViewProjection = camera.viewProjection;
        Begin(); Probe(m_history, {0, historyDepth});
        r::ResourceHandle<r::RenderTargetTag> motion;
        if (previousNdcDepth >= 0.0f) {
            motion = m_resources->CreateRenderTarget(8, 8, {1, r::Format::RGBA16F, false});
            m_bundle.renderer->SetRenderTarget(motion, *m_resources);
            m_bundle.renderer->Clear({0,0,1,previousNdcDepth});
        }
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        auto draw = Draw(m_taa); draw.textures[5] = m_current; draw.textures[7] = m_depth;
        if (sampled) {
            const std::array<uint8_t, 4> black{0, 0, 0, 255};
            draw.textures[5] = m_resources->CreateTexture(black.data(), 1, 1);
            r::ClusterConstantsCB cluster{};
            cluster.clusterLightMode = static_cast<uint32_t>(r::ClusterLightMode::SAMPLED);
            const auto constants = m_resources->CreateConstantBuffer(sizeof(cluster));
            m_resources->Update(constants, &cluster, sizeof(cluster));
            draw.constantBuffers[9] = constants;
        }
        draw.textures[21] = m_resources->GetColorTexture(m_history, 0);
        draw.textures[6] = m_resources->GetColorTexture(m_history, 1);
        if (reactive) draw.textures[9] = m_white;
        if (motion.IsValid()) draw.textures[26] = m_resources->GetColorTexture(motion, 0);
        m_bundle.renderer->Submit(draw, *m_resources); End();
        return Read();
    }
    math::Vector4 TaaCurrentRampAtJitter(float jitterPixels)
    {
        std::array<uint8_t, 8u * 8u * 4u> pixels{};
        for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
            const auto offset = (y * 8u + x) * 4u;
            pixels[offset] = static_cast<uint8_t>(std::round(std::clamp(
                (static_cast<float>(x) + 0.5f - jitterPixels) / 8.0f, 0.0f, 1.0f) * 255.0f));
            pixels[offset + 3u] = 255u;
        }
        const auto current = m_resources->CreateTexture(pixels.data(), 8, 8);
        m_data.taaFeedback = 0.0f;
        m_data.taaJitterX = jitterPixels * 2.0f / 8.0f;
        m_data.taaJitterY = 0.0f;
        Begin();
        const auto frame = r::MakeCameraFrameCB(m_camera, m_data.taaJitterX, 0.0f);
        m_resources->Update(m_frame, &frame, sizeof(frame));
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        auto draw = Draw(m_taa); draw.textures[5] = current; draw.textures[7] = m_depth;
        m_bundle.renderer->Submit(draw, *m_resources); End();
        return Read();
    }
    struct TaaSilhouetteReadback {
        std::array<float, 16> phases{};
        std::array<std::array<float, 16>, 11> edgePhases{};
        std::array<float, 8> changedColor{};
        std::array<float, 8> switchedColor{};
        std::array<float, 8> switchedHistoryLength{};
        uint32_t emptyFootprintPhases = 0;
        float exposedBackground = 1.0f;
        float exposedHistoryLength = 0.0f;
        float freshHistoryLength = 0.0f;
        float movingHistoryLength = 0.0f;
        float reactiveHistoryLength = 0.0f;
    };
    /// @return 静止輪郭の 2 周期、露出背景、色変更と履歴リセット時の GPU 出力を読み戻す。
    /// @note thinGeometry は幅 0.3 ピクセルの線を使い、静止 2 周期だけを返す。
    TaaSilhouetteReadback TaaStaticSilhouette(bool velocity, bool skyBackground, bool hdr = false,
                                            bool staticProof = false, bool thinGeometry = false);
    math::Vector4 TaaRadiance(math::Vector3 current, math::Vector3 history, float encodedHistoryDepth,
                             bool reactive = false, float brightness = 1.0f, float fade = 0.0f)
    {
        const auto camera = r::MakeCameraFrameCB(m_camera, 0, 0);
        m_data.taaFeedback = 0.9f; m_data.prevViewProjection = camera.viewProjection;
        const auto radiance = m_resources->CreateRenderTarget(8, 8, {1, r::Format::RGBA16F, false});
        Begin();
        auto post = r::MakeScreenPostProcCB(8, 8);
        post.exposure = 1.0f; post.saturation = 1.0f;
        post.userBrightness = brightness; post.screenFadeAlpha = fade;
        m_resources->Update(m_post, &post, sizeof(post));
        m_bundle.renderer->SetRenderTarget(radiance, *m_resources);
        m_bundle.renderer->Clear({current.x, current.y, current.z, 1});
        m_bundle.renderer->SetRenderTarget(m_history, *m_resources);
        m_bundle.renderer->Clear({encodedHistoryDepth, history.x, history.y, history.z});
        r::ClusterConstantsCB cluster{};
        cluster.clusterLightMode = static_cast<uint32_t>(r::ClusterLightMode::SAMPLED);
        const auto constants = m_resources->CreateConstantBuffer(sizeof(cluster));
        m_resources->Update(constants, &cluster, sizeof(cluster));
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        auto draw = Draw(m_taa);
        draw.constantBuffers[9] = constants;
        draw.textures[5] = m_current; draw.textures[7] = m_depth;
        draw.textures[8] = m_resources->GetColorTexture(radiance, 0);
        draw.textures[21] = m_resources->GetColorTexture(m_history, 0);
        draw.textures[6] = m_resources->GetColorTexture(m_history, 1);
        if (reactive) draw.textures[9] = m_white;
        m_bundle.renderer->Submit(draw, *m_resources); End();
        return Read();
    }
    math::Vector4 CompositeRadiance(math::Vector3 radiance, float brightness = 1.0f, float fade = 0.0f)
    {
        const auto shader = m_resources->LoadShader("Assets/Shaders/PostProcess/Color/Composite.hlsl");
        const auto source = m_resources->CreateRenderTarget(8, 8, {1, r::Format::RGBA16F, false});
        Begin();
        auto post = r::MakeScreenPostProcCB(8, 8);
        post.exposure = 1.0f; post.saturation = 1.0f;
        post.userBrightness = brightness; post.screenFadeAlpha = fade;
        m_resources->Update(m_post, &post, sizeof(post));
        m_bundle.renderer->SetRenderTarget(source, *m_resources);
        m_bundle.renderer->Clear({radiance.x, radiance.y, radiance.z, 1});
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        auto draw = Draw(shader);
        draw.textures[5] = m_resources->GetColorTexture(source, 0); draw.textures[7] = m_depth;
        m_bundle.renderer->Submit(draw, *m_resources); End();
        return Read();
    }
    math::Vector4 TaaSpatial(uint32_t edge)
    {
        std::array<uint8_t, 8u * 8u * 4u> radiance{}, material{}, normal{}, depth{};
        for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
            const auto offset = (y * 8u + x) * 4u;
            const bool neighbor = (x + y) % 2u != 0u;
            const uint8_t light = neighbor ? 255u : 0u;
            const uint8_t albedo = edge == 1u && neighbor ? 0u : 255u;
            const uint8_t encodedNormalY = edge == 2u && neighbor ? 0u : 255u;
            const uint8_t ndc = edge == 3u && neighbor ? 230u : 128u;
            radiance[offset] = radiance[offset + 1u] = radiance[offset + 2u] = light;
            radiance[offset + 3u] = 255u;
            material[offset] = material[offset + 1u] = material[offset + 2u] = albedo;
            material[offset + 3u] = 255u;
            normal[offset] = normal[offset + 2u] = 128u;
            normal[offset + 1u] = encodedNormalY;
            depth[offset] = depth[offset + 1u] = depth[offset + 2u] = ndc;
            depth[offset + 3u] = 255u;
        }
        const auto current = m_resources->CreateTexture(radiance.data(), 8, 8);
        const auto surfaceMaterial = m_resources->CreateTexture(material.data(), 8, 8);
        const auto surfaceNormal = m_resources->CreateTexture(normal.data(), 8, 8);
        const auto surfaceDepth = m_resources->CreateTexture(depth.data(), 8, 8);
        m_data.taaFeedback = 0.0f;
        Begin();
        auto post = r::MakeScreenPostProcCB(8, 8);
        post.exposure = post.saturation = post.userBrightness = 1.0f;
        m_resources->Update(m_post, &post, sizeof(post));
        r::ClusterConstantsCB cluster{};
        cluster.clusterLightMode = static_cast<uint32_t>(r::ClusterLightMode::SAMPLED);
        const auto constants = m_resources->CreateConstantBuffer(sizeof(cluster));
        m_resources->Update(constants, &cluster, sizeof(cluster));
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        auto draw = Draw(m_taa);
        draw.constantBuffers[9] = constants;
        draw.textures[5] = m_current; draw.textures[7] = surfaceDepth; draw.textures[8] = current;
        draw.textures[11] = surfaceMaterial; draw.textures[12] = surfaceNormal; draw.textures[13] = surfaceDepth;
        m_bundle.renderer->Submit(draw, *m_resources); End();
        return Read();
    }
    math::Vector4 Gi(uint32_t blocked, bool visibility)
    {
        m_data.probeVolumes[0].boxMin = {-1,-1,-1};
        m_data.probeVolumes[0].invSize = {0.5f,0.5f,0.5f};
        m_data.probeVolumes[0].grid[0] = 2; m_data.probeVolumes[0].intensity = 1;
        m_data.probeVisibilityIndices[0] = visibility ? m_resources->Get(m_visibility)->GetBindlessIndex() : r::INVALID_BINDLESS_INDEX;
        Begin();
        const QualityConstants constants{0,0,blocked};
        m_resources->Update(m_quality, &constants, sizeof(constants));
        r::ComputeCall fill; fill.shader = m_fill; fill.constantBuffers[2] = m_quality;
        fill.uavOutputs[0] = m_sh; fill.uavOutputs[1] = m_visibility; fill.dispatchX = 2;
        m_bundle.renderer->Dispatch(fill, *m_resources);
        Probe(m_output, {3}); End(); return Read();
    }
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]") || entry.message.starts_with("  [CORRUPTION]")) {
            ++m_validationFailures; ADD_FAILURE() << entry.message;
        }
    }
    r::RendererBundle m_bundle;
    std::unique_ptr<r::ResourceManager> m_resources;
    r::Camera m_camera;
    r::AdvancedGraphicsCB m_data{};
    r::ResourceHandle<r::ShaderTag> m_probe, m_fill, m_taa, m_resolve;
    r::ResourceHandle<r::PipelineStateTag> m_opaque, m_additive;
    r::ResourceHandle<r::ConstantBufferTag> m_frame, m_post, m_advanced, m_quality, m_taaCycle;
    r::ResourceHandle<r::RenderTargetTag> m_output, m_history, m_delta, m_cube;
    r::ResourceHandle<r::TextureTag> m_current, m_depth, m_white, m_sh, m_visibility;
    std::unique_ptr<GraphicsQualityPlatformState> m_platform;
    bool m_open = false;
    uint32_t m_validationFailures = 0;
    testkit::TempDir m_temp{"graphics-quality"};
};
} /// @note namespace fbzz::tests
