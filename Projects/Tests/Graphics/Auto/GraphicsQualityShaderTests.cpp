/// @file    GraphicsQualityShaderTests.cpp
/// @brief   TAA、材質、鏡面置換と可視性補間の GPU 出力契約。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include "../Fixtures/GraphicsQualityFixture.hpp"
#include "../../../Engine/src/Scene/Systems/RenderPasses/Debug/DebugPasses.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Graphics/Renderer/DebugDraw.hpp>
#include <algorithm>
namespace fbzz::tests {
namespace {
TEST_F(GraphicsQualityShaderTest, TaaRejectsNewlyExposedDepth)
{
    const auto color = Taa(std::log2(1.0f + 2.0f));
    EXPECT_NEAR(color.x, 0, 0.001f); EXPECT_FLOAT_EQ(color.w, 1);
}
TEST_F(GraphicsQualityShaderTest, TaaRejectsSkyHistoryOnGeometry)
{
    EXPECT_NEAR(Taa(-1).x, 0, 0.001f);
}
TEST_F(GraphicsQualityShaderTest, TaaAccumulatesMatchingDepthWithOpaqueOutput)
{
    const float depth = 20.0f - (128.0f / 255.0f) * (20.0f - 0.1f);
    const auto color = Taa(std::log2(1.0f + depth));
    EXPECT_GT(color.x, 0.3f); EXPECT_LT(color.x, 0.7f); EXPECT_FLOAT_EQ(color.w, 1);
}
TEST_F(GraphicsQualityShaderTest, TaaAccumulatesObjectMovingInDepthUsingItsPreviousSurface)
{
    const float previousDepth = 2.0f;
    const float previousNdc = (20.0f - previousDepth) / (20.0f - 0.1f);
    EXPECT_GT(Taa(std::log2(1.0f + previousDepth), false, previousNdc).x, 0.3f);
}
TEST_F(GraphicsQualityShaderTest, TaaStillRejectsDifferentSurfaceWithDepthMotion)
{
    const float previousNdc = (20.0f - 3.0f) / (20.0f - 0.1f);
    EXPECT_NEAR(Taa(std::log2(1.0f + 2.0f), false, previousNdc).x, 0.0f, 0.001f);
}
TEST_F(GraphicsQualityShaderTest, ParticleReactiveMaskRejectsMostHistory)
{
    const float depth = 20.0f - (128.0f / 255.0f) * (20.0f - 0.1f);
    EXPECT_LT(Taa(std::log2(1.0f + depth), true).x, 0.15f);
}
TEST_F(GraphicsQualityShaderTest, SampledTaaAccumulatesSpatiallyConstantLightEstimates)
{
    const float depth = 20.0f - (128.0f / 255.0f) * (20.0f - 0.1f);
    EXPECT_GT(Taa(std::log2(1.0f + depth), false, -1.0f, true).x, 0.3f);
}
TEST_F(GraphicsQualityShaderTest, SampledTaaStillRejectsDisoccludedAndReactiveHistory)
{
    EXPECT_NEAR(Taa(std::log2(1.0f + 2.0f), false, -1.0f, true).x, 0.0f, 0.001f);
    const float depth = 20.0f - (128.0f / 255.0f) * (20.0f - 0.1f);
    EXPECT_LT(Taa(std::log2(1.0f + depth), true, -1.0f, true).x, 0.15f);
}
TEST_F(GraphicsQualityShaderTest, TaaCurrentOutputRemovesRasterJitterEvenWithoutHistory)
{
    const auto left = TaaCurrentRampAtJitter(-0.375f);
    const auto right = TaaCurrentRampAtJitter(0.375f);
    EXPECT_NEAR(left.x, 4.5f / 8.0f, 0.005f);
    EXPECT_NEAR(right.x, left.x, 0.005f);
}
TEST_F(GraphicsQualityShaderTest, TaaStabilizesStationarySilhouetteAcrossJitterPhases)
{
    constexpr std::array<float, 11> EXPECTED_MEANS{
        0.3415798611f, 0.9578993056f, 0.0277777778f, 0.6727430556f,
        0.1896701389f, 0.8875868056f, 0.4869791667f, 0.1150173611f,
        0.7955729167f, 0.3415798611f, 0.9578993056f
    };
    for (bool velocity : {false, true}) for (bool sky : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "velocity=" << velocity << ", sky=" << sky);

        const auto result = TaaStaticSilhouette(velocity, sky);
        const auto& samples = result.phases;
        const auto range = std::minmax_element(samples.begin(), samples.end());
        float maximumAmplitude = 0.0f, maximumMeanError = 0.0f;
        for (size_t point = 0; point < result.edgePhases.size(); ++point) {
            SCOPED_TRACE(point);
            const auto& pointSamples = result.edgePhases[point];
            const auto pointRange = std::minmax_element(pointSamples.begin(), pointSamples.end());
            const float amplitude = *pointRange.second - *pointRange.first;
            float total = 0.0f;
            for (const float sample : pointSamples) total += sample;
            const float mean = total / static_cast<float>(pointSamples.size());
            maximumAmplitude = std::max(maximumAmplitude, amplitude);
            maximumMeanError = std::max(maximumMeanError, std::abs(mean - EXPECTED_MEANS[point]));

            EXPECT_LE(amplitude, 0.000001f) << ::testing::PrintToString(pointSamples);
            EXPECT_NEAR(mean, EXPECTED_MEANS[point], 0.002f);
        }
        const std::string condition = std::string(velocity ? "velocity_" : "depth_") + (sky ? "sky" : "geometry");
        RecordProperty(condition + "_jitter_amplitude", std::to_string(maximumAmplitude));
        RecordProperty(condition + "_mean_color_error", std::to_string(maximumMeanError));
        RecordProperty(condition + "_exposed_background_error", std::to_string(std::abs(result.exposedBackground)));

        EXPECT_GT(*range.first, 0.2f);
        EXPECT_LT(*range.second, 0.8f);
        EXPECT_NEAR(result.exposedBackground, 0.0f, 0.002f);
        EXPECT_FLOAT_EQ(result.exposedHistoryLength, 1.0f);
        EXPECT_FLOAT_EQ(result.freshHistoryLength, 1.0f);
        EXPECT_FLOAT_EQ(result.movingHistoryLength, 1.0f);
        EXPECT_FLOAT_EQ(result.reactiveHistoryLength, 1.0f);
        float previous = 0.0f;
        for (const float color : result.changedColor) {
            EXPECT_GE(color, previous);
            EXPECT_LE(color, 1.0f);
            previous = color;
        }
        EXPECT_FLOAT_EQ(result.changedColor.back(), 1.0f);
    }
}
TEST_F(GraphicsQualityShaderTest, TaaCycleKeepsHdrModesAndTransitionsIsolated)
{
    constexpr std::array<float, 11> EXPECTED_MEANS{
        0.3415798611f, 0.9578993056f, 0.0277777778f, 0.6727430556f,
        0.1896701389f, 0.8875868056f, 0.4869791667f, 0.1150173611f,
        0.7955729167f, 0.3415798611f, 0.9578993056f
    };
    for (bool hdr : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "initialHdr=" << hdr);

        const auto result = TaaStaticSilhouette(true, false, hdr);
        for (size_t point = 0; point < result.edgePhases.size(); ++point) {
            SCOPED_TRACE(point);
            const auto& samples = result.edgePhases[point];
            const auto range = std::minmax_element(samples.begin(), samples.end());
            const float radiance = EXPECTED_MEANS[point];
            const float expected = hdr ? CompositeRadiance({radiance, radiance, radiance}).x : radiance;
            float total = 0.0f;
            for (const float sample : samples) total += sample;

            EXPECT_LE(*range.second - *range.first, 0.000001f) << ::testing::PrintToString(samples);
            EXPECT_NEAR(total / static_cast<float>(samples.size()), expected, 0.002f);
        }
        for (size_t phase = 0; phase < result.switchedHistoryLength.size(); ++phase) {
            SCOPED_TRACE(phase);
            EXPECT_FLOAT_EQ(result.switchedHistoryLength[phase], static_cast<float>(phase + 1u));
            EXPECT_GE(result.switchedColor[phase], 0.0f);
            EXPECT_LE(result.switchedColor[phase], 1.0f);
        }
        const float radiance = EXPECTED_MEANS[6];
        const float expected = hdr ? radiance : CompositeRadiance({radiance, radiance, radiance}).x;
        EXPECT_NEAR(result.switchedColor.back(), expected, 0.002f);
    }
}
TEST_F(GraphicsQualityShaderTest, TaaStaticProofPreservesSubpixelLinesMissingFromSomeJitterPhases)
{
    constexpr std::array<float, 11> EXPECTED_MEANS{
        0.0f, 0.015625f, 0.0f, 0.015625f, 0.015625f, 0.34375f,
        0.34375f, 0.34375f, 0.015625f, 0.015625f, 0.0f
    };
    for (bool hdr : {false, true}) for (bool sky : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "hdr=" << hdr << ", sky=" << sky);

        const auto result = TaaStaticSilhouette(false, sky, hdr, true, true);
        EXPECT_EQ(result.emptyFootprintPhases, 5u);
        for (size_t point = 0; point < result.edgePhases.size(); ++point) {
            SCOPED_TRACE(point);
            const auto& samples = result.edgePhases[point];
            const auto range = std::minmax_element(samples.begin(), samples.end());
            const float radiance = EXPECTED_MEANS[point];
            const float expected = hdr ? CompositeRadiance({radiance, radiance, radiance}).x : radiance;
            float total = 0.0f;
            for (const float sample : samples) total += sample;

            EXPECT_LE(*range.second - *range.first, 0.000001f) << ::testing::PrintToString(samples);
            EXPECT_NEAR(total / static_cast<float>(samples.size()), expected, 0.002f);
        }
        EXPECT_GT(result.phases.front(), 0.3f);
    }
}
TEST_F(GraphicsQualityShaderTest, GtaoSynchronizesStaticNoiseWithTaaJitterPhase)
{
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    const auto shader = m_resources->LoadShader((root / "PostProcess/AmbientOcclusion/GTAO.cs.hlsl").generic_string());
    const auto copy = m_resources->LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
    const auto raw = m_resources->CreateComputeTexture(4, 4);
    ASSERT_TRUE(shader && copy && raw);
    std::array<uint8_t, 8u * 8u * 4u> depthPixels{}, normalPixels{};
    for (uint32_t y = 0; y < 8u; ++y) for (uint32_t x = 0; x < 8u; ++x) {
        const auto offset = (y * 8u + x) * 4u;
        const uint8_t depth = x >= 4u && y >= 4u ? 240u : 230u;
        depthPixels[offset] = depthPixels[offset + 1u] = depthPixels[offset + 2u] = depth;
        depthPixels[offset + 3u] = 255u;
        normalPixels[offset] = normalPixels[offset + 1u] = 128u;
        normalPixels[offset + 3u] = 255u;
    }
    const auto depth = m_resources->CreateTexture(depthPixels.data(), 8, 8);
    const auto normal = m_resources->CreateTexture(normalPixels.data(), 8, 8);
    ASSERT_TRUE(depth && normal);
    constexpr std::array<math::Vector2, 8> JITTER_PIXELS{{
        {0.0f, -1.0f / 6.0f}, {-0.25f, 1.0f / 6.0f},
        {0.25f, -7.0f / 18.0f}, {-0.375f, -1.0f / 18.0f},
        {0.125f, 5.0f / 18.0f}, {-0.125f, -5.0f / 18.0f},
        {0.375f, 1.0f / 18.0f}, {-0.4375f, 7.0f / 18.0f}
    }};
    m_data.gtaoIntensity = 1.0f; m_data.gtaoRadius = 2.0f;
    m_data.gtaoSlices = 2; m_data.gtaoStepsPerSlice = 4;
    m_data.screenWidth = m_data.screenHeight = 8.0f;
    const auto render = [&](float time, uint32_t phase, bool taa) {
        m_data.taaFeedback = taa ? 0.9f : 0.0f;
        m_data.taaJitterX = JITTER_PIXELS[phase].x * 2.0f / 8.0f;
        m_data.taaJitterY = -JITTER_PIXELS[phase].y * 2.0f / 8.0f;
        Begin();
        auto post = r::MakeScreenPostProcCB(8, 8);
        post.time = time;
        m_resources->Update(m_post, &post, sizeof(post));
        r::ComputeCall dispatch;
        dispatch.shader = shader;
        dispatch.constantBuffers[0] = m_frame;
        dispatch.constantBuffers[5] = m_post;
        dispatch.constantBuffers[8] = m_advanced;
        dispatch.srvInputs[6] = normal; dispatch.srvInputs[7] = depth;
        dispatch.uavOutputs[6] = raw;
        m_bundle.renderer->Dispatch(dispatch, *m_resources);
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        auto draw = Draw(copy); draw.textures[5] = raw;
        m_bundle.renderer->Submit(draw, *m_resources);
        End();
        std::array<float, 64> result{};
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_output, *m_resources, pixels, width, height)
            || width != 8u || height != 8u) {
            ADD_FAILURE() << "GTAO readback failed"; return result;
        }
        for (size_t index = 0; index < result.size(); ++index) result[index] = pixels[index * 4u];
        return result;
    };
    const auto difference = [](const auto& left, const auto& right) {
        float maximum = 0.0f;
        for (size_t index = 0; index < left.size(); ++index)
            maximum = std::max(maximum, std::abs(left[index] - right[index]));
        return maximum;
    };

    const auto first = render(0.0f, 0u, true);
    const auto later = render(37.0f, 0u, true);
    float phaseDifference = 0.0f;
    for (uint32_t phase = 1u; phase < 8u; ++phase)
        phaseDifference = std::max(phaseDifference, difference(first, render(0.0f, phase, true)));
    const auto untimed = render(0.0f, 0u, false);
    const auto timed = render(37.0f, 0u, false);
    const auto nontrivial = std::count_if(first.begin(), first.end(), [](float value) { return value > 0.1f && value < 0.9f; });

    EXPECT_FLOAT_EQ(difference(first, later), 0.0f);
    EXPECT_GT(phaseDifference, 0.0001f);
    EXPECT_GT(difference(untimed, timed), 0.0001f);
    EXPECT_GT(nontrivial, 0);
    RecordProperty("taa_time_difference", std::to_string(difference(first, later)));
    RecordProperty("taa_phase_difference", std::to_string(phaseDifference));
    RecordProperty("untethered_time_difference", std::to_string(difference(untimed, timed)));
}
TEST_F(GraphicsQualityShaderTest, EditorDepthGridSharesModelJitterWhileOverlayRemainsStable)
{
    constexpr uint32_t SIZE = 64u;
    constexpr std::array<math::Vector2, 8> JITTER_PIXELS{{
        {0.0f, -1.0f / 6.0f}, {-0.25f, 1.0f / 6.0f},
        {0.25f, -7.0f / 18.0f}, {-0.375f, -1.0f / 18.0f},
        {0.125f, 5.0f / 18.0f}, {-0.125f, -5.0f / 18.0f},
        {0.375f, 1.0f / 18.0f}, {-0.4375f, 7.0f / 18.0f}
    }};
    const auto target = m_resources->CreateRenderTarget(SIZE, SIZE, r::CameraDepthTargetDesc(1));
    const auto modelShader = m_resources->LoadShader("Assets/Shaders/Debug/DebugDraw.hlsl");
    const auto modelCamera = m_resources->CreateConstantBuffer(sizeof(math::Matrix4));
    const auto modelState = m_resources->CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,
        r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_ON});
    ASSERT_TRUE(target && modelShader && modelCamera && modelState);
    const uint32_t indices[]{0,1,2,0,2,3};
    const auto indexBuffer = m_resources->CreateIndexBuffer(indices, 6u);
    std::array<r::ResourceHandle<r::BufferTag>, 2> modelVertices{};
    for (size_t model = 0; model < modelVertices.size(); ++model) {
        constexpr std::array<math::Vector2, 4> CORNERS{{{-40,-40}, {-40,40}, {40,40}, {40,-40}}};
        std::array<r::DebugDrawVertex, 4> vertices{};
        for (size_t corner = 0; corner < CORNERS.size(); ++corner) {
            const auto xz = CORNERS[corner];
            /// @note 線と三角形の被覆規則の差より十分離し、サブピクセルの接触ではなく遮蔽を検証する。
            /// @see https://learn.microsoft.com/en-gb/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules Rasterization rules for triangles and lines.
            vertices[corner] = {{xz.x, 0.5f + (model == 0u ? 0.0f : 0.15f * xz.x), xz.y}, {0,0,0,1}};
        }
        modelVertices[model] = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(r::DebugDrawVertex));
        ASSERT_TRUE(modelVertices[model]);
    }
    ASSERT_TRUE(indexBuffer);
    m_camera.m_position = {3,4,-6}; m_camera.LookAt({0,0,0});
    m_camera.m_aspect = 1.0f; m_camera.m_orthoHeight = 8.0f; m_camera.m_far = 50.0f;
    scene::Scene source;
    r::RenderSettings settings; settings.showGrid = true;
    r::RenderPassHandles handles;
    scene::RenderPassContext context{source, *m_bundle.renderer, *m_resources, m_camera, settings, target, ~0u, handles};
    context.resourceRegistry.BindTarget("HDR", target);
    context.resourceRegistry.BindTarget("Output", target);
    scene::GridDebugPass grid;
    const auto execute = [&](scene::IRenderPass& pass) {
        scene::PassBuilder builder;
        pass.Setup(builder, context);
        scene::PassResources accesses(context.resourceRegistry, builder.Accesses(), pass.Name());
        pass.Execute(accesses, context);
    };
    r::DebugDrawCapture capture;
    Begin();
    m_bundle.renderer->SetRenderTarget(target, *m_resources);
    m_bundle.renderer->Clear({0,0,0,1});
    r::DebugDraw::BeginCapture(capture);
    execute(grid);
    r::DebugDraw::EndCapture();
    End();
    ASSERT_EQ(capture.depthLines.size(), 164u);
    ASSERT_TRUE(capture.overlayLines.empty());
    r::DebugDrawCapture gizmos = capture;
    gizmos.overlayLines = capture.depthLines;
    scene::ScriptGizmoPass depthGizmos{"HDR", "TestDepthGizmos", r::DebugDrawLayer::DepthTested, gizmos};
    scene::ScriptGizmoPass overlayGizmos{"Output", "TestOverlayGizmos", r::DebugDrawLayer::Overlay, gizmos};
    enum class Kind { GRID, DEPTH_GIZMOS, OVERLAY_GIZMOS };
    const auto render = [&](uint32_t phase, uint32_t model, Kind kind, bool reference) {
        context.taaJitterNdcX = 2.0f * JITTER_PIXELS[phase].x / static_cast<float>(SIZE);
        context.taaJitterNdcY = -2.0f * JITTER_PIXELS[phase].y / static_cast<float>(SIZE);
        const auto frame = r::MakeCameraFrameCB(m_camera, context.taaJitterNdcX, context.taaJitterNdcY);
        Begin();
        m_bundle.renderer->SetRenderTarget(target, *m_resources);
        m_bundle.renderer->Clear({0,0,0,1});
        if (model > 0u) {
            m_resources->Update(modelCamera, &frame.viewProjection, sizeof(frame.viewProjection));
            r::DrawCall draw;
            draw.shader = modelShader; draw.pipelineState = modelState;
            draw.vertexBuffer = modelVertices[model - 1u]; draw.indexBuffer = indexBuffer;
            draw.vertexCount = 4u; draw.indexCount = 6u; draw.constantBuffers[0] = modelCamera;
            m_bundle.renderer->Submit(draw, *m_resources);
        }
        if (reference) {
            const auto viewProjection = kind == Kind::OVERLAY_GIZMOS
                ? m_camera.GetGpuViewProjection() : frame.viewProjection;
            r::DebugDraw::BeginFrame(*m_bundle.renderer, *m_resources, viewProjection);
            r::DebugDraw::Replay(*m_bundle.renderer, gizmos,
                kind == Kind::OVERLAY_GIZMOS ? r::DebugDrawLayer::Overlay : r::DebugDrawLayer::DepthTested);
            r::DebugDraw::Flush();
        } else {
            switch (kind) {
            case Kind::GRID: execute(grid); break;
            case Kind::DEPTH_GIZMOS: execute(depthGizmos); break;
            case Kind::OVERLAY_GIZMOS: execute(overlayGizmos); break;
            }
        }
        End();
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToLinearRGBA(target, *m_resources, pixels, width, height)
            || width != SIZE || height != SIZE) {
            ADD_FAILURE() << "Editor grid readback failed"; pixels.clear();
        }
        return pixels;
    };
    const auto difference = [](const auto& left, const auto& right) {
        float maximum = 0.0f;
        for (size_t index = 0; index < left.size(); index += 4u)
            for (size_t channel = 0; channel < 3u; ++channel)
                maximum = std::max(maximum, std::abs(left[index + channel] - right[index + channel]));
        return maximum;
    };
    const auto visiblePixels = [](const auto& pixels) {
        uint32_t count = 0;
        for (size_t index = 0; index < pixels.size(); index += 4u)
            if (std::max({pixels[index], pixels[index + 1u], pixels[index + 2u]}) > 0.05f) ++count;
        return count;
    };
    float maximumError = 0.0f;
    for (const auto projection : {r::ProjectionMode::Perspective, r::ProjectionMode::Orthographic}) {
        SCOPED_TRACE(static_cast<uint32_t>(projection));
        m_camera.m_projection = projection;
        for (const auto kind : {Kind::GRID, Kind::DEPTH_GIZMOS}) for (uint32_t phase = 0; phase < 8u; ++phase) {
            SCOPED_TRACE(::testing::Message() << "kind=" << static_cast<uint32_t>(kind) << ", phase=" << phase);
            std::array<uint32_t, 3> visible{};
            for (uint32_t model = 0; model < 3u; ++model) {
                SCOPED_TRACE(model);
                const auto actual = render(phase, model, kind, false);
                const auto expected = render(phase, model, kind, true);
                ASSERT_EQ(actual.size(), static_cast<size_t>(SIZE) * SIZE * 4u);
                ASSERT_EQ(expected.size(), actual.size());
                const float error = difference(actual, expected);
                maximumError = std::max(maximumError, error);

                EXPECT_FLOAT_EQ(error, 0.0f);
                visible[model] = visiblePixels(actual);
            }
            EXPECT_GT(visible[0], 0u);
            EXPECT_EQ(visible[1], 0u);
            EXPECT_GT(visible[2], 0u);
            EXPECT_LT(visible[2], visible[0]);
        }
        const auto overlayReference = render(0u, 1u, Kind::OVERLAY_GIZMOS, true);
        ASSERT_EQ(overlayReference.size(), static_cast<size_t>(SIZE) * SIZE * 4u);
        EXPECT_GT(visiblePixels(overlayReference), 0u);
        for (uint32_t phase = 0; phase < 8u; ++phase) {
            const auto actual = render(phase, 1u, Kind::OVERLAY_GIZMOS, false);
            ASSERT_EQ(actual.size(), overlayReference.size());
            EXPECT_FLOAT_EQ(difference(actual, overlayReference), 0.0f);
        }
    }
    RecordProperty("editor_grid_model_phase_max_error", std::to_string(maximumError));
}
TEST_F(GraphicsQualityShaderTest, DeclaredUnlitMatchesOpaqueStandardGeometryWithoutFilenameInference)
{
    constexpr uint32_t SIZE = 32u;
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    const auto source = root / "Material/Surface/Unlit.hlsl";
    const auto declared = m_temp.File("Shaders/DeclaredSurface.hlsl");
    const auto undeclared = m_temp.File("Shaders/Unlit.hlsl");
    std::error_code error;
    ASSERT_TRUE(std::filesystem::copy_file(source, declared, error)); ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::copy_file(source.string() + ".meta", declared.string() + ".meta", error)); ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::copy_file(source, undeclared, error)); ASSERT_FALSE(error);
    asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
    struct AssetLifetime { ~AssetLifetime() { asset::AssetManager::UnloadAll(); } } lifetime;
    const auto unlit = m_resources->LoadShader(declared.generic_string());
    const auto unknown = m_resources->LoadShader(undeclared.generic_string());
    const auto standard = m_resources->LoadShader((root / "Pipeline/Deferred/GBuffer.hlsl").generic_string());
    ASSERT_TRUE(unlit && unknown && standard);
    const auto contract = m_resources->GetShaderCapabilities(unlit);
    EXPECT_EQ(contract.vertex, r::ShaderVertexContract::STANDARD_SURFACE);
    EXPECT_EQ(contract.surface, r::ShaderSurfaceContract::UNLIT);
    EXPECT_EQ(contract.opacity, r::ShaderOpacityContract::OPAQUE_OUTPUT);
    EXPECT_EQ(contract.variants, r::ShaderVariantSet::NONE);
    EXPECT_TRUE(contract.HasStandardGeometry());
    EXPECT_FALSE(contract.SupportsGBuffer());
    const auto noDeclaration = m_resources->GetShaderCapabilities(unknown);
    EXPECT_EQ(noDeclaration.vertex, r::ShaderVertexContract::UNKNOWN);
    EXPECT_EQ(noDeclaration.opacity, r::ShaderOpacityContract::UNKNOWN);
    const auto actualTarget = m_resources->CreateRenderTarget(SIZE, SIZE, r::CameraDepthTargetDesc(1));
    const auto referenceTarget = m_resources->CreateRenderTarget(SIZE, SIZE, r::CameraDepthTargetDesc(3));
    const auto objectCB = m_resources->CreateConstantBuffer(sizeof(r::PerObjectCB));
    const auto state = m_resources->CreatePipelineState({r::RasterizerMode::SOLID_NOCULL,
        r::BlendMode::OPAQUE_BLEND, r::DepthMode::DEPTH_ON});
    ASSERT_TRUE(actualTarget && referenceTarget && objectCB && state);
    std::array<r::Vertex, 4> vertices{};
    constexpr std::array<math::Vector2, 4> CORNERS{{{-0.5f,-0.5f}, {-0.5f,0.5f}, {0.5f,0.5f}, {0.5f,-0.5f}}};
    for (size_t corner = 0; corner < vertices.size(); ++corner) {
        vertices[corner].position = {CORNERS[corner].x, CORNERS[corner].y, 0};
        vertices[corner].normal = {0,0,-1}; vertices[corner].tangent = {1,0,0};
        vertices[corner].color = {1,1,1,0};
    }
    const uint32_t indices[]{0,1,2,0,2,3};
    const auto vertexBuffer = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(r::Vertex));
    const auto indexBuffer = m_resources->CreateIndexBuffer(indices, 6u);
    const auto parameters = [&](r::ResourceHandle<r::ShaderTag> shader, math::Vector4 color) {
        const auto& descriptor = m_resources->Get(shader)->GetDescriptor();
        std::vector<uint8_t> bytes(descriptor.cbufferSize);
        asset::InitDefaultMaterialParams(descriptor, bytes);
        asset::ApplyMaterialParamOverrides({{"albedo", {color.x, color.y, color.z, color.w}}}, descriptor, bytes);
        return bytes;
    };
    const auto frontUnlit = parameters(unlit, {0.25f,0.5f,0.75f,0});
    const auto frontStandard = parameters(standard, {0.25f,0.5f,0.75f,1});
    const auto behind = parameters(unlit, {1,0,0,0});
    const auto unlitCB = m_resources->CreateConstantBuffer(frontUnlit.size());
    const auto standardCB = m_resources->CreateConstantBuffer(frontStandard.size());
    ASSERT_TRUE(vertexBuffer && indexBuffer && unlitCB && standardCB);
    r::PerObjectCB front{};
    front.world = math::Matrix4::Identity();
    front.world.m[0][0] = 0.5f; front.world.m[1][1] = 1.25f;
    front.world.m[0][3] = 0.5f; front.world.m[1][3] = -0.25f; front.world.m[2][3] = 3.0f;
    front.worldInvTranspose = math::Matrix4::InverseTransposeAffine(front.world);
    auto back = front; back.world.m[2][3] = 4.0f;
    back.worldInvTranspose = math::Matrix4::InverseTransposeAffine(back.world);
    m_camera.m_aspect = 1.0f;
    constexpr std::array<math::Vector2, 8> JITTER_PIXELS{{
        {0.0f, -1.0f / 6.0f}, {-0.25f, 1.0f / 6.0f},
        {0.25f, -7.0f / 18.0f}, {-0.375f, -1.0f / 18.0f},
        {0.125f, 5.0f / 18.0f}, {-0.125f, -5.0f / 18.0f},
        {0.375f, 1.0f / 18.0f}, {-0.4375f, 7.0f / 18.0f}
    }};
    const auto render = [&](uint32_t phase, bool reference) {
        Begin();
        const auto frame = r::MakeCameraFrameCB(m_camera, 2.0f * JITTER_PIXELS[phase].x / SIZE,
            -2.0f * JITTER_PIXELS[phase].y / SIZE);
        m_resources->Update(m_frame, &frame, sizeof(frame));
        const auto target = reference ? referenceTarget : actualTarget;
        m_bundle.renderer->SetRenderTarget(target, *m_resources);
        m_bundle.renderer->Clear({0,0,0,1});
        m_resources->Update(objectCB, &front, sizeof(front));
        m_resources->Update(unlitCB, frontUnlit.data(), frontUnlit.size());
        m_resources->Update(standardCB, frontStandard.data(), frontStandard.size());
        auto draw = Draw(reference ? standard : unlit);
        draw.pipelineState = state; draw.vertexBuffer = vertexBuffer; draw.indexBuffer = indexBuffer;
        draw.vertexCount = 4u; draw.indexCount = 6u; draw.constantBuffers[1] = objectCB;
        draw.constantBuffers[2] = reference ? standardCB : unlitCB;
        m_bundle.renderer->Submit(draw, *m_resources);
        m_resources->Update(objectCB, &back, sizeof(back));
        m_resources->Update(unlitCB, behind.data(), behind.size());
        draw.shader = unlit; draw.constantBuffers[2] = unlitCB;
        m_bundle.renderer->Submit(draw, *m_resources);
        End();
        std::vector<float> pixels; uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToLinearRGBA(target, *m_resources, pixels, width, height)
            || width != SIZE || height != SIZE) {
            ADD_FAILURE() << "Unlit contract readback failed"; pixels.clear();
        }
        return pixels;
    };
    for (uint32_t phase = 0u; phase < 8u; ++phase) {
        SCOPED_TRACE(phase);
        const auto actual = render(phase, false);
        const auto expected = render(phase, true);
        ASSERT_EQ(actual.size(), static_cast<size_t>(SIZE) * SIZE * 4u);
        ASSERT_EQ(expected.size(), actual.size());
        uint32_t covered = 0;
        for (size_t pixel = 0; pixel < actual.size(); pixel += 4u) {
            for (size_t channel = 0; channel < 3u; ++channel)
                EXPECT_NEAR(actual[pixel + channel], expected[pixel + channel], 0.001f);
            if (actual[pixel + 2u] > 0.5f) {
                ++covered;
                EXPECT_FLOAT_EQ(actual[pixel + 3u], 0.0f);
            }
        }
        EXPECT_GT(covered, 100u);
        const size_t interior = (20u * SIZE + 24u) * 4u;
        EXPECT_NEAR(actual[interior], 0.25f, 0.001f);
        EXPECT_NEAR(actual[interior + 1u], 0.5f, 0.001f);
        EXPECT_NEAR(actual[interior + 2u], 0.75f, 0.001f);
        EXPECT_FLOAT_EQ(actual[(16u * SIZE + 8u) * 4u + 2u], 0.0f);
    }
}
TEST_F(GraphicsQualityShaderTest, SampledTaaAveragesRadianceBeforeToneMapping)
{
    const float depth = 20.0f - (128.0f / 255.0f) * (20.0f - 0.1f);
    const auto accumulated = TaaRadiance({100, 20, 2}, {}, -1.0f - std::log2(1.0f + depth));
    const auto reference = CompositeRadiance({10, 2, 0.2f});
    EXPECT_VEC3_NEAR(accumulated.XYZ(), reference.XYZ(), 0.002f);
    EXPECT_GT(accumulated.x, 0.95f); EXPECT_FLOAT_EQ(accumulated.w, 1);
}
TEST_F(GraphicsQualityShaderTest, SampledHdrTaaRetainsBrightnessAndFadeAfterAccumulation)
{
    const float depth = 20.0f - (128.0f / 255.0f) * (20.0f - 0.1f);
    const auto accumulated = TaaRadiance({100, 20, 2}, {}, -1.0f - std::log2(1.0f + depth), false, 0.7f, 0.4f);
    const auto reference = CompositeRadiance({10, 2, 0.2f}, 0.7f, 0.4f);
    EXPECT_VEC3_NEAR(accumulated.XYZ(), reference.XYZ(), 0.002f);
}
TEST_F(GraphicsQualityShaderTest, SampledHdrTaaRejectsOtherDepthAndLdrHistory)
{
    const auto otherDepth = TaaRadiance({}, {100, 100, 100}, -1.0f - std::log2(3.0f));
    const float depth = 20.0f - (128.0f / 255.0f) * (20.0f - 0.1f);
    const auto ldrHistory = TaaRadiance({}, {100, 100, 100}, std::log2(1.0f + depth));
    EXPECT_VEC3_NEAR(otherDepth.XYZ(), math::Vector3{}, 0.001f);
    EXPECT_VEC3_NEAR(ldrHistory.XYZ(), math::Vector3{}, 0.001f);
}
TEST_F(GraphicsQualityShaderTest, SampledSpatialReconstructionReusesNeighborRadiance)
{
    EXPECT_GT(TaaSpatial(0).x, 0.5f);
}
TEST_F(GraphicsQualityShaderTest, SampledSpatialReconstructionPreservesMaterialNormalAndDepthEdges)
{
    for (uint32_t edge : {1u, 2u, 3u}) {
        SCOPED_TRACE(edge);
        EXPECT_LT(TaaSpatial(edge).x, 0.005f);
    }
}
TEST_F(GraphicsQualityShaderTest, InFrameTextureUpdatesPreserveDrawOrderAndRetainReleasedStorage)
{
    const auto copy = m_resources->LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    const auto dynamic = m_resources->CreateDynamicTexture(1, 1, r::DynamicTextureFormat::RGBA8);
    ASSERT_TRUE(copy && dynamic);
    const std::array<uint8_t,4> red{255,0,0,255}, green{0,255,0,255}, blue{0,0,255,255};
    Begin();
    ASSERT_TRUE(m_resources->Get(dynamic)->UpdateRegion(0,0,1,1,red.data(),4));
    m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
    auto draw = Draw(copy); draw.textures[5] = dynamic;
    m_bundle.renderer->Submit(draw, *m_resources);
    const auto unrelated = m_resources->CreateTexture(blue.data(),1,1);
    ASSERT_TRUE(unrelated);
    ASSERT_TRUE(m_resources->Get(dynamic)->UpdateRegion(0,0,1,1,green.data(),4));
    m_bundle.renderer->SetRenderTarget(m_delta, *m_resources);
    m_bundle.renderer->Submit(draw, *m_resources);
    m_resources->Release(dynamic);
    End();
    const auto first = Read();
    EXPECT_NEAR(first.x,1,0.001f); EXPECT_NEAR(first.y,0,0.001f);
    std::vector<float> second; uint32_t width=0,height=0;
    ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_delta,*m_resources,second,width,height));
    ASSERT_EQ(width,8u); ASSERT_EQ(height,8u);
    EXPECT_NEAR(second[(4u*8u+4u)*4u],0,0.001f);
    EXPECT_NEAR(second[(4u*8u+4u)*4u+1u],1,0.001f);
}
TEST_F(GraphicsQualityShaderTest, EnvironmentSpecularRetainsItsColor)
{
    Begin();
    for (uint32_t face = 0; face < 6; ++face) {
        m_bundle.renderer->SetRenderTargetFace(m_cube, face, 0, *m_resources);
        m_bundle.renderer->Clear({1,0,0,1});
    }
    Probe(m_output, {1}); End();
    const auto color = Read();
    EXPECT_GT(color.x, 0.5f); EXPECT_NEAR(color.y, 0, 0.001f); EXPECT_NEAR(color.z, 0, 0.001f);
}
TEST_F(GraphicsQualityShaderTest, OccludedDiffuseDoesNotAcquireConstantAmbientFloor)
{
    Begin();
    for (uint32_t face = 0; face < 6; ++face) {
        m_bundle.renderer->SetRenderTargetFace(m_cube, face, 0, *m_resources);
        m_bundle.renderer->Clear({0,0,0,1});
    }
    Probe(m_output, {2}); End(); EXPECT_NEAR(Read().x, 0, 0.001f);
}
TEST_F(GraphicsQualityShaderTest, VisibilitySuppressesBrightProbeBehindThinWall)
{
    EXPECT_GT(Gi(1, false).x, 0.4f);
    EXPECT_NEAR(Gi(1, true).x, 0.1f, 0.005f);
}
TEST_F(GraphicsQualityShaderTest, EntirelyOccludedCageDoesNotLeakSkyFallback)
{
    EXPECT_NEAR(Gi(2, true).x, 0, 0.001f);
}
TEST_F(GraphicsQualityShaderTest, VisibilityPreservesOpenCageLighting)
{
    EXPECT_GT(Gi(0, true).x, 0.1f);
    EXPECT_LT(Gi(0, true).x, 1.0f);
}
TEST_F(GraphicsQualityShaderTest, ReflectionResolveAddsSignedSpecularDeltaAndKeepsAlpha)
{
    m_data.ssrIntensity = 1;
    Begin(); Probe(m_delta, {4});
    m_bundle.renderer->SetRenderTarget(m_output, *m_resources); m_bundle.renderer->Clear({0.8f,0.8f,0.8f,1});
    auto draw = Draw(m_resolve); draw.pipelineState = m_additive;
    draw.textures[19] = m_resources->GetColorTexture(m_delta, 0);
    m_bundle.renderer->Submit(draw, *m_resources); End();
    const auto color = Read();
    EXPECT_NEAR(color.x, 0.6f, 0.002f); EXPECT_NEAR(color.y, 1.1f, 0.002f);
    EXPECT_NEAR(color.z, 0.7f, 0.002f); EXPECT_FLOAT_EQ(color.w, 1);
}

TEST_F(GraphicsQualityShaderTest, ReflectionMissPreservesTransparentCameraBackground)
{
    m_data.ssrIntensity = 1;
    Begin(); Probe(m_delta, {5});
    m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
    m_bundle.renderer->Clear({0.8f,0.8f,0.8f,0.25f});
    auto draw = Draw(m_resolve); draw.pipelineState = m_additive;
    draw.textures[19] = m_resources->GetColorTexture(m_delta, 0);
    m_bundle.renderer->Submit(draw, *m_resources); End();
    const auto color = Read();
    EXPECT_NEAR(color.x, 0.8f, 0.002f); EXPECT_FLOAT_EQ(color.w, 0.25f);
}

TEST_F(GraphicsQualityShaderTest, MovingLaterSubmeshWritesVelocityWhenFirstSlotIsHidden)
{
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    const auto shader = m_resources->LoadShader((root / "Motion/Velocity.hlsl").generic_string());
    ASSERT_TRUE(shader.IsValid());
    const r::Vertex vertices[] = {
        {{-1,-1,3},{0,0,-1},{1,0,0},{0,1}}, {{-1,1,3},{0,0,-1},{1,0,0},{0,0}},
        {{1,1,3},{0,0,-1},{1,0,0},{1,0}}, {{1,-1,3},{0,0,-1},{1,0,0},{1,1}}
    };
    const uint32_t indices[]{0,1,2,0,2,3};
    const auto vertex = m_resources->CreateVertexBuffer(vertices, sizeof(vertices), sizeof(r::Vertex));
    const auto index = m_resources->CreateIndexBuffer(indices, 6);
    const auto hdr = m_resources->CreateRenderTarget(8, 8, r::CameraDepthTargetDesc(1));
    const auto velocity = m_resources->CreateRenderTarget(8, 8, r::CameraDepthTargetDesc(1));
    const auto objectCB = m_resources->CreateConstantBuffer(sizeof(r::PerObjectCB));
    ASSERT_TRUE(vertex && index && hdr && velocity && objectCB);
    auto scene = std::make_shared<r::RenderScene>();
    r::RenderObject object;
    object.colorEligible = true; object.itemCount = 2; object.previousWorld.m[0][3] = -0.5f;
    scene->objects.push_back(object);
    r::RenderMeshItem hidden; hidden.slotVisible = false;
    scene->items.push_back(hidden);
    r::RenderMeshItem visible;
    visible.vertexBuffer = vertex; visible.indexBuffer = index; visible.indexCount = 6; visible.vertexCount = 4;
    scene->items.push_back(visible);
    r::RenderSettings settings; settings.gpuInstancing = false;
    r::RenderPassHandles handles; handles.frameCB = m_frame; handles.objectCB = objectCB;
    handles.advancedGraphicsCB = m_advanced; handles.velocityShader = shader;
    r::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, settings, velocity, ~0u, handles};
    context.renderScene = scene; context.frustumCullingEnabled = false;
    context.resourceRegistry.BindTarget("HDR", hdr); context.resourceRegistry.BindTarget("Velocity", velocity);
    m_data.prevViewProjection = r::MakeCameraFrameCB(m_camera, 0, 0).viewProjection;
    Begin(); m_bundle.renderer->SetRenderTarget(hdr, *m_resources); m_bundle.renderer->Clear({0,0,0,1});
    r::VelocityPass pass;
    r::PassBuilder builder; pass.Setup(builder, context);
    r::PassResources passResources(context.resourceRegistry, builder.Accesses(), "Velocity");
    pass.Execute(passResources, context); End();
    std::vector<float> pixels; uint32_t width = 0, height = 0;
    ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(velocity, *m_resources, pixels, width, height));
    ASSERT_EQ(width, 8u); ASSERT_EQ(height, 8u);
    const auto i = (4 * 8 + 4) * 4;
    EXPECT_GT(pixels[i], 0.05f); EXPECT_NEAR(pixels[i+2], 1, 0.001f);
}

TEST_F(GraphicsQualityShaderTest, TerrainGbufferRetainsAuthoredRoughnessAndNormalStrength)
{
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    const auto shader = m_resources->LoadShader((root / "Terrain/TerrainGBuffer.hlsl").generic_string());
    const auto copy = m_resources->LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
    ASSERT_TRUE(shader && copy);
    const uint8_t black[]{0,0,0,0}, weights[]{255,0,0,0}, tilted[]{192,128,255,255};
    const auto indicesTexture = m_resources->CreateTexture(black, 1, 1);
    const auto weightTexture = m_resources->CreateTexture(weights, 1, 1);
    const auto normal = m_resources->CreateTexture(tilted, 1, 1);
    const auto gbuffer = m_resources->CreateRenderTarget(8, 8, {3, r::Format::RGBA16F, false});
    const auto terrainCB = m_resources->CreateConstantBuffer(sizeof(r::TerrainObjectCB));
    r::TerrainLayerGpu layer;
    layer.diffuseIndex = m_resources->Get(m_white)->GetBindlessIndex();
    layer.normalIndex = m_resources->Get(normal)->GetBindlessIndex();
    layer.aoRoughnessIndex = layer.heightIndex = layer.diffuseIndex;
    layer.roughness = 0.1f; layer.tilingX = layer.tilingZ = 1;
    const auto layers = m_resources->CreateStructuredBuffer(&layer, 1, sizeof(layer));
    ASSERT_TRUE(indicesTexture && weightTexture && normal && gbuffer && terrainCB && layers);
    const r::Vertex vertices[] = {
        {{-1,-1,3},{0,0,1},{1,0,0},{0,1}}, {{-1,1,3},{0,0,1},{1,0,0},{0,0}},
        {{1,1,3},{0,0,1},{1,0,0},{1,0}}, {{1,-1,3},{0,0,1},{1,0,0},{1,1}}
    };
    const uint32_t indices[]{0,1,2,0,2,3};
    const auto vertex = m_resources->CreateVertexBuffer(vertices, sizeof(vertices), sizeof(r::Vertex));
    const auto index = m_resources->CreateIndexBuffer(indices, 6);
    ASSERT_TRUE(vertex && index);
    r::TerrainObjectCB terrain{};
    terrain.worldMatrix = math::Matrix4::Identity();
    terrain.wvpMatrix = r::MakeCameraFrameCB(m_camera, 0, 0).viewProjection;
    terrain.terrainParams = {2,2,0,0}; terrain.layerCount = 1;
    terrain.layerBufferIndex = m_resources->Get(layers)->GetBindlessIndex();
    Begin(); m_resources->Update(terrainCB, &terrain, sizeof(terrain));
    m_bundle.renderer->SetRenderTarget(gbuffer, *m_resources); m_bundle.renderer->Clear({0,0,0,0});
    auto draw = Draw(shader); draw.vertexBuffer = vertex; draw.indexBuffer = index; draw.indexCount = 6;
    draw.vertexCount = 4; draw.constantBuffers[1] = terrainCB;
    draw.textures[0] = indicesTexture; draw.textures[1] = weightTexture;
    draw.indirectReadTextures = {m_white, normal};
    m_bundle.renderer->Submit(draw, *m_resources);
    m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
    auto blit = Draw(copy); blit.textures[5] = m_resources->GetColorTexture(gbuffer, 0);
    m_bundle.renderer->Submit(blit, *m_resources); End();
    EXPECT_NEAR(Read().w, 0.1f, 0.002f);
    Begin(); m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
    blit.textures[5] = m_resources->GetColorTexture(gbuffer, 1);
    m_bundle.renderer->Submit(blit, *m_resources); End();
    EXPECT_NEAR(Read().x, 0.725f, 0.01f);
    for (const auto [scale, expected] : {std::pair{2.0f, 0.623f}, std::pair{-2.0f, 0.377f}}) {
        SCOPED_TRACE(scale);
        terrain.worldMatrix.m[0][0] = scale;
        terrain.worldInvTranspose = math::Matrix4::InverseTransposeAffine(terrain.worldMatrix);
        terrain.wvpMatrix = r::MakeCameraFrameCB(m_camera, 0, 0).viewProjection * terrain.worldMatrix;
        Begin(); m_resources->Update(terrainCB, &terrain, sizeof(terrain));
        m_bundle.renderer->SetRenderTarget(gbuffer, *m_resources);
        m_bundle.renderer->Clear({0,0,0,0});
        m_bundle.renderer->Submit(draw, *m_resources);
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        m_bundle.renderer->Submit(blit, *m_resources); End();
        EXPECT_NEAR(Read().x, expected, 0.01f);
    }
}

TEST_F(GraphicsQualityShaderTest, CompilesTerrainReflectionAndProbeProductionShaders)
{
    const auto root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT);
    for (const auto* file : {"Terrain/Terrain.hlsl", "Terrain/TerrainGBuffer.hlsl",
         "PostProcess/Reflections/SSR.cs.hlsl", "IBL/LightProbeProject.cs.hlsl",
         "Material/Surface/PBR.hlsl", "Pipeline/Deferred/DeferredLighting.hlsl"}) {
        SCOPED_TRACE(file);
        EXPECT_TRUE(m_resources->LoadShader((root / file).generic_string()).IsValid());
    }
}
} /// @note namespace
} /// @note namespace fbzz::tests
