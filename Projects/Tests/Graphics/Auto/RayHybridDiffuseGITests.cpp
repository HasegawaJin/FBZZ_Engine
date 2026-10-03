/// @file    RayHybridDiffuseGITests.cpp
/// @brief   Shared point diffuse response, environment accounting and immutable probe provenance.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {
using Pixels = std::array<math::Vector4, 4>;

class RayHybridDiffuseGITest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Hybrid diffuse GI regression", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 32, 32);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        if (!m_bundle.renderer->GetCapabilities().inlineRayQuery)
            GTEST_SKIP() << "Production reflection RayQuery is unavailable";
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        const std::filesystem::path root(FBZZ_GRAPHICS_SHADER_ROOT);
        m_source = std::make_unique<testkit::TempDir>("hybrid_point_gi");
        ASSERT_TRUE(m_source->IsValid());
        /// @note Snapshot production source dependencies under a real shader root; no authored assets or local CSOs are modified.
        const auto staged = m_source->File("Shaders");
        std::error_code error;
        for (const auto& file : std::filesystem::recursive_directory_iterator(root, error)) {
            if (!file.is_regular_file(error)) continue;
            const auto extension = file.path().extension();
            if (extension != ".hlsl" && extension != ".hlsli") continue;
            const auto destination = staged / file.path().lexically_relative(root);
            std::filesystem::create_directories(destination.parent_path(), error);
            ASSERT_FALSE(error) << error.message();
            std::filesystem::copy_file(file.path(), destination, std::filesystem::copy_options::overwrite_existing, error);
            ASSERT_FALSE(error) << error.message();
        }
        const auto testSource = (root / "../../Projects/Tests/Graphics/Shaders/RayHybridDiffuseGI.cs.hlsl").lexically_normal();
        const auto testShader = staged / "RayTracing/PointGiValidation.cs.hlsl";
        std::filesystem::copy_file(testSource, testShader, std::filesystem::copy_options::overwrite_existing, error);
        ASSERT_FALSE(error) << error.message();
        m_shader = m_resources->LoadShader(testShader.generic_string());
        m_copy = m_resources->LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_reconstruction = m_resources->LoadShader((root / "RayTracing/RayReflectionReconstruction.cs.hlsl").generic_string());
        m_constants = m_resources->CreateConstantBuffer(sizeof(renderer::RayReflectionConstants));
        m_point = m_resources->CreateConstantBuffer(2 * sizeof(math::Vector4));
        m_handles.advancedGraphicsCB = m_resources->CreateConstantBuffer(sizeof(renderer::AdvancedGraphicsCB));
        m_cube = m_resources->CreateCubemapRenderTarget(1);
        m_prefilterCube = m_resources->CreateCubemapRenderTarget(1);
        m_handles.iblIrradiance = m_resources->GetCubemapTexture(m_cube);
        m_handles.iblPrefilter = m_resources->GetCubemapTexture(m_prefilterCube);
        const std::array<uint8_t, 4> black{};
        m_handles.iblBrdfLut = m_resources->CreateTexture(black.data(), 1, 1);
        m_output = m_resources->CreateComputeTexture(4, 1);
        m_target = m_resources->CreateRenderTarget(4, 1, {1, renderer::Format::RGBA16F, false});
        m_pipeline = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(m_shader && m_copy && m_reconstruction && m_constants && m_point
            && m_handles.advancedGraphicsCB && m_cube && m_prefilterCube && m_handles.iblIrradiance
            && m_handles.iblPrefilter && m_handles.iblBrdfLut && m_output && m_target && m_pipeline);
        m_data.iblIntensity = 1;
        m_data.iblDiffuseScale = 1;
        m_data.iblSpecularScale = 1;
        m_camera.m_aspect = 1;
        m_camera.m_projection = renderer::ProjectionMode::Orthographic;
        m_camera.m_orthoHeight = 4;
        m_camera.m_near = 0.1f;
        m_camera.m_far = 20;
        Begin();
        for (uint32_t face = 0; face < 6; ++face) {
            m_bundle.renderer->SetRenderTargetFace(m_cube, face, 0, *m_resources);
            m_bundle.renderer->Clear({4, 4, 4, 1});
            m_bundle.renderer->SetRenderTargetFace(m_prefilterCube, face, 0, *m_resources);
            m_bundle.renderer->Clear({0, 0, 0, 1});
        }
        End();
        core::Logger::AddSink(this);
    }

    void TearDown() override
    {
        if (m_frameOpen) End();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        core::Logger::RemoveSink(this);
        m_source.reset();
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        testkit::Fixture::TearDown();
    }

    void Begin()
    {
        m_resources->AdvanceFrame();
        m_bundle.renderer->BeginFrame();
        m_frameOpen = true;
    }
    void End()
    {
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame();
        m_frameOpen = false;
    }
    renderer::ResourceHandle<renderer::TextureTag> ShVolume(uint8_t red = 128, uint8_t green = 64, uint8_t blue = 192)
    {
        std::array<uint8_t, 28> coefficients{};
        coefficients[3] = red;
        coefficients[7] = green;
        coefficients[11] = blue;
        return m_resources->CreateTexture3D(coefficients.data(), 1, 1, 7);
    }
    void ActiveVolume(uint32_t slot, renderer::ResourceHandle<renderer::TextureTag> texture)
    {
        m_handles.lightProbeSH[slot] = texture;
        auto& volume = m_data.probeVolumes[slot];
        volume.intensity = 1;
        volume.boxMin = {-1, -1, -1};
        volume.invSize = {0.5f, 0.5f, 0.5f};
        volume.grid[0] = volume.grid[1] = volume.grid[2] = 1;
    }
    Pixels Run(bool enabled = true, float x = 0, float metallic = 0, float ao = 1,
        float emission = 0, float environment = 0, bool medium = false, bool knownEnvironment = true)
    {
        renderer::RayReflectionConstants constants;
        constants.sceneLighting = 1;
        constants.diffuseIndirectEnabled = enabled ? 1u : 0u;
        constants.environmentMode = knownEnvironment ? 1u : 0u;
        constants.environmentRadiance[0] = constants.environmentRadiance[1] = constants.environmentRadiance[2] = environment;
        const std::array<math::Vector4, 2> point{{{x, 0, 0, metallic}, {0.6f, ao, emission, medium ? 1.0f : 0.0f}}};
        Begin();
        m_resources->Update(m_constants, &constants, sizeof(constants));
        m_resources->Update(m_point, point.data(), sizeof(point));
        m_resources->Update(m_handles.advancedGraphicsCB, &m_data, sizeof(m_data));
        renderer::ComputeCall call;
        call.shader = m_shader;
        call.constantBuffers[0] = m_constants;
        call.constantBuffers[1] = m_point;
        call.constantBuffers[8] = m_handles.advancedGraphicsCB;
        call.srvInputs[16] = m_handles.iblIrradiance;
        call.srvInputs[17] = m_handles.iblPrefilter;
        call.srvInputs[18] = m_handles.iblBrdfLut;
        call.srvInputs[11] = m_handles.lightProbeSH[0];
        call.srvInputs[12] = m_handles.lightProbeSH[1];
        call.uavOutputs[0] = m_output;
        EXPECT_TRUE(m_bundle.renderer->TryDispatch(call, *m_resources));
        m_bundle.renderer->SetRenderTarget(m_target, *m_resources);
        renderer::DrawCall copy;
        copy.shader = m_copy; copy.pipelineState = m_pipeline; copy.vertexCount = 3;
        copy.textures[5] = m_output;
        m_bundle.renderer->Submit(copy, *m_resources);
        End();
        std::vector<float> rgba;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_target, *m_resources, rgba, width, height));
        EXPECT_EQ(width, 4u); EXPECT_EQ(height, 1u); EXPECT_EQ(rgba.size(), 16u);
        Pixels result{};
        if (rgba.size() == 16)
            for (uint32_t i = 0; i < 4; ++i) result[i] = {rgba[4*i], rgba[4*i+1], rgba[4*i+2], rgba[4*i+3]};
        return result;
    }
    void Expect(const math::Vector4& value, const math::Vector4& expected) const
    {
        /// @note The final RGBA16F target rounds an otherwise FP32 linear HDR response.
        EXPECT_NEAR(value.x, expected.x, 0.008f);
        EXPECT_NEAR(value.y, expected.y, 0.008f);
        EXPECT_NEAR(value.z, expected.z, 0.008f);
        EXPECT_FLOAT_EQ(value.w, expected.w);
    }
    void View(renderer::RenderViewResources& view) const
    {
        view.advancedGraphicsSnapshotValid = true;
        view.advancedGraphicsSnapshot = m_data;
        view.rayReflection.diffuseIndirectEnabled = true;
        view.rayReflection.constantEnvironmentKnown = true;
        view.rayReflection.scene.sceneGeneration = 17;
        view.rayReflection.pathScene.contentRevision = 1;
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::RenderPassHandles m_handles;
    renderer::AdvancedGraphicsCB m_data{};
    renderer::Camera m_camera;
    renderer::RenderSettings m_settings;
    renderer::ResourceHandle<renderer::ShaderTag> m_shader, m_copy, m_reconstruction;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_constants, m_point;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_cube, m_prefilterCube, m_target;
    renderer::ResourceHandle<renderer::TextureTag> m_output;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    std::unique_ptr<testkit::TempDir> m_source;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
private:
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]")) ADD_FAILURE() << entry.message;
    }
};

TEST_F(RayHybridDiffuseGITest, PrimaryAndSecondaryShareDiffuseAndApplyMaterialAoOnce)
{
    const auto values = Run(true, 0, 0, 0.5f, 2);
    const math::Vector4 expected{1.945f, 1.945f, 1.945f, 1};
    Expect(values[0], expected);
    Expect(values[1], expected);
    Expect(values[2], {3.945f, 3.945f, 3.945f, 1});
    Expect(values[3], {0, 0, 0, 1});
}

TEST_F(RayHybridDiffuseGITest, HitPointUsesSpatialShRatherThanCameraSelectedConstant)
{
    ActiveVolume(0, ShVolume());
    ASSERT_TRUE(m_handles.lightProbeSH[0]);
    const auto inside = Run();
    const math::Vector4 expected{0.96f * 128 / 255 + 0.025f,
        0.96f * 64 / 255 + 0.025f, 0.96f * 192 / 255 + 0.025f, 1};
    Expect(inside[0], expected); Expect(inside[1], expected); Expect(inside[2], expected);
    const auto outside = Run(true, 3);
    Expect(outside[0], {3.865f, 3.865f, 3.865f, 1});
    Expect(outside[1], outside[0]);
}

TEST_F(RayHybridDiffuseGITest, InnerProbeOverridesOuterAndMetalHasNoDiffuseLobe)
{
    ActiveVolume(1, ShVolume(255, 255, 255));
    ActiveVolume(0, ShVolume(0, 0, 0));
    const auto inside = Run();
    Expect(inside[1], {0.025f, 0.025f, 0.025f, 1});
    m_data.probeVolumes[0].intensity = 0;
    const auto outer = Run();
    Expect(outer[1], {0.985f, 0.985f, 0.985f, 1});
    const auto metal = Run(true, 0, 1);
    /// @note The existing Raster artistic floor remains, but a full metal never receives irradiance diffuse.
    Expect(metal[0], {0.025f, 0.025f, 0.025f, 1});
    Expect(metal[1], metal[0]);
}

TEST_F(RayHybridDiffuseGITest, KnownEnvironmentDiffuseIsReplacedNotAddedTwice)
{
    const auto shared = Run(true, 0, 0, 1, 2, 3);
    const auto legacy = Run(false, 0, 0, 1, 2, 3);
    ASSERT_GT(shared[3].x, 0);
    ASSERT_GT(legacy[3].x, shared[3].x);
    Expect(shared[2], {shared[1].x + shared[3].x + 2, shared[1].y + shared[3].y + 2,
        shared[1].z + shared[3].z + 2, 1});
    Expect(legacy[2], {legacy[3].x + 2, legacy[3].y + 2, legacy[3].z + 2, 1});
}

TEST_F(RayHybridDiffuseGITest, MediumDoesNotInventSpatialGiOrUnknownAmbientOpticalDistance)
{
    const auto black = Run(true, 0, 0, 1, 2, 0, true);
    Expect(black[1], {0, 0, 0, 1});
    Expect(black[2], {2, 2, 2, 1});
    const auto unknown = Run(true, 0, 0, 1, 2, 0, true, false);
    Expect(unknown[2], {0, 0, 0, 0});
}

TEST_F(RayHybridDiffuseGITest, ReadinessRejectsMissingStaleWrongSizeAndNonfiniteActiveVolumes)
{
    renderer::RenderViewResources view;
    View(view);
    renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    EXPECT_TRUE(renderer::IsRayDiffuseIndirectReady(context, view));
    ActiveVolume(0, {});
    View(view);
    EXPECT_FALSE(renderer::IsRayDiffuseIndirectReady(context, view));
    ActiveVolume(0, ShVolume());
    const auto* immutableVolume = m_resources->Get(m_handles.lightProbeSH[0]);
    ASSERT_NE(immutableVolume, nullptr);
    EXPECT_EQ(immutableVolume->GetWidth(), 1u);
    EXPECT_EQ(immutableVolume->GetHeight(), 1u);
    EXPECT_EQ(immutableVolume->GetDepth(), 7u);
    EXPECT_EQ(immutableVolume->GetContentVersion(), 1u);
    View(view);
    EXPECT_TRUE(renderer::IsRayDiffuseIndirectReady(context, view));
    view.advancedGraphicsSnapshot.probeVolumes[0].grid[2] = 2;
    EXPECT_FALSE(renderer::IsRayDiffuseIndirectReady(context, view));
    View(view);
    view.advancedGraphicsSnapshot.probeVolumes[0].normalBias = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(renderer::IsRayDiffuseIndirectReady(context, view));
    View(view);
    m_resources->Release(m_handles.lightProbeSH[0]);
    EXPECT_FALSE(renderer::IsRayDiffuseIndirectReady(context, view));
    view.advancedGraphicsSnapshot.probeVolumes[0].intensity = 0;
    EXPECT_TRUE(renderer::IsRayDiffuseIndirectReady(context, view));
}

TEST_F(RayHybridDiffuseGITest, PublishedIrradianceHistoryRequiresExactOwnerEpochHandleAndConsumedValues)
{
    renderer::RenderViewResources view;
    renderer::RenderSharedResources shared;
    View(view);
    shared.rayReflectionReconstructionShader = m_reconstruction;
    renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = 2; context.frameStamp = 1;
    auto& state = view.rayReflection.reconstruction;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
    /// @note Arrange the same explicit immutable publication that a successful fresh bake supplies; this cube is not written again.
    context.iblIrradiancePublication = {m_handles.iblIrradiance, m_resources.get(), m_resources->GetResetVersion()};
    const auto next = [&]() { state.historyValid = true; state.lastFrameStamp = context.frameStamp++; };
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 1u);
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 0u);
    view.advancedGraphicsSnapshot.iblDiffuseScale = 2;
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    renderer::ResourceManager other(*m_bundle.renderer);
    context.iblIrradiancePublication.owner = &other;
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
    context.iblIrradiancePublication.owner = m_resources.get();
    ++context.iblIrradiancePublication.resourceEpoch;
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
    context.iblIrradiancePublication.resourceEpoch = m_resources->GetResetVersion();
    ++context.iblIrradiancePublication.texture.gen;
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
}

TEST_F(RayHybridDiffuseGITest, ShBindingsAndParametersInvalidateHistoryAndUnversionedWritesCannotReuse)
{
    renderer::RenderViewResources view;
    renderer::RenderSharedResources shared;
    ActiveVolume(0, ShVolume());
    View(view);
    shared.rayReflectionReconstructionShader = m_reconstruction;
    renderer::RenderPassContext context{{}, *m_bundle.renderer, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    context.experimentalRayTracingEnabled = true;
    context.width = context.height = 2; context.frameStamp = 1;
    context.iblIrradiancePublication = {m_handles.iblIrradiance, m_resources.get(), m_resources->GetResetVersion()};
    auto& state = view.rayReflection.reconstruction;
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    const auto next = [&]() { state.historyValid = true; state.lastFrameStamp = context.frameStamp++; };
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 0u);
    m_handles.lightProbeSH[0] = ShVolume(32, 32, 32);
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    view.advancedGraphicsSnapshot.probeVolumes[0].boxMin.x += 0.5f;
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
    m_handles.lightProbeSH[0] = m_resources->CreateComputeTexture3D(1, 1, 7);
    ASSERT_TRUE(m_handles.lightProbeSH[0]);
    next();
    ASSERT_TRUE(renderer::PrepareRayReflectionReconstruction(context, view, shared));
    EXPECT_EQ(state.constantsData.temporalAllowed, 0u);
    EXPECT_EQ(state.constantsData.resetHistory, 1u);
}

} /// @note anonymous namespace
} /// @note namespace fbzz::tests
