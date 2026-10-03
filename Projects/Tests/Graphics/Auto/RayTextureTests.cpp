/// @file    RayTextureTests.cpp
/// @brief   Typed immutable material textures, explicit LOD and alpha candidates on DX12.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/Material.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/RayTracing/RayGeometryCache.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {
struct MaterialTestConstants { std::array<float, 2> uv{0.5f, 0.5f}; uint32_t mode = 0; uint32_t reserved = 0; };
struct MaterialTestGeometry { uint32_t vertexSrv = UINT32_MAX; uint32_t stride = 60; uint32_t firstVertex = 0; uint32_t vertexCount = 3; };

class RayTextureTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Ray texture test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        if (!m_bundle.renderer->GetCapabilities().inlineRayQuery) GTEST_SKIP() << "Inline RayQuery unavailable";
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        m_shader = m_resources->LoadShader((root / "../../Projects/Tests/Graphics/Shaders/RayMaterialRead.cs.hlsl").lexically_normal().generic_string());
        m_copy = m_resources->LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_output = m_resources->CreateComputeTexture(4, 1);
        m_target = m_resources->CreateRenderTarget(4, 1, {1, renderer::Format::RGBA16F, false});
        m_pipeline = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(m_shader && m_copy && m_output && m_target && m_pipeline);
        core::Logger::AddSink(this);
    }
    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        if (m_resources) m_cache.Release(*m_resources);
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
        const std::string text = entry.message;
        if ((entry.level == core::LogLevel::WARNING || entry.level == core::LogLevel::LOG_ERROR)
            && (text.find("[WARNING]") != std::string::npos || text.find("[ERROR]") != std::string::npos
                || text.find("[CORRUPTION]") != std::string::npos)) ++m_validationFailures;
    }
    renderer::ResourceHandle<renderer::TextureTag> Texture(const std::array<uint8_t, 4>& rgba)
    {
        return m_resources->CreateTexture(rgba.data(), 1, 1);
    }
    renderer::RaySurfaceRecord Surface(std::array<renderer::ResourceHandle<renderer::TextureTag>, 5> textures = {})
    {
        renderer::RaySurfaceRecord result;
        result.supported = 1;
        for (uint32_t i = 0; i < textures.size(); ++i) {
            if (!textures[i]) continue;
            result.textureMask |= 1u << i;
            result.textureSrv[i] = m_resources->Get(textures[i])->GetBindlessIndex();
        }
        return result;
    }
    std::array<uint8_t, 96> Params()
    {
        std::array<uint8_t, 96> params{};
        const auto put = [&](size_t offset, float value) { std::memcpy(params.data() + offset, &value, sizeof(value)); };
        for (size_t offset : {0u, 4u, 8u, 12u, 24u, 28u, 48u, 52u}) put(offset, 1);
        put(20, 0.5f);
        return params;
    }
    void Begin()
    {
        m_resources->AdvanceFrame();
        m_bundle.renderer->BeginFrame();
        m_frameOpen = true;
    }
    std::vector<float> Read(std::vector<renderer::RaySurfaceRecord> surfaces,
        std::vector<renderer::ResourceHandle<renderer::TextureTag>> textures,
        MaterialTestConstants constants = {}, renderer::ResourceHandle<renderer::AccelerationStructureTag> scene = {},
        const std::vector<MaterialTestGeometry>& geometries = {},
        const std::vector<renderer::ResourceHandle<renderer::BufferTag>>& buffers = {})
    {
        const auto table = m_resources->CreateStructuredBuffer(surfaces.data(), static_cast<uint32_t>(surfaces.size()), sizeof(renderer::RaySurfaceRecord));
        const auto cb = m_resources->CreateConstantBuffer(sizeof(constants));
        m_resources->Update(cb, &constants, sizeof(constants));
        renderer::ComputeCall call;
        call.shader = m_shader;
        call.constantBuffers[0] = cb;
        call.srvBuffers[14] = table;
        call.uavOutputs[0] = m_output;
        call.indirectReadTextures = std::move(textures);
        call.indirectReadBuffers = buffers;
        call.accelerationStructures[0] = scene;
        if (!geometries.empty()) call.srvBuffers[15] = m_resources->CreateStructuredBuffer(geometries.data(),
            static_cast<uint32_t>(geometries.size()), sizeof(MaterialTestGeometry));
        if (!m_frameOpen) Begin();
        m_bundle.renderer->Dispatch(call, *m_resources);
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget(m_target, *m_resources);
        renderer::DrawCall draw;
        draw.shader = m_copy; draw.pipelineState = m_pipeline; draw.vertexCount = 3; draw.textures[5] = m_output;
        device.Submit(draw, *m_resources);
        device.SetRenderTarget({}, *m_resources);
        device.EndFrame(); m_frameOpen = false;
        std::vector<float> result;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_target, *m_resources, result, width, height));
        EXPECT_EQ(width, 4u); EXPECT_EQ(height, 1u);
        return result;
    }
    struct CandidateScene {
        renderer::ResourceHandle<renderer::AccelerationStructureTag> top;
        std::vector<MaterialTestGeometry> geometries;
        std::vector<renderer::ResourceHandle<renderer::BufferTag>> buffers;
    };
    CandidateScene AlphaScene(bool opaqueFirst)
    {
        CandidateScene result;
        renderer::AccelerationStructureDesc top;
        top.kind = renderer::AccelerationStructureKind::TOP_LEVEL;
        std::vector<renderer::ResourceHandle<renderer::AccelerationStructureTag>> bottoms;
        for (uint32_t i = 0; i < 2; ++i) {
            std::array<renderer::Vertex, 3> vertices{};
            const float z = i ? 4.0f : 2.0f;
            vertices[0].position = {-4, -4, z}; vertices[1].position = {0, 4, z}; vertices[2].position = {4, -4, z};
            for (auto& vertex : vertices) { vertex.normal = {0, 0, -1}; vertex.tangent = {1, 0, 0}; vertex.uv = {0.25f, 0.5f}; }
            const auto vb = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
            result.buffers.push_back(vb);
            result.geometries.push_back({m_resources->Get(vb)->GetBindlessSrvIndex(), sizeof(renderer::Vertex), 0, 3});
            renderer::AccelerationStructureDesc bottom;
            bottom.geometries.push_back({vb, {}, 0, 3, 0, 0, 0, i != 0 || opaqueFirst});
            const auto handle = m_resources->CreateAccelerationStructure(bottom);
            bottoms.push_back(handle);
            top.instances.push_back({handle, math::Matrix4::Identity(), i, 1, true});
        }
        result.top = m_resources->CreateAccelerationStructure(top);
        Begin();
        for (auto bottom : bottoms) EXPECT_TRUE(m_bundle.renderer->BuildAccelerationStructure(bottom, *m_resources));
        EXPECT_TRUE(m_bundle.renderer->BuildAccelerationStructure(result.top, *m_resources));
        return result;
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::RayGeometryCache m_cache;
    renderer::ResourceHandle<renderer::ShaderTag> m_shader, m_copy;
    renderer::ResourceHandle<renderer::TextureTag> m_output;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
    uint32_t m_validationFailures = 0;
};

TEST_F(RayTextureTest, SamplesRasterEncodedColorTintAndGlTfMetalRoughReplacement)
{
    const auto albedo = Texture({128, 64, 255, 128});
    const auto mr = Texture({0, 153, 204, 255});
    const auto ao = Texture({64, 0, 0, 255});
    auto surface = Surface({albedo, {}, mr, {}, ao});
    surface.baseColor = {0.25f, 0.5f, 0.75f, 0.5f};
    surface.metallic = 0.1f; surface.roughness = 0.2f; surface.occlusionStrength = 0.5f;
    const auto result = Read({surface}, {albedo, mr, ao});
    ASSERT_EQ(result.size(), 16u);
    EXPECT_NEAR(result[0], std::pow(128.0f / 255, 2.2f) * 0.25f, 0.0002f);
    EXPECT_NEAR(result[1], std::pow(64.0f / 255, 2.2f) * 0.5f, 0.0002f);
    EXPECT_NEAR(result[2], 0.75f, 0.0005f); EXPECT_NEAR(result[3], 128.0f / 255 * 0.5f, 0.0003f);
    EXPECT_NEAR(result[4], 0.8f, 0.0005f); EXPECT_NEAR(result[5], 0.6f, 0.0005f);
    EXPECT_NEAR(result[6], 0.5f + 0.5f * 64 / 255, 0.0005f); EXPECT_NEAR(result[7], 1, 0.0001f);
}
TEST_F(RayTextureTest, AppliesUvTransformWrapAndExplicitMipWithoutDerivatives)
{
    const std::array<uint8_t, 8> pixels{255, 255, 255, 255, 255, 0, 0, 255};
    const auto texture = m_resources->CreateTexture(pixels.data(), 2, 1);
    auto surface = Surface({texture});
    surface.uvTiling = {2, 1}; surface.uvOffset = {-0.25f, 0};
    auto white = Read({surface}, {texture}, {{0.25f, 0.5f}});
    EXPECT_NEAR(white[1], 1, 0.0005f);
    auto red = Read({surface}, {texture}, {{0.5f, 0.5f}});
    EXPECT_NEAR(red[0], 1, 0.0005f); EXPECT_NEAR(red[1], 0, 0.0001f);
    surface.uvOffset[0] += 0.5f;
    auto wrapped = Read({surface}, {texture}, {{0.5f, 0.5f}});
    EXPECT_NEAR(wrapped[1], 1, 0.0005f);
    const std::array<uint8_t, 4> mipPixel{0, 255, 0, 255};
    const std::array<renderer::TextureMipData, 2> mips{{{pixels.data(), 2, 1}, {mipPixel.data(), 1, 1}}};
    const auto mipTexture = m_resources->CreateTextureWithMips(mips.data(), 2);
    surface = Surface({mipTexture}); surface.explicitTextureLod = 1;
    auto mip = Read({surface}, {mipTexture});
    EXPECT_NEAR(mip[0], 0, 0.0001f); EXPECT_NEAR(mip[1], 1, 0.0005f);
}
TEST_F(RayTextureTest, AppliesNormalStrengthWithoutChangingGeometricNormal)
{
    const auto texture = Texture({255, 128, 128, 255});
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 5> textures{};
    textures[1] = texture;
    auto surface = Surface(textures);
    auto tilted = Read({surface}, {texture});
    EXPECT_GT(tilted[8], 0.999f); EXPECT_NEAR(tilted[10], 0.50196f, 0.0005f);
    surface.normalStrength = 0;
    auto flat = Read({surface}, {texture});
    EXPECT_NEAR(flat[8], 0.5f, 0.0001f); EXPECT_NEAR(flat[9], 0.5f, 0.0001f); EXPECT_NEAR(flat[10], 1, 0.0001f);
    surface.normalStrength = -1;
    auto below = Read({surface}, {texture});
    for (uint32_t channel = 0; channel < 3; ++channel) EXPECT_FLOAT_EQ(below[8 + channel], flat[8 + channel]);
    surface.normalStrength = 2;
    auto above = Read({surface}, {texture});
    for (uint32_t channel = 0; channel < 3; ++channel) EXPECT_FLOAT_EQ(above[8 + channel], tilted[8 + channel]);
}
TEST_F(RayTextureTest, FallsBackToTheOriginalNormalForExactCancellationAndDegenerateVertexInputs)
{
    const auto normalTexture = m_resources->CreateComputeTexture(4, 1);
    ASSERT_TRUE(normalTexture);
    const auto output = m_output;
    m_output = normalTexture;
    MaterialTestConstants seed; seed.mode = 4;
    Read({Surface()}, {}, seed);
    m_output = output;
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 5> textures{};
    textures[1] = normalTexture;
    auto surface = Surface(textures);
    surface.normalStrength = 0.5f;
    const auto cancelled = Read({surface}, {normalTexture});
    ASSERT_EQ(cancelled.size(), 16u);
    EXPECT_NEAR(cancelled[8], 0.5f, 0.0001f); EXPECT_NEAR(cancelled[9], 0.5f, 0.0001f);
    EXPECT_NEAR(cancelled[10], 1, 0.0001f); EXPECT_NEAR(cancelled[7], 1, 0.0001f);
    MaterialTestConstants degenerate; degenerate.mode = 5;
    const auto fallback = Read({Surface()}, {}, degenerate);
    ASSERT_EQ(fallback.size(), 16u);
    EXPECT_NEAR(fallback[8], 0.5f, 0.0001f); EXPECT_NEAR(fallback[9], 1, 0.0001f);
    EXPECT_NEAR(fallback[10], 0.5f, 0.0001f); EXPECT_NEAR(fallback[7], 1, 0.0001f);
}
TEST_F(RayTextureTest, RejectsAlphaCandidatesBeforeCommitForPrimaryAndShadowRays)
{
    const auto hole = Texture({255, 255, 255, 0});
    auto first = Surface({hole}); first.alphaCutoff = 0.5f;
    const auto second = Surface();
    auto scene = AlphaScene(false);
    auto primary = Read({first, second}, {hole}, {{0.5f, 0.5f}, 1}, scene.top, scene.geometries, scene.buffers);
    EXPECT_NEAR(primary[0], 1, 0.0001f); EXPECT_NEAR(primary[1], 4, 0.0001f);
    auto shadow = Read({first, second}, {hole}, {{0.5f, 0.5f}, 2}, scene.top, scene.geometries, scene.buffers);
    EXPECT_NEAR(shadow[0], 1, 0.0001f); EXPECT_NEAR(shadow[1], 4, 0.0001f);
    first.alphaCutoff = 0;
    auto equality = Read({first, second}, {hole}, {{0.5f, 0.5f}, 1}, scene.top, scene.geometries, scene.buffers);
    EXPECT_NEAR(equality[0], 0, 0.0001f); EXPECT_NEAR(equality[1], 2, 0.0001f);
}
TEST_F(RayTextureTest, DistinguishesOpaqueBlasVariantFromCandidateAlphaVariant)
{
    const auto hole = Texture({255, 255, 255, 0});
    auto first = Surface({hole}); first.alphaCutoff = 0.5f;
    auto scene = AlphaScene(true);
    auto opaque = Read({first, Surface()}, {hole}, {{0.5f, 0.5f}, 1}, scene.top, scene.geometries, scene.buffers);
    EXPECT_NEAR(opaque[0], 0, 0.0001f); EXPECT_NEAR(opaque[1], 2, 0.0001f);
}
TEST_F(RayTextureTest, TracksSameHandleReplacementVersionAndPublishesNewDescriptor)
{
    const auto initial = Texture({255, 0, 0, 255});
    const auto replacement = Texture({0, 255, 0, 255});
    EXPECT_TRUE(m_resources->Get(initial)->IsRayMaterialTexture());
    EXPECT_EQ(m_resources->Get(initial)->GetContentVersion(), 1u);
    const auto oldSurface = Surface({initial});
    ASSERT_TRUE(m_resources->ReplaceTextureContents(initial, replacement));
    EXPECT_EQ(m_resources->Get(initial)->GetContentVersion(), 2u);
    EXPECT_EQ(m_resources->Get(replacement), nullptr);
    const auto current = Read({Surface({initial})}, {initial});
    EXPECT_NEAR(current[0], 0, 0.0001f); EXPECT_NEAR(current[1], 1, 0.0005f);
    EXPECT_NE(oldSurface.textureSrv[0], Surface({initial}).textureSrv[0]);
}
TEST_F(RayTextureTest, RejectsStaleIndirectTexturesBeforeWriting)
{
    const auto valid = Texture({0, 255, 0, 255});
    const auto stale = Texture({255, 0, 0, 255});
    auto before = Read({Surface({valid})}, {valid});
    const auto oldSurface = Surface({stale});
    m_resources->Release(stale);
    /// @note Expected stale dependency diagnostic must reject the dispatch before the freed descriptor can be read.
    auto after = Read({oldSurface}, {stale});
    ASSERT_EQ(before.size(), after.size());
    for (size_t i = 0; i < before.size(); ++i) EXPECT_FLOAT_EQ(before[i], after[i]);
}
TEST_F(RayTextureTest, RejectsMissingDynamicInputsAndSupportsImmutableTexturedEmission)
{
    auto params = Params();
    renderer::Material material;
    const auto missing = material.ResolveRaySurface(params, true, 1, false, false, *m_resources);
    EXPECT_FALSE(missing.standardSurfaceSupported);
    EXPECT_EQ(missing.issue, renderer::SurfaceMaterialIssue::TEXTURE_UNAVAILABLE);
    const auto dynamic = m_resources->CreateDynamicTexture(1, 1, renderer::DynamicTextureFormat::RGBA8);
    const std::array<uint8_t, 4> pixel{255, 0, 0, 255};
    EXPECT_EQ(m_resources->Get(dynamic)->GetContentVersion(), 1u);
    ASSERT_TRUE(m_resources->Get(dynamic)->UpdateRegion(0, 0, 1, 1, pixel.data(), 4));
    EXPECT_EQ(m_resources->Get(dynamic)->GetContentVersion(), 2u);
    EXPECT_FALSE(m_resources->Get(dynamic)->IsRayMaterialTexture());
    material.textures = {dynamic};
    EXPECT_FALSE(material.ResolveRaySurface(params, true, 1, false, false, *m_resources).standardSurfaceSupported);
    material.textures = {{}, {}, {}, Texture({255, 255, 255, 255})};
    const auto emissive = material.ResolveRaySurface(params, true, 8, false, false, *m_resources);
    EXPECT_TRUE(emissive.standardSurfaceSupported);
    EXPECT_EQ(emissive.issue, renderer::SurfaceMaterialIssue::NONE);
    material.textures = {Texture({255, 255, 255, 255})};
    auto before = material.ResolveRaySurface(params, true, 1, false, false, *m_resources);
    EXPECT_TRUE(before.standardSurfaceSupported);
    const auto newContent = Texture({128, 128, 128, 255});
    ASSERT_TRUE(m_resources->ReplaceTextureContents(material.textures[0], newContent));
    auto after = material.ResolveRaySurface(params, true, 1, false, false, *m_resources);
    EXPECT_FALSE(before == after);
}
TEST_F(RayTextureTest, SamplesTheSameLinearTexturedEmissionForHitAndNeeEvaluation)
{
    const auto texture = Texture({128, 64, 255, 255});
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 5> textures{};
    textures[3] = texture;
    auto surface = Surface(textures);
    surface.emission = {2, 3, 4};
    MaterialTestConstants constants; constants.mode = 3;
    const auto result = Read({surface}, {texture}, constants);
    ASSERT_EQ(result.size(), 16u);
    EXPECT_NEAR(result[0], 2 * std::pow(128.0f / 255, 2.2f), 0.001f);
    EXPECT_NEAR(result[1], 3 * std::pow(64.0f / 255, 2.2f), 0.001f);
    EXPECT_NEAR(result[2], 4, 0.001f);
}

} /// @note namespace
} /// @note namespace fbzz::tests
