/// @file    DeferredEmissionTests.cpp
/// @brief   標準 GBuffer と DeferredLighting の発光を実 GPU の HDR 読み戻しで検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

class DeferredEmissionTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Deferred emission test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
    }

    void TearDown() override
    {
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
    }

    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
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

TEST_F(DeferredEmissionTest, PreservesHdrTextureAndOcclusionContracts)
{
    auto bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
    ASSERT_NE(bundle.renderer, nullptr);
    auto& device = *bundle.renderer;
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
        const auto geometryState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
        const auto lightingState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(gbuffer.IsValid());
        ASSERT_TRUE(hdr.IsValid());
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
        ASSERT_TRUE(emissionTexture.IsValid());
        ASSERT_TRUE(blackTexture.IsValid());

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

        const auto drawAndRead = [&](GBufferMaterialCB material, bool occlude) {
            resources.AdvanceFrame();
            device.BeginFrame();
            resources.Update(frameCB, &frame, sizeof(frame));
            resources.Update(lightCB, &light, sizeof(light));
            resources.Update(shadowCB, &shadow, sizeof(shadow));
            resources.Update(postCB, &post, sizeof(post));
            resources.Update(advancedCB, &advanced, sizeof(advanced));
            device.SetRenderTarget(gbuffer, resources);
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
        resources.Reset();
    }
    device.Shutdown();
}

} /// @note namespace
} /// @note namespace fbzz::tests
