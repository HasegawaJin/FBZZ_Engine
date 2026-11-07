/// @file    GpuGeometrySelectionTests.cpp
/// @brief   GPU geometry selection, indexed-indirect output, ResourceManager lifetime, and mesh equivalence tests.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>

#include <Graphics/Pipeline/GpuHiZ.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/GpuGeometrySelection.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::tests {
namespace {

bool g_rejectGeometrySelectionShader = false;

std::filesystem::path ResolveGpuGeometryShader(const std::filesystem::path& requested)
{
    const std::string relative = requested.generic_string();
    if (g_rejectGeometrySelectionShader
        && relative.ends_with("Assets/Shaders/Pipeline/GpuGeometrySelection.cs.hlsl"))
        return {};
    constexpr std::string_view prefix = "Assets/Shaders/";
    if (relative.starts_with(prefix))
        return std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT) / relative.substr(prefix.size());
    return requested;
}

struct ProbePosition {
    float x;
    float y;
    float z;
};

struct ProbeInstance {
    std::array<float, 2> offset;
    std::array<float, 2> padding{};
    std::array<float, 4> color;
};

static_assert(sizeof(ProbePosition) == 12);
static_assert(sizeof(ProbeInstance) == 32);

class GpuGeometrySelectionTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"GPU geometry selection", WS_POPUP,
            0, 0, kExtent, kExtent, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, kExtent, kExtent);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        renderer::SetShaderPathResolver(ResolveGpuGeometryShader);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);

        auto& resources = *m_resources;
        m_target = resources.CreateRenderTarget(kExtent, kExtent);
        m_pipelineState = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        m_vertexShader = resources.LoadShader(FBZZ_GPU_GEOMETRY_SELECTION_VS_SHADER);
        m_meshShader = m_bundle.renderer->SupportsMeshShaders()
            ? resources.LoadShader(FBZZ_GPU_GEOMETRY_SELECTION_MS_SHADER)
            : renderer::ResourceHandle<renderer::ShaderTag>{};

        const std::array<ProbePosition, 6> positions{{
            {-0.42f, -0.40f, 0.5f}, {0.0f, 0.42f, 0.5f}, {0.42f, -0.40f, 0.5f},
            {-0.16f, -0.16f, 0.5f}, {0.0f, 0.16f, 0.5f}, {0.16f, -0.16f, 0.5f}
        }};
        const std::array<uint32_t, 6> indices{{0, 1, 2, 0, 1, 2}};
        const std::array<ProbeInstance, 3> instances{{
            {{-0.45f, 0.0f}, {}, {1.0f, 0.0f, 0.0f, 1.0f}},
            {{0.45f, 0.0f}, {}, {0.0f, 1.0f, 0.0f, 1.0f}},
            {{-0.45f, 0.0f}, {}, {0.0f, 0.0f, 1.0f, 1.0f}}
        }};
        m_vertexBuffer = resources.CreateVertexBuffer(positions.data(), sizeof(positions), sizeof(ProbePosition));
        m_indexBuffer = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        m_instanceBuffer = resources.CreateStructuredBuffer(instances.data(),
            static_cast<uint32_t>(instances.size()), sizeof(ProbeInstance));

        ASSERT_TRUE(m_target.IsValid() && m_pipelineState.IsValid() && m_vertexShader.IsValid()
            && m_vertexBuffer.IsValid() && m_indexBuffer.IsValid() && m_instanceBuffer.IsValid());
        if (m_bundle.renderer->SupportsMeshShaders()) ASSERT_TRUE(m_meshShader.IsValid());
        InitializeViewAndCandidates();
    }

    void TearDown() override
    {
        m_resources.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.imguiRenderer.reset();
        m_bundle.renderer.reset();
        renderer::SetShaderPathResolver(nullptr);
        g_rejectGeometrySelectionShader = false;
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        testkit::Fixture::TearDown();
    }

    void InitializeViewAndCandidates()
    {
        m_view.frustumPlanes = {{
            {{1.0f, 0.0f, 0.0f, 1.0f}}, {{-1.0f, 0.0f, 0.0f, 1.0f}},
            {{0.0f, 1.0f, 0.0f, 1.0f}}, {{0.0f, -1.0f, 0.0f, 1.0f}},
            {{0.0f, 0.0f, 1.0f, 1.0f}}, {{0.0f, 0.0f, -1.0f, 1.0f}}
        }};
        m_view.cameraAndProjectionScale = {{0.0f, 0.0f, -2.0f, 100.0f}};
        m_view.projectionMetrics = {{0.0f, 1.0f, 1.0f, 0.0f}};

        const std::array<renderer::GpuGeometryLodRange, 2> lods{{
            {3, 0, 0, 3, 0.001f, {}},
            {3, 3, 3, 3, 0.01f, {}}
        }};
        for (uint32_t index = 0; index < m_candidates.size(); ++index) {
            renderer::GpuGeometryCandidate& candidate = m_candidates[index];
            candidate.lodCount = static_cast<uint32_t>(lods.size());
            candidate.lods[0] = lods[0];
            candidate.lods[1] = lods[1];
            candidate.boundsMin = index == 2
                ? std::array<float, 4>{{2.0f, -0.3f, 0.4f, 0.0f}}
                : std::array<float, 4>{{index == 0 ? -0.75f : 0.15f, -0.3f, 0.4f, 0.0f}};
            candidate.boundsMax = index == 2
                ? std::array<float, 4>{{3.0f, 0.3f, 0.6f, 0.0f}}
                : std::array<float, 4>{{index == 0 ? -0.15f : 0.75f, 0.3f, 0.6f, 0.0f}};
        }
    }

    bool Capture(std::vector<float>& pixels)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        return m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_target, *m_resources,
            pixels, width, height) && width == kExtent && height == kExtent;
    }

    static constexpr uint32_t kExtent = 64;
    HRESULT m_comResult = E_FAIL;
    HWND m_window = nullptr;
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::GpuGeometrySelection m_selection;
    renderer::GpuGeometrySelectionView m_view{};
    std::array<renderer::GpuGeometryCandidate, 3> m_candidates{};
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target{};
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipelineState{};
    renderer::ResourceHandle<renderer::ShaderTag> m_vertexShader{};
    renderer::ResourceHandle<renderer::ShaderTag> m_meshShader{};
    renderer::ResourceHandle<renderer::BufferTag> m_vertexBuffer{};
    renderer::ResourceHandle<renderer::BufferTag> m_indexBuffer{};
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_instanceBuffer{};
};

TEST_F(GpuGeometrySelectionTest, DispatchesOrderedIndexedArgumentsAndCullsOutsideCandidates)
{
    auto& device = *m_bundle.renderer;
    auto& resources = *m_resources;
    if (!device.SupportsIndirectDraw()) GTEST_SKIP() << "Indexed indirect drawing is unavailable";

    renderer::GpuGeometryDispatch untouched;
    untouched.candidateCount = 0x1234u;
    untouched.indirectArgumentOffset = 0x5678u;
    auto malformed = m_candidates[0];
    malformed.lods[1].startIndex = std::numeric_limits<uint32_t>::max();
    EXPECT_FALSE(m_selection.Prepare(device, resources, m_view,
        std::span<const renderer::GpuGeometryCandidate>(&malformed, 1), 6, 6, untouched));
    EXPECT_EQ(untouched.candidateCount, 0x1234u);
    EXPECT_EQ(untouched.indirectArgumentOffset, 0x5678u);

    /// @note DX12 rejects dispatches outside an open frame; failure must leave the caller's fallback output intact.
    EXPECT_FALSE(m_selection.Prepare(device, resources, m_view, m_candidates, 6, 6, untouched));
    EXPECT_EQ(untouched.candidateCount, 0x1234u);
    EXPECT_EQ(untouched.indirectArgumentOffset, 0x5678u);

    resources.AdvanceFrame();
    device.BeginFrame();
    renderer::GpuGeometryDispatch dispatch;
    ASSERT_TRUE(m_selection.Prepare(device, resources, m_view, m_candidates, 6, 6, dispatch));
    ASSERT_TRUE(dispatch.decisions.IsValid() && dispatch.indirectArguments.IsValid());
    EXPECT_EQ(dispatch.candidateCount, 3u);
    EXPECT_EQ(dispatch.indirectArgumentOffset, 0u);

    renderer::GpuGeometrySelectionView secondView = m_view;
    renderer::GpuGeometryDispatch secondDispatch;
    ASSERT_TRUE(m_selection.Prepare(device, resources, secondView, m_candidates, 6, 6, secondDispatch));
    EXPECT_NE(dispatch.indirectArguments, secondDispatch.indirectArguments);
    EXPECT_NE(dispatch.decisions, secondDispatch.decisions);

    device.SetRenderTarget(m_target, resources);
    device.Clear({0.0f, 0.0f, 0.0f, 1.0f});
    renderer::DrawCall draw;
    draw.vertexBuffer = m_vertexBuffer;
    draw.indexBuffer = m_indexBuffer;
    draw.shader = m_vertexShader;
    draw.pipelineState = m_pipelineState;
    draw.instanceBuffer = m_instanceBuffer;
    draw.indexCount = 6;
    draw.indirectArguments = dispatch.indirectArguments;
    draw.indirectCommandCount = dispatch.candidateCount;
    draw.indirectInstanceOffset = true;
    device.Submit(draw, resources);
    device.SetRenderTarget({}, resources);
    device.EndFrame();

    std::vector<float> pixels;
    ASSERT_TRUE(Capture(pixels));
    uint32_t redPixels = 0;
    uint32_t greenPixels = 0;
    uint32_t bluePixels = 0;
    for (size_t offset = 0; offset < pixels.size(); offset += 4) {
        const float red = pixels[offset];
        const float green = pixels[offset + 1];
        const float blue = pixels[offset + 2];
        if (red > 0.9f && green < 0.1f && blue < 0.1f) ++redPixels;
        if (green > 0.9f && red < 0.1f && blue < 0.1f) ++greenPixels;
        if (blue > 0.9f && red < 0.1f && green < 0.1f) ++bluePixels;
    }
    EXPECT_GT(redPixels, 0u);
    EXPECT_GT(greenPixels, 0u);
    EXPECT_EQ(bluePixels, 0u);
}

TEST_F(GpuGeometrySelectionTest, GpuHiZReducesActualAtlasDepthAndPreservesIndependentViewLoans)
{
    auto& device = *m_bundle.renderer;
    auto& resources = *m_resources;
    if (!device.SupportsIndirectDraw()) GTEST_SKIP() << "Indexed indirect drawing is unavailable";

    struct ProbeConstants {
        float depth;
        uint32_t index;
        uint32_t readPyramid;
        uint32_t padding;
    };
    const auto shader = resources.LoadShader(FBZZ_GPU_HI_Z_PROBE_SHADER);
    const auto constants = resources.CreateConstantBuffer(sizeof(ProbeConstants));
    const auto depthPipeline = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
    ASSERT_TRUE(resources.Get(shader) && constants.IsValid() && depthPipeline.IsValid());

    renderer::DrawCall draw;
    draw.shader = shader;
    draw.constantBuffers[0] = constants;
    draw.vertexCount = 3;
    const auto writeDepth = [&](float depth) {
        const ProbeConstants values{depth, 0, 0, 0};
        resources.Update(constants, &values, sizeof(values));
        draw.pipelineState = depthPipeline;
        draw.vsBuffers[0] = {};
        device.Submit(draw, resources);
    };
    const auto probe = [&](const renderer::GpuHiZ& hierarchy, uint32_t index, uint32_t x) {
        const ProbeConstants values{0.5f, index, 1, 0};
        resources.Update(constants, &values, sizeof(values));
        device.SetViewport(x, 0, 1, 1);
        draw.pipelineState = m_pipelineState;
        draw.vsBuffers[0] = hierarchy.depths;
        device.Submit(draw, resources);
    };

    resources.AdvanceFrame();
    device.BeginFrame();
    device.SetRenderTarget(m_target, resources);
    device.Clear({0.0f, 0.0f, 0.0f, 0.0f});
    uint32_t resultOffset = 0;
    for (const bool reversedZ : {false, true}) {
        renderer::RenderTargetDesc desc;
        desc.reversedZ = reversedZ;
        const auto source = resources.CreateRenderTarget(43, 13, desc);
        ASSERT_TRUE(source.IsValid());
        device.SetRenderTarget(source, resources);
        device.ClearDepth();
        device.SetViewport(3, 2, 36, 9);
        writeDepth(0.5f);
        device.SetViewport(3, 2, 2, 2);
        writeDepth(reversedZ ? 0.75f : 0.25f);

        /// @note 37×9 は最初の段で 10×3。端の未描画列と端数の行を両方含む。
        device.SetViewport(3, 2, 37, 9);
        const auto first = renderer::BuildGpuHiZ(device, resources, source, 3, 2, 37, 9);
        ASSERT_TRUE(first.depths.IsValid());
        EXPECT_EQ(first.width, 37u);
        EXPECT_EQ(first.height, 9u);
        EXPECT_EQ(first.reversedZ, reversedZ);
        EXPECT_FALSE(renderer::BuildGpuHiZ(device, resources, source, 42, 0, 2, 1).depths.IsValid());

        /// @note ビルダーが復元した viewport と DSV へ描く。古い貸出のデータはそのまま残る。
        writeDepth(reversedZ ? 0.75f : 0.25f);
        const auto second = renderer::BuildGpuHiZ(device, resources, source, 3, 2, 37, 9);
        ASSERT_TRUE(second.depths.IsValid());
        EXPECT_NE(first.depths, second.depths);
        device.SetViewport(0, 0, 3, 2);
        const auto neighbor = renderer::BuildGpuHiZ(device, resources, source, 0, 0, 3, 2);
        ASSERT_TRUE(neighbor.depths.IsValid());

        device.SetRenderTarget(m_target, resources);
        probe(first, 0, resultOffset++);
        probe(first, 20, resultOffset++);
        probe(first, 43, resultOffset++);
        probe(first, 44, resultOffset++);
        probe(first, 45, resultOffset++);
        probe(second, 45, resultOffset++);
        probe(neighbor, 0, resultOffset++);
    }
    device.SetRenderTarget({}, resources);
    device.EndFrame();

    std::vector<float> pixels;
    ASSERT_TRUE(Capture(pixels));
    const std::array expected{0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 0.25f, 1.0f,
        0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.75f, 0.0f};
    for (size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_FLOAT_EQ(pixels[i * 4], expected[i]);
        EXPECT_FLOAT_EQ(pixels[i * 4 + 1], 1.0f);
    }
}

TEST_F(GpuGeometrySelectionTest, MeshShaderMatchesGpuSelectedIndexedLodAndInstanceRows)
{
    auto& device = *m_bundle.renderer;
    auto& resources = *m_resources;
    if (!device.SupportsIndirectDraw()) GTEST_SKIP() << "Indexed indirect drawing is unavailable";
    if (!device.SupportsMeshShaders()) GTEST_SKIP() << "Mesh shaders are unavailable";

    resources.AdvanceFrame();
    device.BeginFrame();
    renderer::GpuGeometryDispatch dispatch;
    ASSERT_TRUE(m_selection.Prepare(device, resources, m_view, m_candidates, 6, 6, dispatch));
    device.SetRenderTarget(m_target, resources);
    device.Clear({0.0f, 0.0f, 0.0f, 1.0f});
    renderer::DrawCall indexed;
    indexed.vertexBuffer = m_vertexBuffer;
    indexed.indexBuffer = m_indexBuffer;
    indexed.shader = m_vertexShader;
    indexed.pipelineState = m_pipelineState;
    indexed.instanceBuffer = m_instanceBuffer;
    indexed.indexCount = 6;
    indexed.indirectArguments = dispatch.indirectArguments;
    indexed.indirectCommandCount = dispatch.candidateCount;
    indexed.indirectInstanceOffset = true;
    device.Submit(indexed, resources);
    device.SetRenderTarget({}, resources);
    device.EndFrame();
    std::vector<float> indexedPixels;
    ASSERT_TRUE(Capture(indexedPixels));

    resources.AdvanceFrame();
    device.BeginFrame();
    device.SetRenderTarget(m_target, resources);
    device.Clear({0.0f, 0.0f, 0.0f, 1.0f});
    renderer::DrawCall mesh;
    mesh.shader = m_meshShader;
    mesh.pipelineState = m_pipelineState;
    mesh.instanceBuffer = m_instanceBuffer;
    mesh.meshGroupCountX = 1;
    mesh.meshGroupCountY = 2;
    mesh.meshGroupCountZ = 1;
    device.Submit(mesh, resources);
    device.SetRenderTarget({}, resources);
    device.EndFrame();
    std::vector<float> meshPixels;
    ASSERT_TRUE(Capture(meshPixels));
    ASSERT_EQ(indexedPixels.size(), meshPixels.size());
    for (size_t index = 0; index < indexedPixels.size(); ++index)
        EXPECT_NEAR(indexedPixels[index], meshPixels[index], 0.001f);
}

TEST_F(GpuGeometrySelectionTest, ResourceManagerSwitchAndResourceFailurePreserveFallbackOutput)
{
    auto& device = *m_bundle.renderer;
    auto& originalResources = *m_resources;
    if (!device.SupportsIndirectDraw()) GTEST_SKIP() << "Indexed indirect drawing is unavailable";

    originalResources.AdvanceFrame();
    device.BeginFrame();
    renderer::GpuGeometryDispatch originalDispatch;
    ASSERT_TRUE(m_selection.Prepare(device, originalResources, m_view, m_candidates, 6, 6, originalDispatch));
    device.SetRenderTarget({}, originalResources);
    device.EndFrame();

    renderer::ResourceManager alternateResources(device);
    renderer::GpuGeometryDispatch untouched;
    untouched.candidateCount = 0x4321u;
    untouched.indirectArgumentOffset = 0x8765u;
    g_rejectGeometrySelectionShader = true;
    EXPECT_FALSE(m_selection.Prepare(device, alternateResources, m_view, m_candidates, 6, 6, untouched));
    EXPECT_EQ(untouched.candidateCount, 0x4321u);
    EXPECT_EQ(untouched.indirectArgumentOffset, 0x8765u);
    g_rejectGeometrySelectionShader = false;

    alternateResources.AdvanceFrame();
    device.BeginFrame();
    renderer::GpuGeometryDispatch alternateDispatch;
    ASSERT_TRUE(m_selection.Prepare(device, alternateResources, m_view, m_candidates, 6, 6, alternateDispatch));
    ASSERT_TRUE(alternateDispatch.indirectArguments.IsValid());
    m_selection.Release(originalResources);
    renderer::GpuGeometryDispatch afterWrongOwnerRelease;
    ASSERT_TRUE(m_selection.Prepare(device, alternateResources, m_view, m_candidates, 6, 6,
        afterWrongOwnerRelease));
    EXPECT_TRUE(afterWrongOwnerRelease.indirectArguments.IsValid());
    device.SetRenderTarget({}, alternateResources);
    device.EndFrame();
    m_selection.Release(alternateResources);
}

} /// @note namespace
} /// @note namespace fbzz::tests
