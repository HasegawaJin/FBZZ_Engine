/// @file    GraphicsQualityFixture.cpp
/// @brief   GPU 検証の OS 初期化とシェーダー資源の準備。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include "GraphicsQualityFixture.hpp"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
namespace fbzz::tests {
struct GraphicsQualityPlatformState {
    HWND window = nullptr;
    HRESULT com = E_FAIL;
    ~GraphicsQualityPlatformState() {
        if (window) DestroyWindow(window);
        if (SUCCEEDED(com)) CoUninitialize();
    }
};
GraphicsQualityShaderTest::GraphicsQualityShaderTest() = default;
GraphicsQualityShaderTest::~GraphicsQualityShaderTest() = default;
GraphicsQualityShaderTest::TaaSilhouetteReadback GraphicsQualityShaderTest::TaaStaticSilhouette(
    bool velocity, bool skyBackground, bool hdr, bool staticProof, bool thinGeometry)
{
    constexpr std::array<math::Vector2, 8> JITTER_PIXELS{{
        {0.0f, -1.0f / 6.0f}, {-0.25f, 1.0f / 6.0f},
        {0.25f, -7.0f / 18.0f}, {-0.375f, -1.0f / 18.0f},
        {0.125f, 5.0f / 18.0f}, {-0.125f, -5.0f / 18.0f},
        {0.375f, 1.0f / 18.0f}, {-0.4375f, 7.0f / 18.0f}
    }};
    constexpr std::array<std::array<uint32_t, 2>, 11> EDGE_PIXELS{{
        {2,1}, {3,1}, {2,2}, {3,2}, {3,3}, {4,3}, {4,4}, {4,5}, {5,5}, {5,6}, {6,6}
    }};
    constexpr std::array<uint32_t, 8> RAW_SLOTS{0, 1, 2, 3, 4, 10, 14, 15};
    struct SilhouetteTextures {
        r::ResourceHandle<r::TextureTag> color, depth, motion;
    };
    std::array<SilhouetteTextures, 10> textures{};
    uint32_t emptyFootprintPhases = 0;
    for (size_t textureIndex = 0; textureIndex < textures.size(); ++textureIndex) {
        const auto phase = textureIndex < 8u ? textureIndex : 0u;
        const float displacement = textureIndex == 8u ? 4.0f : (textureIndex == 9u ? 0.25f : 0.0f);
        std::array<uint8_t, 8u * 8u * 4u> color{}, depth{}, motion{};
        bool footprintHasForeground = false;
        for (uint32_t y = 0; y < 8u; ++y) for (uint32_t x = 0; x < 8u; ++x) {
            const auto offset = (y * 8u + x) * 4u;
            const float sampleX = static_cast<float>(x) + 0.5f - JITTER_PIXELS[phase].x;
            const float sampleY = static_cast<float>(y) + 0.5f - JITTER_PIXELS[phase].y;
            const bool foreground = thinGeometry
                ? std::abs(sampleX - 4.5f - displacement) < 0.15f
                : sampleX > sampleY * 0.6f + 1.8f + displacement;
            if (foreground && x >= 3u && x <= 5u && y >= 3u && y <= 5u) footprintHasForeground = true;
            const uint8_t shade = foreground ? 255u : 0u;
            const uint8_t ndc = foreground ? 230u : (skyBackground ? 0u : 128u);
            color[offset] = color[offset + 1u] = color[offset + 2u] = shade;
            color[offset + 3u] = 255u;
            depth[offset] = depth[offset + 1u] = depth[offset + 2u] = ndc;
            depth[offset + 3u] = 255u;
            motion[offset] = foreground ? static_cast<uint8_t>(std::round(displacement * 255.0f / 8.0f)) : 0u;
            motion[offset + 2u] = 255u;
            motion[offset + 3u] = ndc;
        }
        if (textureIndex < 8u && !footprintHasForeground) ++emptyFootprintPhases;
        textures[textureIndex].color = m_resources->CreateTexture(color.data(), 8, 8);
        textures[textureIndex].depth = m_resources->CreateTexture(depth.data(), 8, 8);
        textures[textureIndex].motion = m_resources->CreateTexture(motion.data(), 8, 8);
        if (!textures[textureIndex].color || !textures[textureIndex].depth || !textures[textureIndex].motion) {
            ADD_FAILURE() << "Silhouette texture allocation failed";
            return {};
        }
    }
    const auto stableFrame = r::MakeCameraFrameCB(m_camera, 0, 0);
    m_data.prevViewProjection = stableFrame.viewProjection;
    m_data.invPrevViewProjection = stableFrame.invViewProjection;
    std::array<r::ResourceHandle<r::RenderTargetTag>, 8> rawPhases{};
    for (auto& raw : rawPhases) {
        raw = m_resources->CreateRenderTarget(8, 8, {1, r::Format::RGBA16F, false});
        if (!raw) { ADD_FAILURE() << "Raw phase allocation failed"; return {}; }
    }
    const auto clusterCB = m_resources->CreateConstantBuffer(sizeof(r::ClusterConstantsCB));
    if (!clusterCB) { ADD_FAILURE() << "TAA cluster constants allocation failed"; return {}; }
    bool hdrMode = hdr;
    TaaSilhouetteReadback result;
    result.emptyFootprintPhases = emptyFootprintPhases;
    enum class Input { STATIC, REVEAL, WHITE, MOTION, REACTIVE };
    r::ResourceHandle<r::RenderTargetTag> lastOutput, lastRaw;
    const auto renderFrame = [&](uint32_t frameIndex, Input input, bool fresh) {
        const auto phase = frameIndex % 8u;
        const auto textureIndex = input == Input::REVEAL ? 8u : (input == Input::MOTION ? 9u : phase);
        m_data.taaFeedback = fresh ? 0.0f : 0.9f;
        m_data.taaJitterX = JITTER_PIXELS[phase].x * 2.0f / 8.0f;
        m_data.taaJitterY = -JITTER_PIXELS[phase].y * 2.0f / 8.0f;
        const auto write = frameIndex % 2u == 0u ? m_output : m_history;
        const auto read = frameIndex % 2u == 0u ? m_history : m_output;
        Begin();
        const auto cameraFrame = r::MakeCameraFrameCB(m_camera, m_data.taaJitterX, m_data.taaJitterY);
        m_resources->Update(m_frame, &cameraFrame, sizeof(cameraFrame));
        r::TaaCycleCB cycle{};
        cycle.phaseIndex = phase; cycle.enabled = 1u;
        cycle.reserved[0] = staticProof && (input == Input::STATIC || input == Input::WHITE) ? 1u : 0u;
        m_resources->Update(m_taaCycle, &cycle, sizeof(cycle));
        r::ClusterConstantsCB cluster{};
        if (hdrMode) cluster.clusterLightMode = static_cast<uint32_t>(r::ClusterLightMode::SAMPLED);
        m_resources->Update(clusterCB, &cluster, sizeof(cluster));
        auto post = r::MakeScreenPostProcCB(8, 8);
        post.exposure = post.saturation = post.userBrightness = 1.0f;
        m_resources->Update(m_post, &post, sizeof(post));
        const std::array<r::RenderTargetColorAttachment, 3> attachments{{
            {write, 0u}, {write, 1u}, {rawPhases[phase], 0u}
        }};
        if (!m_bundle.renderer->SetRenderTargetColors(attachments, *m_resources)) {
            End(); ADD_FAILURE() << "TAA cycle MRT binding failed"; return false;
        }
        auto draw = Draw(m_taa);
        draw.constantBuffers[9] = clusterCB;
        for (uint32_t rawPhase = 0u; rawPhase < 8u; ++rawPhase)
            if (rawPhase != phase)
                draw.textures[RAW_SLOTS[rawPhase]] = m_resources->GetColorTexture(rawPhases[rawPhase], 0);
        draw.textures[5] = input == Input::WHITE ? m_white : textures[textureIndex].color;
        if (hdrMode) draw.textures[8] = draw.textures[5];
        draw.textures[7] = textures[textureIndex].depth;
        draw.textures[21] = m_resources->GetColorTexture(read, 0);
        draw.textures[6] = m_resources->GetColorTexture(read, 1);
        if (velocity || input == Input::MOTION) draw.textures[26] = textures[textureIndex].motion;
        if (input == Input::REACTIVE) draw.textures[9] = m_white;
        m_bundle.renderer->Submit(draw, *m_resources);
        End();
        lastOutput = write; lastRaw = rawPhases[phase];
        return true;
    };
    const auto capture = [&](r::ResourceHandle<r::RenderTargetTag> target, std::vector<float>& pixels) {
        uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToLinearRGBA(target, *m_resources, pixels, width, height)
            || width != 8u || height != 8u) {
            ADD_FAILURE() << "Silhouette readback failed"; return false;
        }
        return true;
    };
    std::vector<float> pixels;
    for (uint32_t frameIndex = 0u; frameIndex < 24u; ++frameIndex) {
        if (!renderFrame(frameIndex, Input::STATIC, frameIndex == 0u)) return {};
        if (frameIndex >= 8u) {
            if (!capture(lastOutput, pixels)) return {};
            const auto sample = frameIndex - 8u;
            result.phases[sample] = pixels[(4u * 8u + 4u) * 4u];
            for (size_t point = 0; point < EDGE_PIXELS.size(); ++point) {
                const auto index = (EDGE_PIXELS[point][1] * 8u + EDGE_PIXELS[point][0]) * 4u;
                result.edgePhases[point][sample] = pixels[index];
            }
        }
    }
    if (thinGeometry) return result;
    if (!renderFrame(24u, Input::REVEAL, false) || !capture(lastOutput, pixels)) return {};
    result.exposedBackground = pixels[(4u * 8u + 4u) * 4u];
    if (!capture(lastRaw, pixels)) return {};
    result.exposedHistoryLength = pixels[(4u * 8u + 4u) * 4u + 3u];

    for (uint32_t frameIndex = 0u; frameIndex < 32u; ++frameIndex) {
        const auto input = frameIndex < 24u ? Input::STATIC : Input::WHITE;
        if (!renderFrame(frameIndex, input, frameIndex == 0u)) return {};
        if (frameIndex == 0u) {
            if (!capture(lastRaw, pixels)) return {};
            result.freshHistoryLength = pixels[(4u * 8u + 4u) * 4u + 3u];
        }
        if (frameIndex >= 24u) {
            if (!capture(lastOutput, pixels)) return {};
            result.changedColor[frameIndex - 24u] = pixels[(4u * 8u + 4u) * 4u];
        }
    }

    for (uint32_t frameIndex = 0u; frameIndex < 24u; ++frameIndex)
        if (!renderFrame(frameIndex, Input::STATIC, frameIndex == 0u)) return {};
    if (!renderFrame(24u, Input::MOTION, false) || !capture(lastRaw, pixels)) return {};
    result.movingHistoryLength = pixels[(4u * 8u + 4u) * 4u + 3u];
    if (!renderFrame(25u, Input::REACTIVE, false) || !capture(lastRaw, pixels)) return {};
    result.reactiveHistoryLength = pixels[(4u * 8u + 4u) * 4u + 3u];

    for (uint32_t frameIndex = 0u; frameIndex < 24u; ++frameIndex)
        if (!renderFrame(frameIndex, Input::STATIC, frameIndex == 0u)) return {};
    hdrMode = !hdr;
    for (uint32_t frameIndex = 24u; frameIndex < 32u; ++frameIndex) {
        if (!renderFrame(frameIndex, Input::STATIC, false) || !capture(lastOutput, pixels)) return {};
        result.switchedColor[frameIndex - 24u] = pixels[(4u * 8u + 4u) * 4u];
        if (!capture(lastRaw, pixels)) return {};
        result.switchedHistoryLength[frameIndex - 24u] = pixels[(4u * 8u + 4u) * 4u + 3u];
    }
    return result;
}
void GraphicsQualityShaderTest::SetUp()
    {
        testkit::Fixture::SetUp();
        core::Logger::AddSink(this);
        m_platform = std::make_unique<GraphicsQualityPlatformState>();
        m_platform->com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_platform->com));
        m_platform->window = CreateWindowExW(0, L"STATIC", L"Graphics quality tests", WS_POPUP, 0, 0, 8, 8,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_platform->window, nullptr);
        m_bundle = r::CreateRenderer(r::RendererBackend::DX12, m_platform->window, 8, 8);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        r::SetShaderPathResolver(ResolveQualityShader);
        m_resources = std::make_unique<r::ResourceManager>(*m_bundle.renderer);
        const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
        ASSERT_TRUE(m_temp.IsValid());
        const auto testRoot = m_temp.File("Shaders");
        std::error_code error;
        std::filesystem::create_directories(testRoot, error);
        ASSERT_FALSE(error);
        /// @note テスト用の Shaders root に本番ヘッダーをそのまま配置し、DXC の include 解決を製品と揃える。
        for (const auto* directory : {"Common", "Rendering", "Platform"}) {
            std::filesystem::copy(root / directory, testRoot / directory, std::filesystem::copy_options::recursive, error);
            ASSERT_FALSE(error);
        }
        for (const auto* file : {"GraphicsQualityProbe.hlsl", "GraphicsQualityFill.cs.hlsl"}) {
            std::filesystem::copy_file((root / "../../Projects/Tests/Graphics/Shaders" / file).lexically_normal(), testRoot / file, error);
            ASSERT_FALSE(error);
        }
        m_probe = m_resources->LoadShader((testRoot / "GraphicsQualityProbe.hlsl").generic_string());
        m_fill = m_resources->LoadShader((testRoot / "GraphicsQualityFill.cs.hlsl").generic_string());
        m_taa = m_resources->LoadShader((root / "PostProcess/AntiAliasing/TAA.hlsl").generic_string());
        auto resolveFile = root / "PostProcess/Reflections/SSRResolve.hlsl";
        /// @note 検出力の確認は一時コピーだけを壊す。本番アセットと他のビルドの入力を変更しない。
        if (const char* mutation = std::getenv("FBZZ_GRAPHICS_QUALITY_MUTATION");
            mutation && std::string_view(mutation) == "ssr-alpha") {
            std::ifstream input(resolveFile, std::ios::binary);
            ASSERT_TRUE(input);
            std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            constexpr std::string_view before = "return float4(weightedDelta, 1.0f);";
            const auto location = source.find(before);
            ASSERT_NE(location, std::string::npos);
            source.replace(location, before.size(), "return float4(weightedDelta, 0.0f);");
            resolveFile = testRoot / "SSRResolveMutation.hlsl";
            std::ofstream output(resolveFile, std::ios::binary);
            ASSERT_TRUE(output);
            output.write(source.data(), static_cast<std::streamsize>(source.size()));
            output.close();
            ASSERT_TRUE(output);
        }
        m_resolve = m_resources->LoadShader(resolveFile.generic_string());
        m_opaque = m_resources->CreatePipelineState({r::RasterizerMode::SOLID_NOCULL, r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_OFF});
        m_additive = m_resources->CreatePipelineState({r::RasterizerMode::SOLID_NOCULL, r::BlendMode::ADDITIVE, r::DepthMode::DEPTH_OFF});
        m_frame = m_resources->CreateConstantBuffer(sizeof(r::PerFrameCB));
        m_post = m_resources->CreateConstantBuffer(sizeof(r::PostProcCB));
        m_advanced = m_resources->CreateConstantBuffer(sizeof(r::AdvancedGraphicsCB));
        m_quality = m_resources->CreateConstantBuffer(sizeof(QualityConstants));
        m_taaCycle = m_resources->CreateConstantBuffer(sizeof(r::TaaCycleCB));
        m_output = m_resources->CreateRenderTarget(8, 8, r::RenderTargetDesc{2, r::Format::RGBA16F, false});
        m_history = m_resources->CreateRenderTarget(8, 8, r::RenderTargetDesc{2, r::Format::RGBA16F, false});
        m_delta = m_resources->CreateRenderTarget(8, 8, r::RenderTargetDesc{2, r::Format::RGBA16F, false});
        std::array<uint8_t, 8 * 8 * 4> checker{};
        for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
            const auto i = (y * 8 + x) * 4;
            const uint8_t value = (x + y) % 2 ? 255 : 0;
            checker[i] = checker[i+1] = checker[i+2] = value; checker[i+3] = 255;
        }
        m_current = m_resources->CreateTexture(checker.data(), 8, 8);
        const uint8_t white[]{255,255,255,255}, depth[]{128,128,128,255};
        m_white = m_resources->CreateTexture(white, 1, 1);
        m_depth = m_resources->CreateTexture(depth, 1, 1);
        m_cube = m_resources->CreateCubemapRenderTarget(8, 1);
        m_sh = m_resources->CreateComputeTexture3D(2, 1, 7);
        m_visibility = m_resources->CreateComputeTexture3D(16, 8, 1);
        ASSERT_TRUE(m_probe && m_fill && m_taa && m_resolve && m_opaque && m_additive);
        ASSERT_TRUE(m_frame && m_post && m_advanced && m_quality && m_taaCycle && m_output && m_history && m_delta);
        ASSERT_TRUE(m_current && m_white && m_depth && m_cube && m_sh && m_visibility);
        m_camera.m_projection = r::ProjectionMode::Orthographic;
        m_camera.m_orthoHeight = 2; m_camera.m_near = 0.1f; m_camera.m_far = 20;
        m_data.taaPreviousNearZ = m_camera.m_near;
        m_data.taaPreviousFarZ = m_camera.m_far;
        m_data.taaPreviousOrthographic = 1.0f;
    }
void GraphicsQualityShaderTest::TearDown()
    {
        if (m_open) End();
        m_resources.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.imguiRenderer.reset(); m_bundle.renderer.reset();
        r::SetShaderPathResolver(nullptr); core::Logger::RemoveSink(this);
        m_platform.reset();
        EXPECT_EQ(m_validationFailures, 0u);
        testkit::Fixture::TearDown();
    }
} /// @note namespace fbzz::tests
