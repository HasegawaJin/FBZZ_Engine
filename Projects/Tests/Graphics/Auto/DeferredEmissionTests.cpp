/// @file    DeferredEmissionTests.cpp
/// @brief   標準 GBuffer と DeferredLighting の発光を実 GPU の HDR 読み戻しで検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

class DeferredEmissionTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Deferred emission test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
    }

    void TearDown() override
    {
        core::Logger::RemoveSink(this);
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        testkit::Fixture::TearDown();
    }

    bool m_allowHdrCubeClearMetadataWarning = false;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
private:
    void OnLog(const core::LogEntry& entry) override
    {
        /// @note The existing HDR cube fixture clears to (4,4,4,1), unlike its black optimized-clear metadata; only that scoped performance warning is expected.
        if (m_allowHdrCubeClearMetadataWarning && entry.message.starts_with("  [WARNING] (id=820) ")) return;
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }
};

/// @note Constants.hlsli の MaterialConstants (b2)。GBuffer の材質入力は 96 バイト。
struct GBufferMaterialCB {
    math::Vector4 albedo{0, 0, 0, 1};
    float metallic = 0;
    float roughness = 0.5f;
    float normalStrength = 1;
    float occlusionStrength = 1;
    math::Vector3 emissiveColor{2, 0.125f, 0.5625f};
    float emissiveScale = 4;
    math::Vector2 uvTiling{1, 1};
    math::Vector2 uvOffset{};
    float alphaCutoff = 0.5f;
    float padding0[3]{};
    uint32_t textureMask = 0;
    float padding1[3]{};
};
static_assert(sizeof(GBufferMaterialCB) == 96);

TEST_F(DeferredEmissionTest, SlopedReceiverAvoidsPcfSelfShadowAndPreservesBlockerShadow)
{
    constexpr uint32_t extent = 64;
    constexpr uint32_t shadowExtent = 16;
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, extent, extent);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        const auto shaderRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        const auto loadShader = [&](const char* relative) {
            return resources.LoadShader((shaderRoot / relative).generic_string());
        };
        const auto shadowShader = loadShader("Pipeline/Shadow/ShadowMap.hlsl");
        const auto geometryShader = loadShader("Pipeline/Deferred/GBuffer.hlsl");
        const auto lightingShader = loadShader("Pipeline/Deferred/DeferredLighting.hlsl");
        const auto shadowTarget = resources.CreateRenderTarget(shadowExtent, shadowExtent, 0);
        const auto gbuffer = resources.CreateRenderTarget(extent, extent,
            renderer::CameraDepthTargetDesc(renderer::GBUFFER_COLOR_COUNT));
        const auto hdr = resources.CreateRenderTarget(extent, extent,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        const auto geometryState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        const auto lightingState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(shadowShader && geometryShader && lightingShader && shadowTarget && gbuffer && hdr
            && geometryState && lightingState);

        /// @note The real rasterized receiver rises 1.2m per horizontal metre; no blocker exists in the self-shadow cases.
        const math::Vector3 normal = math::Vector3{1.2f, 0, -1}.Normalized();
        const math::Vector3 tangent = math::Vector3{1, 0, 1.2f}.Normalized();
        const renderer::Vertex receiver[] = {
            {{-1, -1, 1.8f}, normal, tangent, {0, 1}},
            {{-1,  1, 1.8f}, normal, tangent, {0, 0}},
            {{ 1, -1, 4.2f}, normal, tangent, {1, 1}},
            {{ 1, -1, 4.2f}, normal, tangent, {1, 1}},
            {{-1,  1, 1.8f}, normal, tangent, {0, 0}},
            {{ 1,  1, 4.2f}, normal, tangent, {1, 0}},
        };
        /// @note A separate opaque blocker is closer to the light; it changes only the shadow producer, not the visible receiver.
        const renderer::Vertex blocker[] = {
            {{-0.7f, -0.7f, 1}, {0, 0, -1}, {1, 0, 0}, {0, 1}},
            {{-0.7f,  0.7f, 1}, {0, 0, -1}, {1, 0, 0}, {0, 0}},
            {{ 0.7f, -0.7f, 1}, {0, 0, -1}, {1, 0, 0}, {1, 1}},
            {{ 0.7f, -0.7f, 1}, {0, 0, -1}, {1, 0, 0}, {1, 1}},
            {{-0.7f,  0.7f, 1}, {0, 0, -1}, {1, 0, 0}, {0, 0}},
            {{ 0.7f,  0.7f, 1}, {0, 0, -1}, {1, 0, 0}, {1, 0}},
        };
        const auto receiverBuffer = resources.CreateVertexBuffer(receiver, sizeof(receiver), sizeof(renderer::Vertex));
        const auto blockerBuffer = resources.CreateVertexBuffer(blocker, sizeof(blocker), sizeof(renderer::Vertex));
        ASSERT_TRUE(receiverBuffer && blockerBuffer);

        renderer::Camera camera;
        camera.m_position = {};
        camera.m_projection = renderer::ProjectionMode::Orthographic;
        camera.m_orthoHeight = 2;
        camera.m_aspect = 1;
        camera.m_near = 0.1f;
        camera.m_far = 20;
        const auto frame = renderer::MakeCameraFrameCB(camera, 0, 0);
        auto lightFrame = frame;
        lightFrame.projection = camera.GetProjectionMatrix();
        lightFrame.viewProjection = camera.GetViewProjection();
        renderer::PerObjectCB object{};
        object.world = object.worldInvTranspose = math::Matrix4::Identity();
        GBufferMaterialCB material;
        material.albedo = {0.5f, 0.5f, 0.5f, 1};
        material.roughness = 0.8f;
        material.emissiveScale = 0;
        renderer::LightConstantsCB light{};
        light.lightDir = {0, 0, 1};
        light.lightColor = {1, 1, 1};
        light.lightIntensity = 1;
        /// @note Isolate direct visibility; ambient radiance remains even behind an opaque blocker.
        light.ambientColor = {};
        renderer::ShadowConstantsCB shadow{};
        shadow.lightViewProjection = lightFrame.viewProjection;
        shadow.shadowMapTexelSize[0] = shadow.shadowMapTexelSize[1] = 1.0f / shadowExtent;
        shadow.shadowBias = 0.005f / (camera.m_far - camera.m_near);
        shadow.cascadeCount = 1;
        renderer::PostProcCB post{};
        renderer::AdvancedGraphicsCB advanced{};
        const auto frameCB = resources.CreateConstantBuffer(sizeof(frame));
        const auto lightFrameCB = resources.CreateConstantBuffer(sizeof(lightFrame));
        const auto objectCB = resources.CreateConstantBuffer(sizeof(object));
        const auto materialCB = resources.CreateConstantBuffer(sizeof(material));
        const auto lightCB = resources.CreateConstantBuffer(sizeof(light));
        const auto shadowCB = resources.CreateConstantBuffer(sizeof(shadow));
        const auto postCB = resources.CreateConstantBuffer(sizeof(post));
        const auto advancedCB = resources.CreateConstantBuffer(sizeof(advanced));
        ASSERT_TRUE(frameCB && lightFrameCB && objectCB && materialCB && lightCB && shadowCB && postCB && advancedCB);

        const auto render = [&](int pcfRadius, float strength, bool hasBlocker) {
            resources.AdvanceFrame();
            device.BeginFrame();
            shadow.shadowPcfRadius = pcfRadius;
            shadow.shadowStrength = strength;
            resources.Update(frameCB, &frame, sizeof(frame));
            resources.Update(lightFrameCB, &lightFrame, sizeof(lightFrame));
            resources.Update(objectCB, &object, sizeof(object));
            resources.Update(materialCB, &material, sizeof(material));
            resources.Update(lightCB, &light, sizeof(light));
            resources.Update(shadowCB, &shadow, sizeof(shadow));
            resources.Update(postCB, &post, sizeof(post));
            resources.Update(advancedCB, &advanced, sizeof(advanced));

            device.SetRenderTarget(shadowTarget, resources);
            device.ClearDepth();
            renderer::DrawCall caster;
            caster.shader = shadowShader;
            caster.pipelineState = geometryState;
            caster.vertexBuffer = receiverBuffer;
            caster.vertexCount = 6;
            caster.constantBuffers[0] = lightFrameCB;
            caster.constantBuffers[1] = objectCB;
            device.Submit(caster, resources);
            if (hasBlocker) {
                caster.vertexBuffer = blockerBuffer;
                device.Submit(caster, resources);
            }

            device.SetRenderTarget(gbuffer, resources);
            device.Clear({0, 0, 0, 0});
            renderer::DrawCall geometry;
            geometry.shader = geometryShader;
            geometry.pipelineState = geometryState;
            geometry.vertexBuffer = receiverBuffer;
            geometry.vertexCount = 6;
            geometry.constantBuffers[0] = frameCB;
            geometry.constantBuffers[1] = objectCB;
            geometry.constantBuffers[2] = materialCB;
            geometry.constantBuffers[8] = advancedCB;
            device.Submit(geometry, resources);

            device.SetRenderTarget(hdr, resources);
            device.Clear({0, 0, 0, 0});
            renderer::DrawCall lighting;
            lighting.shader = lightingShader;
            lighting.pipelineState = lightingState;
            lighting.vertexCount = 3;
            lighting.constantBuffers[0] = frameCB;
            lighting.constantBuffers[3] = lightCB;
            lighting.constantBuffers[4] = shadowCB;
            lighting.constantBuffers[5] = postCB;
            lighting.constantBuffers[8] = advancedCB;
            lighting.textures[3] = resources.GetColorTexture(gbuffer, 2);
            lighting.textures[5] = resources.GetColorTexture(gbuffer, 0);
            lighting.textures[6] = resources.GetColorTexture(gbuffer, 1);
            lighting.textures[7] = resources.GetDepthTexture(gbuffer);
            /// @note DeferredLighting.hlsl binds directional depth at TEX_SHADOW_SLOT (t8).
            lighting.textures[8] = resources.GetDepthTexture(shadowTarget);
            device.Submit(lighting, resources);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(hdr, resources, pixels, width, height));
            EXPECT_EQ(width, extent);
            EXPECT_EQ(height, extent);
            return pixels;
        };

        for (int radius : {2, 3}) {
            SCOPED_TRACE(radius);
            const auto unshadowed = render(radius, 0, false);
            const auto selfShadow = render(radius, 1, false);
            const auto blocked = render(radius, 1, true);
            ASSERT_EQ(unshadowed.size(), extent * extent * 4u);
            ASSERT_EQ(selfShadow.size(), unshadowed.size());
            ASSERT_EQ(blocked.size(), unshadowed.size());
            /// @note Ignore raster borders; the entire checked region is the same isolated planar receiver in all three draws.
            for (uint32_t y = 16; y < 48; ++y) {
                for (uint32_t x = 16; x < 48; ++x) {
                    const size_t offset = (y * extent + x) * 4;
                    for (size_t channel = 0; channel < 3; ++channel) {
                        ASSERT_TRUE(std::isfinite(unshadowed[offset + channel]));
                        ASSERT_GT(unshadowed[offset + channel], 0.05f);
                        EXPECT_TRUE(std::isfinite(selfShadow[offset + channel]));
                        EXPECT_TRUE(std::isfinite(blocked[offset + channel]));
                        EXPECT_NEAR(selfShadow[offset + channel], unshadowed[offset + channel], 0.001f)
                            << "pixel=" << x << ',' << y << " channel=" << channel;
                    }
                }
            }
            /// @note The blocker covers every central PCF tap; preserving its strong shadow rejects a fix that simply disables occlusion.
            for (uint32_t y = 28; y < 36; ++y) {
                for (uint32_t x = 28; x < 36; ++x) {
                    const size_t offset = (y * extent + x) * 4;
                    for (size_t channel = 0; channel < 3; ++channel)
                        EXPECT_LT(blocked[offset + channel], unshadowed[offset + channel] * 0.1f)
                            << "pixel=" << x << ',' << y << " channel=" << channel;
                }
            }
        }
        resources.Reset();
    }
    device.Shutdown();
}

TEST_F(DeferredEmissionTest, PreservesHdrTextureAndOcclusionContracts)
{
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    /// @note Hidden-window readback must keep recording after Present reports occlusion.
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        const auto shaderRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        const auto loadShader = [&](const char* relative) {
            return resources.LoadShader((shaderRoot / relative).generic_string());
        };
        const auto geometryShader = loadShader("Pipeline/Deferred/GBuffer.hlsl");
        const auto lightingShader = loadShader("Pipeline/Deferred/DeferredLighting.hlsl");
        ASSERT_TRUE(geometryShader.IsValid());
        ASSERT_TRUE(lightingShader.IsValid());
        /// @note 共有出力を使う全変種と、独自構造体の地形も新しい MRT 契約でコンパイルできること。
        for (const auto* variant : {"Pipeline/Deferred/GBufferInstanced.hlsl",
                "Pipeline/Deferred/GBufferSkinned.hlsl", "Terrain/TerrainGBuffer.hlsl",
                "Fiber/FiberShellGBuffer.hlsl", "Fiber/FiberShellSkinnedGBuffer.hlsl",
                "Fiber/FiberFinGBuffer.hlsl", "Fiber/FiberFinSkinnedGBuffer.hlsl",
                "Fiber/FiberBladeGBuffer.hlsl"}) {
            EXPECT_TRUE(loadShader(variant).IsValid()) << variant;
        }
        const auto gbuffer = resources.CreateRenderTarget(64, 64,
            renderer::CameraDepthTargetDesc(renderer::GBUFFER_COLOR_COUNT));
        const auto hdr = resources.CreateRenderTarget(64, 64,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        const auto rayResult = resources.CreateRenderTarget(64, 64,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        const auto environment = resources.CreateCubemapRenderTarget(1);
        const auto geometryState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        const auto lightingState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(gbuffer.IsValid());
        ASSERT_TRUE(hdr.IsValid());
        ASSERT_TRUE(rayResult.IsValid());
        ASSERT_TRUE(environment.IsValid());
        ASSERT_TRUE(geometryState.IsValid());
        ASSERT_TRUE(lightingState.IsValid());
        const renderer::Vertex vertices[] = {
            {{-0.8f, -0.8f, 0.5f}, {0, 0, 1}, {1, 0, 0}, {0, 1}},
            {{0, 0.8f, 0.5f}, {0, 0, 1}, {1, 0, 0}, {0.5f, 0}},
            {{0.8f, -0.8f, 0.5f}, {0, 0, 1}, {1, 0, 0}, {1, 1}},
        };
        const auto vertexBuffer = resources.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(renderer::Vertex));
        ASSERT_TRUE(vertexBuffer.IsValid());
        const uint8_t gray[] = {128, 64, 255, 255};
        const uint8_t black[] = {0, 0, 0, 255};
        const auto emissionTexture = resources.CreateTexture(gray, 1, 1);
        const auto blackTexture = resources.CreateTexture(black, 1, 1);
        const uint8_t brdfScale[] = {255, 0, 0, 255};
        const auto brdfTexture = resources.CreateTexture(brdfScale, 1, 1);
        ASSERT_TRUE(emissionTexture.IsValid());
        ASSERT_TRUE(blackTexture.IsValid());
        ASSERT_TRUE(brdfTexture.IsValid());

        renderer::PerFrameCB frame{};
        frame.view = frame.projection = frame.viewProjection = frame.invViewProjection = math::Matrix4::Identity();
        frame.cameraPos = {0, 0, 2};
        frame.nearZ = 0.1f;
        frame.farZ = 10;
        renderer::PerObjectCB object{};
        object.world = object.worldInvTranspose = math::Matrix4::Identity();
        renderer::LightConstantsCB light{};
        light.ambientColor = {};
        light.lightDir = {0, 0, -1};
        light.lightIntensity = 0;
        renderer::ShadowConstantsCB shadow{};
        renderer::PostProcCB post{};
        post.ssaoIntensity = 1;
        renderer::AdvancedGraphicsCB advanced{};
        advanced.contactShadowStrength = 1;
        const auto createConstants = [&](const auto& value) {
            return resources.CreateConstantBuffer(sizeof(value));
        };
        const auto frameCB = createConstants(frame);
        const auto objectCB = createConstants(object);
        const auto lightCB = createConstants(light);
        const auto shadowCB = createConstants(shadow);
        const auto postCB = createConstants(post);
        const auto advancedCB = createConstants(advanced);
        const auto materialCB = resources.CreateConstantBuffer(sizeof(GBufferMaterialCB));

        const auto drawAndRead = [&](GBufferMaterialCB material, bool occlude,
                                     math::Vector4 reflected = {}, bool rayEnabled = false) {
            resources.AdvanceFrame();
            device.BeginFrame();
            device.SetRenderTarget(rayResult, resources);
            device.Clear(reflected);
            m_allowHdrCubeClearMetadataWarning = true;
            for (uint32_t face = 0; face < 6; ++face) {
                device.SetRenderTargetFace(environment, face, 0, resources);
                device.Clear({4, 4, 4, 1});
            }
            post.rayReflectionEnabled = rayEnabled ? 1.0f : 0.0f;
            resources.Update(frameCB, &frame, sizeof(frame));
            resources.Update(lightCB, &light, sizeof(light));
            resources.Update(shadowCB, &shadow, sizeof(shadow));
            resources.Update(postCB, &post, sizeof(post));
            resources.Update(advancedCB, &advanced, sizeof(advanced));
            device.SetRenderTarget(gbuffer, resources);
            device.SetViewport(0, 0, 64, 64);
            device.Clear({0, 0, 0, 0});
            resources.Update(objectCB, &object, sizeof(object));
            resources.Update(materialCB, &material, sizeof(material));
            renderer::DrawCall geometry;
            geometry.shader = geometryShader;
            geometry.pipelineState = geometryState;
            geometry.vertexBuffer = vertexBuffer;
            geometry.vertexCount = 3;
            geometry.constantBuffers[0] = frameCB;
            geometry.constantBuffers[1] = objectCB;
            geometry.constantBuffers[2] = materialCB;
            geometry.constantBuffers[8] = advancedCB;
            geometry.textures[3] = emissionTexture;
            device.Submit(geometry, resources);
            if (occlude) {
                auto closer = object;
                closer.world.m[2][3] = 0.25f;
                material.emissiveScale = 0;
                resources.Update(objectCB, &closer, sizeof(closer));
                resources.Update(materialCB, &material, sizeof(material));
                device.Submit(geometry, resources);
            }
            device.SetRenderTarget(hdr, resources);
            device.Clear({0, 0, 0, 0});
            renderer::DrawCall lighting;
            lighting.shader = lightingShader;
            lighting.pipelineState = lightingState;
            lighting.vertexCount = 3;
            lighting.constantBuffers[0] = frameCB;
            lighting.constantBuffers[3] = lightCB;
            lighting.constantBuffers[4] = shadowCB;
            lighting.constantBuffers[5] = postCB;
            lighting.constantBuffers[8] = advancedCB;
            /// @note 未指定 b12 の末尾にある legacyShapedLightCount もゼロとして読めることを検証する。
            lighting.textures[3] = resources.GetColorTexture(gbuffer, 2);
            lighting.textures[5] = resources.GetColorTexture(gbuffer, 0);
            lighting.textures[6] = resources.GetColorTexture(gbuffer, 1);
            lighting.textures[7] = resources.GetDepthTexture(gbuffer);
            lighting.textures[9] = blackTexture;
            lighting.textures[16] = resources.GetCubemapTexture(environment);
            lighting.textures[17] = resources.GetCubemapTexture(environment);
            lighting.textures[18] = brdfTexture;
            if (rayEnabled) lighting.textures[20] = resources.GetColorTexture(rayResult, 0);
            lighting.textures[24] = blackTexture;
            device.Submit(lighting, resources);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            if (!device.CaptureRenderTargetToLinearRGBA(hdr, resources, pixels, width, height)
                    || width != 64 || height != 64 || pixels.size() != 64 * 64 * 4) {
                ADD_FAILURE() << "Deferred HDR readback failed";
                return math::Vector3{};
            }
            const auto offset = (32 * 64 + 32) * 4;
            EXPECT_NEAR(pixels[0], 0, 0.001f);
            return math::Vector3{pixels[offset], pixels[offset + 1], pixels[offset + 2]};
        };
        GBufferMaterialCB material;
        EXPECT_VEC3_NEAR(drawAndRead(material, false), (math::Vector3{8, 0.5f, 2.25f}), 0.01f);
        material.textureMask = 1u << 3;
        /// @note Color.hlsli の既存 gamma 2.2 近似に合わせる。HDR の 1 超えをクランプしない。
        EXPECT_VEC3_NEAR(drawAndRead(material, false), (math::Vector3{1.756158f, 0.02388788f, 2.25f}), 0.01f);
        material.textureMask = 0;
        EXPECT_VEC3_NEAR(drawAndRead(material, true), math::Vector3{}, 0.001f);
        material.albedo.w = 0.25f;
        EXPECT_VEC3_NEAR(drawAndRead(material, false), math::Vector3{}, 0.001f);
        material.albedo.w = 1;
        material.emissiveScale = 0;
        EXPECT_VEC3_NEAR(drawAndRead(material, false), math::Vector3{}, 0.001f);

        material.albedo = {0.5f, 0.5f, 0.5f, 1};
        light.lightColor = {1, 1, 1};
        /// @note HDR の半精度量子化で小さな追加項が失われない光量に保つ。
        light.lightIntensity = 1;
        advanced.contactShadowStrength = 0;
        const auto direct = drawAndRead(material, false);
        EXPECT_GT(direct.x, 0.0f);
        material.emissiveScale = 4;
        const auto directAndEmission = drawAndRead(material, false);
        EXPECT_VEC3_NEAR(directAndEmission, (direct + math::Vector3{8, 0.5f, 2.25f}), 0.01f);
        const math::Vector3 reflected{0.125f, 0.5f, 1.25f};
        /// @note Alpha is validity, not additive weight; disabled and invalid pixels preserve the previous lighting.
        EXPECT_VEC3_NEAR(drawAndRead(material, false, {reflected.x, reflected.y, reflected.z, 1}),
            directAndEmission, 0.01f);
        EXPECT_VEC3_NEAR(drawAndRead(material, false, {reflected.x, reflected.y, reflected.z, 0}, true),
            directAndEmission, 0.01f);
        EXPECT_VEC3_NEAR(drawAndRead(material, false, {reflected.x, reflected.y, reflected.z, 1}, true),
            (directAndEmission + reflected), 0.01f);

        advanced.iblIntensity = 1;
        advanced.iblSpecularScale = 1;
        material.metallic = 1;
        const auto withoutEnvironmentSpecular = drawAndRead(material, false, {0, 0, 0, 1}, true);
        const auto fallback = drawAndRead(material, false, {reflected.x, reflected.y, reflected.z, 0}, true);
        /// @note The constant white cube and scale-only LUT contribute exactly 4 * F0 = 2, never on top of valid RT.
        EXPECT_VEC3_NEAR(fallback, (withoutEnvironmentSpecular + math::Vector3{2, 2, 2}), 0.02f);
        EXPECT_VEC3_NEAR(drawAndRead(material, false, {reflected.x, reflected.y, reflected.z, 1}, true),
            (withoutEnvironmentSpecular + reflected), 0.02f);
        resources.Reset();
    }
    device.Shutdown();
}

TEST_F(DeferredEmissionTest, ConsumesFinalGBufferRoughnessWithoutFilteringAcrossNormalBoundaries)
{
    constexpr uint32_t extent = 4;
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        const auto shader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
            / "Pipeline/Deferred/DeferredLighting.hlsl").generic_string());
        const auto hdr = resources.CreateRenderTarget(extent, extent,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(shader.IsValid() && hdr.IsValid() && state.IsValid());
        std::array<uint8_t, extent * extent * 4> uniformPixels{}, varyingPixels{};
        for (uint32_t y = 0; y < extent; ++y) {
            for (uint32_t x = 0; x < extent; ++x) {
                const size_t offset = (y * extent + x) * 4;
                const bool boundary = (x + y) % 2 != 0;
                uniformPixels[offset] = uniformPixels[offset + 1] = 128;
                uniformPixels[offset + 2] = uniformPixels[offset + 3] = 255;
                varyingPixels[offset] = boundary ? 255 : 128;
                varyingPixels[offset + 1] = 128;
                varyingPixels[offset + 2] = boundary ? 128 : 255;
                varyingPixels[offset + 3] = 255;
            }
        }
        const auto uniformNormals = resources.CreateTexture(uniformPixels.data(), extent, extent);
        const auto varyingNormals = resources.CreateTexture(varyingPixels.data(), extent, extent);
        const uint8_t depthPixels[] = {128, 128, 128, 255};
        const uint8_t blackPixels[] = {0, 0, 0, 255};
        const auto depth = resources.CreateTexture(depthPixels, 1, 1);
        const auto black = resources.CreateTexture(blackPixels, 1, 1);
        ASSERT_TRUE(uniformNormals.IsValid() && varyingNormals.IsValid() && depth.IsValid() && black.IsValid());
        renderer::PerFrameCB frame{};
        frame.view = frame.projection = frame.viewProjection = frame.invViewProjection = math::Matrix4::Identity();
        frame.cameraPos = {0, 0, 10}; frame.nearZ = 0.1f; frame.farZ = 20;
        renderer::LightConstantsCB light{};
        light.lightDir = {0, 0, -1}; light.lightColor = {1, 1, 1}; light.lightIntensity = 0.1f;
        renderer::ShadowConstantsCB shadow{};
        renderer::PostProcCB post{};
        renderer::AdvancedGraphicsCB advanced{};
        const auto frameCB = resources.CreateConstantBuffer(sizeof(frame));
        const auto lightCB = resources.CreateConstantBuffer(sizeof(light));
        const auto shadowCB = resources.CreateConstantBuffer(sizeof(shadow));
        const auto postCB = resources.CreateConstantBuffer(sizeof(post));
        const auto advancedCB = resources.CreateConstantBuffer(sizeof(advanced));
        ASSERT_TRUE(frameCB.IsValid() && lightCB.IsValid() && shadowCB.IsValid()
            && postCB.IsValid() && advancedCB.IsValid());
        const auto render = [&](renderer::ResourceHandle<renderer::TextureTag> normals,
            renderer::ResourceHandle<renderer::TextureTag> material) {
            resources.AdvanceFrame();
            device.BeginFrame();
            resources.Update(frameCB, &frame, sizeof(frame));
            resources.Update(lightCB, &light, sizeof(light));
            resources.Update(shadowCB, &shadow, sizeof(shadow));
            resources.Update(postCB, &post, sizeof(post));
            resources.Update(advancedCB, &advanced, sizeof(advanced));
            device.SetRenderTarget(hdr, resources);
            device.SetViewport(0, 0, extent, extent);
            device.Clear({0, 0, 0, 0});
            renderer::DrawCall draw;
            draw.shader = shader; draw.pipelineState = state; draw.vertexCount = 3;
            draw.constantBuffers[0] = frameCB; draw.constantBuffers[3] = lightCB;
            draw.constantBuffers[4] = shadowCB; draw.constantBuffers[5] = postCB;
            draw.constantBuffers[8] = advancedCB;
            draw.textures[3] = black; draw.textures[5] = material;
            draw.textures[6] = normals; draw.textures[7] = depth;
            device.Submit(draw, resources);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            m_allowHdrCubeClearMetadataWarning = false;
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(hdr, resources, pixels, width, height));
            EXPECT_EQ(width, extent); EXPECT_EQ(height, extent);
            return pixels;
        };
        /// @note Each 2x2 quad changes adjacent normals but keeps the checked receiver's normal/material/depth identical; a second ddx filter changes its actual BRDF output.
        for (uint8_t roughness : {uint8_t{64}, uint8_t{0}, uint8_t{255}}) {
            const uint8_t materialPixels[] = {128, 128, 128, roughness};
            const auto material = resources.CreateTexture(materialPixels, 1, 1);
            ASSERT_TRUE(material.IsValid());
            const auto uniform = render(uniformNormals, material);
            const auto varying = render(varyingNormals, material);
            ASSERT_EQ(uniform.size(), extent * extent * 4u);
            ASSERT_EQ(varying.size(), uniform.size());
            for (uint32_t y = 0; y < extent; ++y) {
                for (uint32_t x = 0; x < extent; ++x) {
                    const size_t offset = (y * extent + x) * 4;
                    for (size_t channel = 0; channel < 4; ++channel) {
                        EXPECT_TRUE(std::isfinite(uniform[offset + channel]));
                        EXPECT_TRUE(std::isfinite(varying[offset + channel]));
                        if ((x + y) % 2 == 0)
                            EXPECT_NEAR(varying[offset + channel], uniform[offset + channel], 0.001f)
                                << "roughness=" << static_cast<uint32_t>(roughness) << " pixel=" << x << ',' << y;
                    }
                    EXPECT_NEAR(uniform[offset + 3], 1, 0.001f);
                    EXPECT_NEAR(varying[offset + 3], 1, 0.001f);
                }
            }
            if (roughness == 64) {
                EXPECT_GT(uniform[(2 * extent + 2) * 4], 0.5f);
                EXPECT_GT(uniform[4], varying[4] + 0.1f);
            }
        }
        resources.Reset();
    }
    device.Shutdown();
}

TEST_F(DeferredEmissionTest, SelectsRayReflectionAndSsrExclusivelyBeforeToneMapping)
{
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        const auto shader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
            / "PostProcess/Color/Composite.hlsl").generic_string());
        const renderer::RenderTargetDesc desc{1, renderer::Format::RGBA16F, false};
        const auto hdr = resources.CreateRenderTarget(1, 1, desc);
        const auto ssr = resources.CreateRenderTarget(1, 1, desc);
        const auto ray = resources.CreateRenderTarget(1, 1, desc);
        const auto output = resources.CreateRenderTarget(1, 1, desc);
        const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(shader.IsValid());
        ASSERT_TRUE(hdr.IsValid() && ssr.IsValid() && ray.IsValid() && output.IsValid());
        ASSERT_TRUE(state.IsValid());
        renderer::PerFrameCB frame{};
        renderer::PostProcCB post = renderer::MakeScreenPostProcCB(1, 1);
        post.exposure = 1;
        post.saturation = 1;
        post.userBrightness = 1;
        renderer::AdvancedGraphicsCB advanced{};
        const auto frameCB = resources.CreateConstantBuffer(sizeof(frame));
        const auto postCB = resources.CreateConstantBuffer(sizeof(post));
        const auto advancedCB = resources.CreateConstantBuffer(sizeof(advanced));
        const auto render = [&](bool enabled, float validity, float ssrIntensity) {
            resources.AdvanceFrame();
            device.BeginFrame();
            device.SetRenderTarget(hdr, resources);
            device.Clear({0.125f, 0.25f, 0.5f, 1});
            device.SetRenderTarget(ssr, resources);
            device.Clear({4, 1, 2, 1});
            device.SetRenderTarget(ray, resources);
            device.Clear({0.25f, 0.5f, 1, validity});
            post.rayReflectionEnabled = enabled ? 1.0f : 0.0f;
            advanced.ssrIntensity = ssrIntensity;
            resources.Update(frameCB, &frame, sizeof(frame));
            resources.Update(postCB, &post, sizeof(post));
            resources.Update(advancedCB, &advanced, sizeof(advanced));
            device.SetRenderTarget(output, resources);
            renderer::DrawCall draw;
            draw.shader = shader;
            draw.pipelineState = state;
            draw.vertexCount = 3;
            draw.constantBuffers[0] = frameCB;
            draw.constantBuffers[5] = postCB;
            draw.constantBuffers[8] = advancedCB;
            draw.textures[5] = resources.GetColorTexture(hdr, 0);
            draw.textures[19] = resources.GetColorTexture(ssr, 0);
            if (enabled) draw.textures[20] = resources.GetColorTexture(ray, 0);
            device.Submit(draw, resources);
            device.SetRenderTarget({}, resources);
            device.EndFrame();
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            if (!device.CaptureRenderTargetToLinearRGBA(output, resources, pixels, width, height)
                    || width != 1 || height != 1 || pixels.size() != 4) {
                ADD_FAILURE() << "Reflection composite readback failed";
                return math::Vector3{};
            }
            return math::Vector3{pixels[0], pixels[1], pixels[2]};
        };
        const auto original = render(false, 0, 0);
        const auto ssrFallback = render(false, 0, 1);
        EXPECT_GT((ssrFallback - original).LengthSq(), 0.01f);
        EXPECT_VEC3_NEAR(render(true, 0, 1), ssrFallback, 0.002f);
        EXPECT_VEC3_NEAR(render(true, 1, 1), original, 0.002f);
        EXPECT_VEC3_NEAR(render(false, 1, 1), ssrFallback, 0.002f);
        resources.Reset();
    }
    device.Shutdown();
}

TEST_F(DeferredEmissionTest, ResolvesScreenFirstSpecularWithoutBlendingDirectLightOrEmission)
{
    constexpr uint32_t extent = 4;
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        const auto shader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
            / "Pipeline/Deferred/DeferredLighting.hlsl").generic_string());
        const renderer::RenderTargetDesc description{1, renderer::Format::RGBA16F, false};
        const auto hdr = resources.CreateRenderTarget(extent, extent, description);
        const auto material = resources.CreateRenderTarget(extent, extent, description);
        const auto normals = resources.CreateRenderTarget(extent, extent, description);
        const auto emission = resources.CreateRenderTarget(extent, extent, description);
        const auto ray = resources.CreateRenderTarget(extent, extent, description);
        const auto ssr = resources.CreateRenderTarget(extent, extent, description);
        const auto environment = resources.CreateCubemapRenderTarget(1);
        const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        const uint8_t depthPixels[] = {128, 128, 128, 255};
        const uint8_t brdfPixels[] = {255, 0, 0, 255};
        const auto depth = resources.CreateTexture(depthPixels, 1, 1);
        const auto brdf = resources.CreateTexture(brdfPixels, 1, 1);
        ASSERT_TRUE(shader && hdr && material && normals && emission && ray && ssr
            && environment && state && depth && brdf);
        renderer::PerFrameCB frame{};
        frame.view = frame.projection = frame.viewProjection = frame.invViewProjection = math::Matrix4::Identity();
        frame.cameraPos = {0, 0, 10}; frame.nearZ = 0.1f; frame.farZ = 20;
        renderer::LightConstantsCB light{};
        light.lightDir = {0, 0, -1}; light.lightColor = {1, 1, 1}; light.lightIntensity = 1;
        renderer::ShadowConstantsCB shadow{};
        renderer::PostProcCB post = renderer::MakeScreenPostProcCB(extent, extent);
        renderer::AdvancedGraphicsCB advanced{};
        advanced.iblIntensity = 1; advanced.iblDiffuseScale = 0.5f; advanced.iblSpecularScale = 1;
        const auto frameCB = resources.CreateConstantBuffer(sizeof(frame));
        const auto lightCB = resources.CreateConstantBuffer(sizeof(light));
        const auto shadowCB = resources.CreateConstantBuffer(sizeof(shadow));
        const auto postCB = resources.CreateConstantBuffer(sizeof(post));
        const auto advancedCB = resources.CreateConstantBuffer(sizeof(advanced));
        ASSERT_TRUE(frameCB && lightCB && shadowCB && postCB && advancedCB);
        const math::Vector3 baseEmission{0.5f, 1, 2};
        const auto render = [&](math::Vector4 screen, math::Vector4 traced, float intensity = 1,
            bool screenEnabled = true, bool tracedEnabled = true, bool resolveEnabled = true,
            math::Vector3 emissive = math::Vector3{0.5f, 1, 2}, float roughness = 0.25f, float metallic = 0,
            float glassMarker = 0, float resolveModeOverride = -1) {
            resources.AdvanceFrame(); device.BeginFrame();
            const auto clear = [&](auto target, math::Vector4 value) {
                device.SetRenderTarget(target, resources); device.Clear(value);
            };
            clear(material, {0.5f, 0.25f, 0.75f, roughness});
            clear(normals, {0.5f, 0.5f, 1, metallic});
            clear(emission, {emissive.x, emissive.y, emissive.z, glassMarker});
            clear(ray, traced); clear(ssr, screen);
            m_allowHdrCubeClearMetadataWarning = true;
            for (uint32_t face = 0; face < 6; ++face) {
                device.SetRenderTargetFace(environment, face, 0, resources); device.Clear({4, 4, 4, 1});
            }
            post.reflectionResolveEnabled = resolveModeOverride >= 0 ? resolveModeOverride : (resolveEnabled ? 1.0f : 0.0f);
            post.reflectionSsrEnabled = screenEnabled ? 1.0f : 0.0f;
            post.rayReflectionEnabled = tracedEnabled ? 1.0f : 0.0f;
            advanced.ssrIntensity = intensity;
            resources.Update(frameCB, &frame, sizeof(frame)); resources.Update(lightCB, &light, sizeof(light));
            resources.Update(shadowCB, &shadow, sizeof(shadow)); resources.Update(postCB, &post, sizeof(post));
            resources.Update(advancedCB, &advanced, sizeof(advanced));
            clear(hdr, {}); device.SetViewport(0, 0, extent, extent);
            renderer::DrawCall draw;
            draw.shader = shader; draw.pipelineState = state; draw.vertexCount = 3;
            draw.constantBuffers[0] = frameCB; draw.constantBuffers[3] = lightCB;
            draw.constantBuffers[4] = shadowCB; draw.constantBuffers[5] = postCB;
            draw.constantBuffers[8] = advancedCB;
            draw.textures[3] = resources.GetColorTexture(emission, 0);
            draw.textures[5] = resources.GetColorTexture(material, 0);
            draw.textures[6] = resources.GetColorTexture(normals, 0); draw.textures[7] = depth;
            draw.textures[16] = draw.textures[17] = resources.GetCubemapTexture(environment);
            draw.textures[18] = brdf;
            if (tracedEnabled) draw.textures[20] = resources.GetColorTexture(ray, 0);
            if (screenEnabled) draw.textures[23] = resources.GetColorTexture(ssr, 0);
            device.Submit(draw, resources); device.SetRenderTarget({}, resources); device.EndFrame();
            m_allowHdrCubeClearMetadataWarning = false;
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(hdr, resources, pixels, width, height));
            EXPECT_EQ(width, extent); EXPECT_EQ(height, extent);
            if (pixels.size() != extent * extent * 4u) { ADD_FAILURE() << "Specular resolve HDR readback failed"; return math::Vector3{}; }
            const size_t offset = (2u * extent + 2u) * 4u;
            EXPECT_NEAR(pixels[offset + 3], 1, 0.001f);
            return math::Vector3{pixels[offset], pixels[offset + 1], pixels[offset + 2]};
        };
        const math::Vector4 screen{0.25f, 0.5f, 0.75f, 1}, traced{0.125f, 0.375f, 1, 1};
        const math::Vector3 screenRgb{screen.x, screen.y, screen.z}, rayRgb{traced.x, traced.y, traced.z};
        light.lightIntensity = 0;
        const auto diffuseAndEmission = render({}, {0, 0, 0, 1});
        light.lightIntensity = 1;
        const auto base = render({}, {0, 0, 0, 1});
        const auto legacyBaseline = render({}, {}, 0, false, false, false);
        /// @note F0=0.04, a constant radiance-4 cube and a scale-only LUT give 0.16; the checked view's fifth-power Fresnel remainder is below half-storage precision.
        const math::Vector3 baselineSpecular{0.16f, 0.16f, 0.16f};
        EXPECT_GT(base.x - diffuseAndEmission.x, 0.05f);
        EXPECT_VEC3_NEAR(legacyBaseline, (base + baselineSpecular), 0.01f);
        EXPECT_VEC3_NEAR(render({}, {}), legacyBaseline, 0.01f);
        EXPECT_VEC3_NEAR(render({}, {0, 0, 0, 1}), base, 0.01f);
        EXPECT_VEC3_NEAR(render({0, 0, 0, 1}, traced), base, 0.01f);
        EXPECT_VEC3_NEAR(render(screen, traced), (base + screenRgb), 0.01f);
        EXPECT_VEC3_NEAR(render({screen.x, screen.y, screen.z, 0.25f}, traced),
            (base + screenRgb * 0.25f + rayRgb * 0.75f), 0.01f);
        EXPECT_VEC3_NEAR(render({screen.x, screen.y, screen.z, 0.25f}, {}),
            (base + screenRgb * 0.25f + baselineSpecular * 0.75f), 0.01f);
        EXPECT_VEC3_NEAR(render(screen, traced, 0.25f), render({screen.x, screen.y, screen.z, 0.25f}, traced), 0.01f);
        EXPECT_VEC3_NEAR(render(screen, traced, 1, false), (base + rayRgb), 0.01f);
        EXPECT_VEC3_NEAR(render({screen.x, screen.y, screen.z, 0.25f}, traced, 1, true, false),
            (base + screenRgb * 0.25f + baselineSpecular * 0.75f), 0.01f);
        EXPECT_VEC3_NEAR(render(screen, traced, 0), (base + rayRgb), 0.01f);
        EXPECT_VEC3_NEAR(render({screen.x, screen.y, screen.z, -1}, traced), (base + rayRgb), 0.01f);
        EXPECT_VEC3_NEAR(render({screen.x, screen.y, screen.z, 2}, traced), (base + screenRgb), 0.01f);
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        EXPECT_VEC3_NEAR(render({invalid, screen.y, screen.z, 1}, traced), (base + rayRgb), 0.01f);
        EXPECT_VEC3_NEAR(render(screen, {invalid, traced.y, traced.z, 1}, 0), legacyBaseline, 0.01f);
        EXPECT_VEC3_NEAR(render(screen, traced, invalid), (base + rayRgb), 0.01f);
        /// @note The HDR readback is before ACES; changing emission must add the full value at both complete and partial SSR confidence, never lerp the whole lighting result.
        const math::Vector3 extraEmission{0.25f, 0.5f, 1};
        for (float confidence : {0.25f, 1.0f}) {
            const math::Vector4 sampled{screen.x, screen.y, screen.z, confidence};
            EXPECT_VEC3_NEAR(render(sampled, traced, 1, true, true, true, baseEmission + extraEmission),
                (render(sampled, traced) + extraEmission), 0.01f);
        }
        const auto renderMirror = [&](math::Vector4 sampled, math::Vector4 raySample,
            bool tracedEnabled = true, float intensity = 1) {
            return render(sampled, raySample, intensity, true, tracedEnabled, true, baseEmission, 0.045f, 1);
        };
        const auto mirrorBase = renderMirror({}, {0, 0, 0, 1});
        const auto mirrorIbl = renderMirror({}, {}, false);
        /// @note A constant radiance-4 cube and a scale-only LUT give albedo*4 for a fully metallic receiver; direct light and emission stay in mirrorBase.
        const math::Vector3 mirrorSpecular{2, 1, 3};
        EXPECT_VEC3_NEAR(mirrorIbl, (mirrorBase + mirrorSpecular), 0.01f);
        EXPECT_VEC3_NEAR(renderMirror(screen, traced), (mirrorBase + rayRgb), 0.01f);
        EXPECT_VEC3_NEAR(renderMirror(screen, {0, 0, 0, 1}), mirrorBase, 0.01f);
        EXPECT_VEC3_NEAR(renderMirror(screen, {}), mirrorIbl, 0.01f);
        EXPECT_VEC3_NEAR(renderMirror(screen, traced, false), (mirrorBase + screenRgb), 0.01f);
        EXPECT_VEC3_NEAR(renderMirror({screen.x, screen.y, screen.z, 0.25f}, traced),
            (mirrorBase + rayRgb), 0.01f);
        EXPECT_VEC3_NEAR(renderMirror(screen, traced, true, 0.25f), (mirrorBase + rayRgb), 0.01f);
        EXPECT_VEC3_NEAR(renderMirror({screen.x, screen.y, screen.z, 0.25f}, {}, false),
            (mirrorBase + screenRgb * 0.25f + mirrorSpecular * 0.75f), 0.01f);
        EXPECT_VEC3_NEAR(render(screen, traced, 1, true, true, false, baseEmission, 0.045f, 1),
            (mirrorBase + rayRgb), 0.01f);
        /// @note These values are the adjacent representable RGBA16F values around the policy thresholds, so clear quantization cannot move a case across the intended boundary.
        struct PolicyBoundary { float roughness, metallic; bool preferRay; };
        for (const auto boundary : {PolicyBoundary{0.2498779296875f, 0.900390625f, true},
                 PolicyBoundary{0.25f, 1, true},
                 PolicyBoundary{0.250244140625f, 1, false},
                 PolicyBoundary{0.1f, 1, true},
                 PolicyBoundary{0.2f, 1, true},
                 PolicyBoundary{0.045f, 0.89990234375f, false},
                 PolicyBoundary{0.1f, 0.6f, false},
                 PolicyBoundary{0.045f, 0, false}}) {
            SCOPED_TRACE(boundary.roughness);
            SCOPED_TRACE(boundary.metallic);
            const auto materialBase = render({}, {0, 0, 0, 1}, 1, true, true, true,
                baseEmission, boundary.roughness, boundary.metallic);
            EXPECT_VEC3_NEAR(render(screen, traced, 1, true, true, true, baseEmission,
                boundary.roughness, boundary.metallic),
                (materialBase + (boundary.preferRay ? rayRgb : screenRgb)), 0.01f);
        }
        const auto glassRender = [&](math::Vector4 sampled, float marker = 1,
            bool enabled = true, math::Vector3 glow = math::Vector3{0.5f, 1, 2}, float mode = 1) {
            return render(screen, sampled, 1, true, enabled, true, glow, 0.015f, 0, marker, mode);
        };
        /// @note Full dielectric radiance replaces all raster lighting; SSR confidence and emitter/IBL inputs must not add energy twice.
        const math::Vector4 glassSample{0.125f, 0.375f, 1, 2};
        EXPECT_VEC3_NEAR(glassRender(glassSample), rayRgb, 0.001f);
        light.lightIntensity = 50;
        advanced.iblIntensity = 20;
        EXPECT_VEC3_NEAR(glassRender(glassSample, 1, true, {32, 16, 8}), rayRgb, 0.001f);
        EXPECT_VEC3_NEAR(glassRender({0, 0, 0, 2}), math::Vector3{}, 0.001f);
        const auto glassFallback = glassRender({}, 1, false);
        EXPECT_VEC3_NEAR(glassRender(glassSample, 1, false), glassFallback, 0.01f);
        EXPECT_VEC3_NEAR(glassRender({invalid, 0, 0, 2}), glassFallback, 0.01f);
        /// @note A proven initial medium also publishes full radiance for its opaque terminal; marker0 is not an ordinary kind1 specular response.
        EXPECT_VEC3_NEAR(glassRender(glassSample, 0), rayRgb, 0.001f);
        EXPECT_VEC3_NEAR(glassRender(glassSample, 0, true, {32, 16, 8}), rayRgb, 0.001f);
        EXPECT_VEC3_NEAR(glassRender({0, 0, 0, 2}, 0), math::Vector3{}, 0.001f);
        const auto insideOpaqueFallback = glassRender({}, 0, false);
        /// @note The receipt flag, not a stale full-radiance texture, authorizes replacement of the current raster receiver.
        EXPECT_VEC3_NEAR(glassRender(glassSample, 0, false), insideOpaqueFallback, 0.01f);
        EXPECT_VEC3_NEAR(glassRender({invalid, 0, 0, 2}, 0), glassRender({}, 0), 0.01f);
        EXPECT_VEC3_NEAR(glassRender({-1, 0, 0, 2}, 0), glassRender({}, 0), 0.01f);
        /// @note Unsupported dielectric marker2 cannot accept full radiance even with a current receipt.
        EXPECT_VEC3_NEAR(glassRender(glassSample, 2), glassRender({}, 2), 0.01f);
        EXPECT_VEC3_NEAR(glassRender(glassSample, 1, true, {8, 4, 2}, 2), math::Vector3{}, 0.001f);
        EXPECT_VEC3_NEAR(glassRender({}, 2, false, {8, 4, 2}, 2), math::Vector3{}, 0.001f);
        resources.Reset();
    }
    device.Shutdown();
}

TEST_F(DeferredEmissionTest, RejectsStaleSsrReceiptBeforePublishingAnOldResult)
{
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        renderer::RenderPassHandles handles;
        handles.ssrShader = resources.LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
            / "PostProcess/Reflections/SSR.cs.hlsl").generic_string());
        handles.ssrResult = resources.CreateComputeTexture(1, 1);
        handles.frameCB = resources.CreateConstantBuffer(sizeof(renderer::PerFrameCB));
        handles.advancedGraphicsCB = resources.CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
        handles.postprocCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
        const auto gbuffer = resources.CreateRenderTarget(1, 1, renderer::CameraDepthTargetDesc(2));
        const auto hdr = resources.CreateRenderTarget(1, 1, renderer::CameraDepthTargetDesc(1));
        ASSERT_TRUE(handles.ssrShader && handles.ssrResult && handles.frameCB
            && handles.advancedGraphicsCB && handles.postprocCB && gbuffer && hdr);
        renderer::Camera camera;
        renderer::RenderSettings settings;
        settings.ssr.enabled = true;
        renderer::RenderPassContext context{{}, device, resources, camera, settings, {}, ~0u, handles};
        context.width = context.height = 1;
        context.hybridReflectionResolveActive = context.hybridReflectionSsrPlanned = true;
        context.resourceRegistry.BindTarget("GBuffer", gbuffer);
        context.resourceRegistry.BindTarget("HDR", hdr);
        context.resourceRegistry.BindTexture("SSRResult", handles.ssrResult);
        renderer::SSRPass pass;
        renderer::PassBuilder builder;
        pass.Setup(builder, context);
        renderer::PassResources passResources(context.resourceRegistry, builder.Accesses(), pass.Name());
        context.passResources = &passResources;
        /// @note All inputs are live, but a closed physical frame cannot produce a dispatch receipt.
        context.ssrPassActive = true;
        renderer::ExecuteSSRPass(context);
        EXPECT_FALSE(context.ssrPassActive);
        context.resourceRegistry.BindTarget("HDR", {});
        context.ssrPassActive = true;
        renderer::ExecuteSSRPass(context);
        EXPECT_FALSE(context.ssrPassActive);
        context.resourceRegistry.BindTarget("HDR", hdr);
        resources.Release(handles.frameCB);
        context.ssrPassActive = true;
        renderer::ExecuteSSRPass(context);
        EXPECT_FALSE(context.ssrPassActive);
        context.passResources = nullptr;
        resources.Reset();
    }
    device.Shutdown();
}

TEST_F(DeferredEmissionTest, ProducesWeightedHybridSsrWithMaterialIndependentConfidenceAndEdgeFade)
{
    constexpr uint32_t extent = 32;
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
    device.SetRenderWhenOccluded(true);
    {
        renderer::ResourceManager resources(device);
        const auto shaderRoot = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        const auto shader = resources.LoadShader((shaderRoot / "PostProcess/Reflections/SSR.cs.hlsl").generic_string());
        const auto copy = resources.LoadShader((shaderRoot / "PostProcess/Color/CopyColor.hlsl").generic_string());
        const renderer::RenderTargetDesc description{1, renderer::Format::RGBA16F, false};
        const auto hdr = resources.CreateRenderTarget(extent, extent, description);
        const auto material = resources.CreateRenderTarget(extent, extent, description);
        const auto normals = resources.CreateRenderTarget(extent, extent, description);
        const auto target = resources.CreateRenderTarget(extent, extent, description);
        const auto output = resources.CreateComputeTexture(extent, extent);
        const auto glassMarkers = resources.CreateRenderTarget(extent, extent, description);
        const auto glassMarkerSource = resources.CreateRenderTarget(1, 1, description);
        const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(shader && copy && hdr && material && normals && target && output && glassMarkers && glassMarkerSource && state);
        renderer::Camera camera;
        camera.m_projection = renderer::ProjectionMode::Orthographic;
        camera.m_position = {};
        camera.m_orthoHeight = 4; camera.m_aspect = 1; camera.m_near = 0.1f; camera.m_far = 10;
        const auto frame = renderer::MakeCameraFrameCB(camera, 0, 0);
        renderer::AdvancedGraphicsCB advanced{};
        advanced.ssrMaxDistance = 3; advanced.ssrThickness = 0.15f; advanced.ssrSteps = 64; advanced.ssrIntensity = 1;
        const auto frameCB = resources.CreateConstantBuffer(sizeof(frame));
        const auto advancedCB = resources.CreateConstantBuffer(sizeof(advanced));
        const auto postCB = resources.CreateConstantBuffer(sizeof(renderer::PostProcCB));
        ASSERT_TRUE(frameCB && advancedCB && postCB);
        const auto depthByte = [&](float z) {
            return static_cast<uint8_t>(std::lround((camera.m_far - z) / (camera.m_far - camera.m_near) * 255.0f));
        };
        enum class TargetCase : uint8_t { FRONT_PLANE, BACK_FACE, BACKGROUND, SELF, THICKNESS_GAP };
        const math::Vector3 incident{5, 10, 15};
        const auto render = [&](float metallic, bool unified, uint32_t targetBoundary = 16,
            uint32_t receiverX = 8, math::Vector3 source = math::Vector3{5, 10, 15},
            TargetCase targetCase = TargetCase::FRONT_PLANE, float roughness = 0.045f,
            float thickness = 0.15f, float intensity = 1, float maxDistance = 3,
            float receiverGlassMarker = 0, float targetGlassMarker = 0) {
            std::array<uint8_t, extent * extent * 4> sceneDepthPixels{};
            for (uint32_t y = 0; y < extent; ++y) {
                for (uint32_t x = 0; x < extent; ++x) {
                    const size_t offset = (y * extent + x) * 4;
                    /// @note The distinct camera-facing target is z=3.1898-(4/3)*(worldX-worldXAtBoundary). Its normal (-.8,0,-.6) faces the reflected ray; the ray physically reaches it within distance3.
                    const float targetZ = 3.1898f - static_cast<float>(static_cast<int>(x) - static_cast<int>(targetBoundary)) / 6.0f;
                    const uint8_t targetDepth = targetCase == TargetCase::BACKGROUND ? 0
                        : depthByte(targetCase == TargetCase::THICKNESS_GAP ? 2.4f : targetZ);
                    sceneDepthPixels[offset] = sceneDepthPixels[offset + 1] = sceneDepthPixels[offset + 2]
                        = x < targetBoundary || targetCase == TargetCase::SELF ? depthByte(3) : targetDepth;
                    sceneDepthPixels[offset + 3] = 255;
                }
            }
            const auto sceneDepth = resources.CreateTexture(sceneDepthPixels.data(), extent, extent);
            EXPECT_TRUE(sceneDepth);
            const uint8_t targetNormalPixels[] = {
                static_cast<uint8_t>(targetCase == TargetCase::BACK_FACE ? 230 : 26), 128,
                static_cast<uint8_t>(targetCase == TargetCase::BACK_FACE ? 204 : 51), 255};
            const auto targetNormal = resources.CreateTexture(targetNormalPixels, 1, 1);
            EXPECT_TRUE(targetNormal);
            renderer::PostProcCB post{};
            post.reflectionResolveEnabled = post.reflectionSsrEnabled = unified ? 1.0f : 0.0f;
            advanced.ssrSteps = targetCase == TargetCase::THICKNESS_GAP ? 1 : 64;
            advanced.ssrThickness = thickness; advanced.ssrIntensity = intensity;
            advanced.ssrMaxDistance = maxDistance;
            resources.AdvanceFrame(); device.BeginFrame();
            device.SetViewport(0, 0, extent, extent);
            device.SetRenderTarget(hdr, resources); device.Clear({source.x, source.y, source.z, 1});
            device.SetRenderTarget(glassMarkerSource, resources); device.Clear({0, 0, 0, targetGlassMarker});
            device.SetRenderTarget(glassMarkers, resources); device.Clear({0, 0, 0, receiverGlassMarker});
            device.SetViewport(targetBoundary, 0, extent - targetBoundary, extent);
            renderer::DrawCall fillMarker;
            fillMarker.shader = copy; fillMarker.pipelineState = state; fillMarker.vertexCount = 3;
            fillMarker.textures[5] = resources.GetColorTexture(glassMarkerSource, 0);
            device.Submit(fillMarker, resources);
            device.SetViewport(0, 0, extent, extent);
            device.SetRenderTarget(material, resources); device.Clear({0.2f, 0.4f, 0.8f, roughness});
            /// @note The true target uses N.z=-3/4 at the receiver. SELF instead sends the reflection behind its own constant-depth surface, which legacy SSR accepts on the first step.
            device.SetRenderTarget(normals, resources);
            device.Clear(targetCase == TargetCase::SELF ? math::Vector4{0.9f, 0.5f, 0.2f, metallic}
                : math::Vector4{(std::sqrt(7.0f) / 4.0f + 1) * 0.5f, 0.5f, 0.125f, metallic});
            if (targetCase != TargetCase::SELF) {
                device.SetViewport(targetBoundary, 0, extent - targetBoundary, extent);
                renderer::DrawCall fillTarget;
                fillTarget.shader = copy; fillTarget.pipelineState = state; fillTarget.vertexCount = 3;
                fillTarget.textures[5] = targetNormal;
                device.Submit(fillTarget, resources);
                device.SetViewport(0, 0, extent, extent);
            }
            device.SetRenderTarget({}, resources);
            resources.Update(frameCB, &frame, sizeof(frame));
            resources.Update(advancedCB, &advanced, sizeof(advanced));
            resources.Update(postCB, &post, sizeof(post));
            renderer::ComputeCall dispatch;
            dispatch.shader = shader; dispatch.constantBuffers[0] = frameCB;
            dispatch.constantBuffers[5] = postCB; dispatch.constantBuffers[8] = advancedCB;
            dispatch.srvInputs[0] = resources.GetColorTexture(material, 0);
            dispatch.srvInputs[5] = resources.GetColorTexture(hdr, 0);
            dispatch.srvInputs[6] = resources.GetColorTexture(normals, 0);
            dispatch.srvInputs[7] = dispatch.srvInputs[25] = sceneDepth;
            dispatch.srvInputs[22] = resources.GetColorTexture(glassMarkers, 0);
            dispatch.uavOutputs[3] = output; dispatch.dispatchX = dispatch.dispatchY = extent / 8;
            EXPECT_TRUE(device.TryDispatch(dispatch, resources));
            device.SetRenderTarget(target, resources);
            renderer::DrawCall draw;
            draw.shader = copy; draw.pipelineState = state; draw.vertexCount = 3; draw.textures[5] = output;
            device.Submit(draw, resources); device.SetRenderTarget({}, resources); device.EndFrame();
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(target, resources, pixels, width, height));
            EXPECT_EQ(width, extent); EXPECT_EQ(height, extent);
            if (pixels.size() != extent * extent * 4u) { ADD_FAILURE() << "SSR adapter readback failed"; return math::Vector4{}; }
            const size_t offset = (16u * extent + receiverX) * 4u;
            return math::Vector4{pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
        };
        /// @note This CPU Schlick reference is independent of the shader helper; half-float material/normal storage is the only allowed output tolerance.
        const float fifthPower = 1.0f / 1024.0f;
        const float dielectricFresnel = 0.04f + 0.96f * fifthPower;
        const auto dielectric = render(0, true);
        EXPECT_NEAR(dielectric.x, incident.x * dielectricFresnel, 0.003f);
        EXPECT_NEAR(dielectric.y, incident.y * dielectricFresnel, 0.005f);
        EXPECT_NEAR(dielectric.z, incident.z * dielectricFresnel, 0.008f);
        /// @note Point-sampled UNORM depth quantization retains a small nonzero separation; confidence is near one but is not fabricated as a perfect intersection.
        EXPECT_GT(dielectric.w, 0.98f); EXPECT_LE(dielectric.w, 1);
        const auto metallic = render(1, true);
        EXPECT_NEAR(metallic.x, incident.x * (0.2f + 0.8f * fifthPower), 0.005f);
        EXPECT_NEAR(metallic.y, incident.y * (0.4f + 0.6f * fifthPower), 0.01f);
        EXPECT_NEAR(metallic.z, incident.z * (0.8f + 0.2f * fifthPower), 0.02f);
        EXPECT_NEAR(metallic.w, dielectric.w, 0.001f);
        const auto edge = render(0, true, 29, 24);
        EXPECT_GT(edge.w, 0); EXPECT_LT(edge.w, 0.5f);
        EXPECT_NEAR(edge.x, dielectric.x, 0.003f);
        EXPECT_NEAR(edge.y, dielectric.y, 0.005f);
        EXPECT_NEAR(edge.z, dielectric.z, 0.008f);
        const auto black = render(0, true, 16, 8, {});
        EXPECT_FLOAT_EQ(black.x, 0); EXPECT_FLOAT_EQ(black.y, 0); EXPECT_FLOAT_EQ(black.z, 0);
        EXPECT_NEAR(black.w, dielectric.w, 0.001f);
        const auto legacy = render(0, false);
        EXPECT_NEAR(legacy.x, incident.x, 0.01f);
        EXPECT_NEAR(legacy.y, incident.y, 0.01f);
        EXPECT_NEAR(legacy.z, incident.z, 0.02f);
        EXPECT_NEAR(legacy.w, dielectricFresnel, 0.002f);
        for (float marker : {1.0f, 2.0f}) {
            SCOPED_TRACE(marker);
            const auto receiverGlass = render(0, true, 16, 8, incident, TargetCase::FRONT_PLANE,
                0.045f, 0.15f, 1, 3, marker, 0);
            const auto targetGlass = render(0, true, 16, 8, incident, TargetCase::FRONT_PLANE,
                0.045f, 0.15f, 1, 3, 0, marker);
            for (const auto rejected : {receiverGlass, targetGlass}) {
                EXPECT_FLOAT_EQ(rejected.x, 0); EXPECT_FLOAT_EQ(rejected.y, 0);
                EXPECT_FLOAT_EQ(rejected.z, 0); EXPECT_FLOAT_EQ(rejected.w, 0);
            }
        }
        for (const TargetCase falseHit : {TargetCase::SELF, TargetCase::BACK_FACE,
                TargetCase::BACKGROUND, TargetCase::THICKNESS_GAP}) {
            SCOPED_TRACE(static_cast<uint32_t>(falseHit));
            const auto rejected = render(0, true, 16, 8, incident, falseHit);
            EXPECT_FLOAT_EQ(rejected.x, 0); EXPECT_FLOAT_EQ(rejected.y, 0);
            EXPECT_FLOAT_EQ(rejected.z, 0); EXPECT_FLOAT_EQ(rejected.w, 0);
            if (falseHit != TargetCase::BACKGROUND) {
                /// @note These three inputs reproduce actual old Hybrid false positives; legacy Raster retains its existing acceptance instead of silently changing.
                const auto oldAccepted = render(0, false, 16, 8, incident, falseHit);
                EXPECT_GT(oldAccepted.w, 0.01f);
                EXPECT_GT(oldAccepted.x, 1);
            }
        }
        for (float roughness : {0.25f, 0.5f}) {
            SCOPED_TRACE(roughness);
            const auto rejected = render(0, true, 16, 8, incident, TargetCase::FRONT_PLANE, roughness);
            EXPECT_FLOAT_EQ(rejected.x, 0); EXPECT_FLOAT_EQ(rejected.y, 0);
            EXPECT_FLOAT_EQ(rejected.z, 0); EXPECT_FLOAT_EQ(rejected.w, 0);
            const auto raster = render(0, false, 16, 8, incident, TargetCase::FRONT_PLANE, roughness);
            EXPECT_GT(raster.w, 0);
        }
        const float invalid = std::numeric_limits<float>::quiet_NaN();
        const float infinite = std::numeric_limits<float>::infinity();
        for (float thickness : {0.0f, -0.1f, invalid, infinite}) {
            SCOPED_TRACE(thickness);
            const auto rejected = render(0, true, 16, 8, incident, TargetCase::FRONT_PLANE, 0.045f, thickness);
            EXPECT_FLOAT_EQ(rejected.x, 0); EXPECT_FLOAT_EQ(rejected.y, 0);
            EXPECT_FLOAT_EQ(rejected.z, 0); EXPECT_FLOAT_EQ(rejected.w, 0);
        }
        for (float setting : {invalid, infinite}) {
            SCOPED_TRACE(setting);
            const auto invalidIntensity = render(0, true, 16, 8, incident, TargetCase::FRONT_PLANE, 0.045f, 0.15f, setting);
            const auto invalidDistance = render(0, true, 16, 8, incident, TargetCase::FRONT_PLANE, 0.045f, 0.15f, 1, setting);
            for (const auto value : {invalidIntensity, invalidDistance}) {
                EXPECT_FLOAT_EQ(value.x, 0); EXPECT_FLOAT_EQ(value.y, 0);
                EXPECT_FLOAT_EQ(value.z, 0); EXPECT_FLOAT_EQ(value.w, 0);
            }
        }
        resources.Reset();
    }
    device.Shutdown();
}

} /// @note namespace
} /// @note namespace fbzz::tests
