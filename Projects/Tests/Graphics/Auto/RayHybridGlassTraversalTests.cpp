/// @file    RayHybridGlassTraversalTests.cpp
/// @brief   Production glass tail-continuation equivalence against the frozen GPU stack traversal.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayGeometryCache.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

namespace fbzz::tests {
namespace {

struct TraversalConstants {
    uint32_t scenario = 0, initialSeed = 12345, glassBoundaryLimit = 16, instanceCount = 3;
    uint32_t environmentMode = 1, glassEnabled = 1, initialDepth = 0, repeatCount = 4;
    math::Vector3 constantEnvironmentRadiance{0.25f, 0.5f, 1};
    float envRotation = 0;
    float envIntensity = 1;
    uint32_t terminalDraws = 3;
    std::array<uint32_t, 2> reserved{};
};
static_assert(sizeof(TraversalConstants) == 64);
using Report = std::array<math::Vector4, 5>;

/// @note The geometry and lighting adapters isolate traversal; existing RayReflectionTest cases retain real TLAS, material and shadow coverage.
class RayHybridGlassTraversalTest : public testkit::Fixture, private core::ILogSink {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Glass traversal equivalence", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 32, 32);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        m_source = std::make_unique<testkit::TempDir>("glass_traversal");
        ASSERT_TRUE(m_source->IsValid());
        const std::filesystem::path root(FBZZ_GRAPHICS_SHADER_ROOT);
        const auto staged = m_source->File("Shaders");
        std::error_code error;
        /// @note Compile the current production include tree in temporary storage; authored assets and their compiled shader caches stay untouched.
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
        ASSERT_FALSE(error) << error.message();
        const auto testRoot = (root / "../../Projects/Tests/Graphics/Shaders").lexically_normal();
        for (const char* name : {"RayHybridGlassTraversal.cs.hlsl", "RayHybridGlassTraversalLegacy.hlsli"}) {
            std::filesystem::copy_file(testRoot / name, staged / "RayTracing" / name,
                std::filesystem::copy_options::overwrite_existing, error);
            ASSERT_FALSE(error) << error.message();
        }
        m_shader = m_resources->LoadShader((staged / "RayTracing/RayHybridGlassTraversal.cs.hlsl").generic_string());
        m_copy = m_resources->LoadShader((root / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_constants = m_resources->CreateConstantBuffer(sizeof(TraversalConstants));
        m_events = m_resources->CreateRWStructuredBuffer(nullptr, 16448, 4 * sizeof(uint32_t));
        m_output = m_resources->CreateComputeTexture(5, 1);
        m_target = m_resources->CreateRenderTarget(5, 1, {1, renderer::Format::RGBA16F, false});
        m_pipeline = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(m_shader && m_copy && m_constants && m_events && m_output && m_target && m_pipeline);
        core::Logger::AddSink(this);
    }
    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        core::Logger::RemoveSink(this);
        m_source.reset();
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        EXPECT_EQ(m_validationFailures, 0u);
        testkit::Fixture::TearDown();
    }
    Report Run(TraversalConstants constants, float roughness = 0, float ior = 1.5f)
    {
        std::vector<renderer::RayHitRecord> records(constants.instanceCount);
        std::vector<renderer::RaySurfaceRecord> surfaces(constants.instanceCount);
        for (uint32_t i = 0; i < constants.instanceCount; ++i) {
            records[i].objectIndex = i + 10;
            records[i].objectGeneration = 2;
            records[i].sceneGenerationLow = 7;
            records[i].sceneGenerationHigh = 3;
            auto& surface = surfaces[i];
            surface.baseColor = {1, 1, 1, 1};
            surface.transmission = 1; surface.ior = ior;
            surface.roughness = roughness; surface.metallic = 0; surface.supported = 2;
            surface.attenuationColor = {0.8f, 0.9f, 1}; surface.attenuationDistance = 2;
            if (constants.scenario == 2 || constants.scenario == 6 || constants.scenario == 7) {
                surface.dielectricFlags = 1;
                surface.roughness = 0;
                surface.attenuationColor = {1, 1, 1};
            }
        }
        if (constants.scenario != 4 && constants.scenario != 7) {
            auto& terminal = surfaces.back();
            terminal.supported = 1; terminal.transmission = 0;
            terminal.emission = {2, 4, 6};
        }
        if (constants.scenario == 1 && ior != 1) surfaces[1].ior = 1.2f;
        m_resources->Release(m_records);
        m_resources->Release(m_surfaces);
        m_records = m_resources->CreateStructuredBuffer(records.data(), constants.instanceCount, sizeof(renderer::RayHitRecord));
        m_surfaces = m_resources->CreateStructuredBuffer(surfaces.data(), constants.instanceCount, sizeof(renderer::RaySurfaceRecord));
        EXPECT_TRUE(m_records && m_surfaces);
        m_resources->AdvanceFrame();
        auto& device = *m_bundle.renderer;
        device.BeginFrame(); m_frameOpen = true;
        m_resources->Update(m_constants, &constants, sizeof(constants));
        renderer::ComputeCall call;
        call.shader = m_shader; call.constantBuffers[0] = m_constants;
        call.srvBuffers[1] = m_records; call.srvBuffers[2] = m_surfaces;
        call.uavBuffers[0] = m_events; call.uavOutputs[0] = m_output;
        EXPECT_TRUE(device.TryDispatch(call, *m_resources));
        device.SetRenderTarget(m_target, *m_resources);
        renderer::DrawCall copy;
        copy.shader = m_copy; copy.pipelineState = m_pipeline; copy.vertexCount = 3;
        copy.textures[5] = m_output;
        device.Submit(copy, *m_resources);
        device.SetRenderTarget({}, *m_resources);
        device.EndFrame(); m_frameOpen = false;
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_target, *m_resources, pixels, width, height));
        EXPECT_EQ(width, 5u); EXPECT_EQ(height, 1u); EXPECT_EQ(pixels.size(), 20u);
        Report report{};
        if (pixels.size() == 20)
            for (size_t pixel = 0; pixel < report.size(); ++pixel)
                report[pixel] = {pixels[4 * pixel], pixels[4 * pixel + 1], pixels[4 * pixel + 2], pixels[4 * pixel + 3]};
        return report;
    }
    void ExpectEquivalent(const Report& report, bool success) const
    {
        /// @note Comparisons happen on FP32/uint bits in the CS; RGBA16F stores only exact Boolean results and bounded event counts.
        for (size_t pixel = 0; pixel < 2; ++pixel) {
            EXPECT_FLOAT_EQ(report[pixel].x, 1); EXPECT_FLOAT_EQ(report[pixel].y, 1);
            EXPECT_FLOAT_EQ(report[pixel].z, 1); EXPECT_FLOAT_EQ(report[pixel].w, 1);
        }
        EXPECT_FLOAT_EQ(report[3].w, success ? 1.0f : 0.0f);
        EXPECT_FLOAT_EQ(report[4].w, report[3].w);
        EXPECT_FLOAT_EQ(report[2].x, report[2].y);
        EXPECT_FLOAT_EQ(report[2].z, report[2].w);
    }
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.starts_with("  [WARNING]") || entry.message.starts_with("  [ERROR]")
            || entry.message.starts_with("  [CORRUPTION]"))
            ++m_validationFailures;
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    std::unique_ptr<testkit::TempDir> m_source;
    renderer::ResourceHandle<renderer::ShaderTag> m_shader, m_copy;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_constants;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_records, m_surfaces, m_events;
    renderer::ResourceHandle<renderer::TextureTag> m_output;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
    uint32_t m_validationFailures = 0;
};

TEST_F(RayHybridGlassTraversalTest, SmoothSlabPreservesFourSampleRngAndExactTerminalVisitOrder)
{
    for (uint32_t seed : {1u, 17u, 12345u, 0xFFFF1234u}) {
        SCOPED_TRACE(seed);
        TraversalConstants constants; constants.initialSeed = seed;
        const auto report = Run(constants);
        ExpectEquivalent(report, true);
        EXPECT_GT(report[2].x, 8);
        EXPECT_GT(report[2].z, 0);
        EXPECT_GT(report[4].x, 0);
    }
}
TEST_F(RayHybridGlassTraversalTest, RoughSingleBranchesPreserveSamplingNullEventsAndSeedAdvancement)
{
    for (float roughness : {0.2f, 1.0f})
        for (uint32_t seed : {1u, 2u, 3u, 17u, 12345u, 0xFFFF1234u}) {
            SCOPED_TRACE(roughness);
            SCOPED_TRACE(seed);
            TraversalConstants constants; constants.initialSeed = seed;
            ExpectEquivalent(Run(constants, roughness), true);
        }
}
TEST_F(RayHybridGlassTraversalTest, NestedSolidsRetainLifoBeerAndBoundaryBudget)
{
    TraversalConstants constants; constants.scenario = 1; constants.instanceCount = 3;
    const auto matched = Run(constants, 0, 1);
    ExpectEquivalent(matched, true);
    EXPECT_GT(matched[4].x, 0);
    constants.glassBoundaryLimit = 8;
    ExpectEquivalent(Run(constants), true);
}
TEST_F(RayHybridGlassTraversalTest, ThinSplitBranchesRetainBothMotionGuidesAndDirectTransmission)
{
    TraversalConstants constants; constants.scenario = 2;
    const auto report = Run(constants);
    ExpectEquivalent(report, true);
    EXPECT_GT(report[2].z, 0);
    EXPECT_GT(report[4].x, 0);
}
TEST_F(RayHybridGlassTraversalTest, TotalInternalReflectionContinuesWithoutChangingTheWorkDenominator)
{
    for (uint32_t limit : {1u, 5u, 16u}) {
        SCOPED_TRACE(limit);
        TraversalConstants constants; constants.scenario = 3; constants.initialDepth = 1;
        constants.glassBoundaryLimit = limit; constants.repeatCount = 1;
        const auto report = Run(constants);
        ExpectEquivalent(report, true);
        EXPECT_GE(report[2].x, static_cast<float>(limit));
        EXPECT_FLOAT_EQ(report[4].x, 0); EXPECT_FLOAT_EQ(report[4].y, 0); EXPECT_FLOAT_EQ(report[4].z, 0);
    }
}
TEST_F(RayHybridGlassTraversalTest, ZeroAndFiniteBoundaryLimitsPreserveBlackResidualAndFailure)
{
    for (uint32_t limit : {0u, 1u, 2u, 16u}) {
        SCOPED_TRACE(limit);
        TraversalConstants constants; constants.glassBoundaryLimit = limit; constants.repeatCount = 1;
        const auto report = Run(constants);
        ExpectEquivalent(report, limit != 0);
        if (limit == 0) EXPECT_FLOAT_EQ(report[2].x, 0);
    }
    TraversalConstants nullInterface; nullInterface.scenario = 8;
    const auto nullReport = Run(nullInterface);
    ExpectEquivalent(nullReport, true);
    EXPECT_FLOAT_EQ(nullReport[2].x, 0); EXPECT_FLOAT_EQ(nullReport[4].x, 0);
}
TEST_F(RayHybridGlassTraversalTest, NinthMediumMismatchedOwnerAndUnknownEnvironmentRemainFailClosed)
{
    TraversalConstants ninth; ninth.scenario = 4; ninth.instanceCount = 9; ninth.initialDepth = 8;
    ExpectEquivalent(Run(ninth), false);
    TraversalConstants mismatch; mismatch.scenario = 5;
    ExpectEquivalent(Run(mismatch), false);
    TraversalConstants unknown; unknown.scenario = 6; unknown.environmentMode = 0;
    ExpectEquivalent(Run(unknown), false);
}
TEST_F(RayHybridGlassTraversalTest, FiveHundredTwelveWorkCapRejectsTheSameIncompleteSplitTree)
{
    TraversalConstants constants; constants.scenario = 7; constants.instanceCount = 4;
    constants.repeatCount = 1;
    const auto report = Run(constants);
    ExpectEquivalent(report, false);
    /// @note The first hit is supplied, so 511 subsequent queries prove that the same 512 states were visited before fail-closed truncation.
    EXPECT_FLOAT_EQ(report[2].x, 511);
}

} /// @note namespace
} /// @note namespace fbzz::tests
