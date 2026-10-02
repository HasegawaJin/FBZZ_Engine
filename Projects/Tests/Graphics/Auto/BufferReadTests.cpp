/// @file    BufferReadTests.cpp
/// @brief   Raw Buffer SRV の内容版・GPU snapshot と間接依存の検証を実デバイスで行う。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <filesystem>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

struct BufferReadInput {
    uint32_t vertexDescriptor = renderer::INVALID_BINDLESS_INDEX;
    uint32_t indexDescriptor = renderer::INVALID_BINDLESS_INDEX;
    uint32_t byteOffset = 0;
    uint32_t indirect = 0;
    uint32_t outputPixel = 0;
};
static_assert(sizeof(BufferReadInput) == 20);

class BufferReadTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_comResult));
        m_window = CreateWindowExW(0, L"STATIC", L"Raw buffer read test", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 64, 64);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        m_compute = m_resources->LoadShader((std::filesystem::path(FBZZ_RAY_INTERSECTION_SHADER)
            .parent_path() / "BufferRead.cs.hlsl").generic_string());
        m_copy = m_resources->LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
            / "PostProcess/Color/CopyColor.hlsl").generic_string());
        m_output = m_resources->CreateComputeTexture(3, 1);
        m_target = m_resources->CreateRenderTarget(3, 1,
            renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
        m_state = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
        ASSERT_TRUE(m_compute.IsValid() && m_copy.IsValid() && m_output.IsValid()
            && m_target.IsValid() && m_state.IsValid());
    }

    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_comResult)) CoUninitialize();
        testkit::Fixture::TearDown();
    }

    void BeginFrame()
    {
        m_resources->AdvanceFrame();
        m_bundle.renderer->BeginFrame();
        m_frameOpen = true;
    }

    void Read(renderer::ResourceHandle<renderer::BufferTag> vertices,
              renderer::ResourceHandle<renderer::BufferTag> indices,
              uint32_t outputPixel, bool indirect = false,
              renderer::ResourceHandle<renderer::BufferTag> extraDependency = {})
    {
        BufferReadInput input;
        input.vertexDescriptor = m_resources->Get(vertices)->GetBindlessSrvIndex();
        input.indexDescriptor = m_resources->Get(indices)->GetBindlessSrvIndex();
        ASSERT_NE(input.vertexDescriptor, renderer::INVALID_BINDLESS_INDEX);
        ASSERT_NE(input.indexDescriptor, renderer::INVALID_BINDLESS_INDEX);
        input.indirect = indirect ? 1u : 0u;
        input.outputPixel = outputPixel;
        const auto table = m_resources->CreateStructuredBuffer(&input, 1, sizeof(input));
        ASSERT_TRUE(table.IsValid());
        renderer::ComputeCall dispatch;
        dispatch.shader = m_compute;
        dispatch.srvBuffers[14] = table;
        dispatch.uavOutputs[0] = m_output;
        if (indirect)
            dispatch.indirectReadBuffers = {vertices, indices};
        else {
            dispatch.srvRawBuffers[0] = vertices;
            dispatch.srvRawBuffers[1] = indices;
        }
        if (extraDependency.IsValid()) dispatch.indirectReadBuffers.push_back(extraDependency);
        m_bundle.renderer->Dispatch(dispatch, *m_resources);
    }

    std::vector<math::Vector4> FinishAndReadback()
    {
        auto& device = *m_bundle.renderer;
        device.SetRenderTarget(m_target, *m_resources);
        renderer::DrawCall draw;
        draw.shader = m_copy;
        draw.pipelineState = m_state;
        draw.vertexCount = 3;
        draw.textures[5] = m_output;
        device.Submit(draw, *m_resources);
        device.SetRenderTarget({}, *m_resources);
        device.EndFrame();
        m_frameOpen = false;
        std::vector<float> rgba;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(device.CaptureRenderTargetToLinearRGBA(m_target, *m_resources, rgba, width, height));
        EXPECT_EQ(width, 3u);
        EXPECT_EQ(height, 1u);
        std::vector<math::Vector4> result;
        for (size_t index = 0; index + 3 < rgba.size(); index += 4)
            result.push_back({rgba[index], rgba[index + 1], rgba[index + 2], rgba[index + 3]});
        return result;
    }

    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ResourceHandle<renderer::ShaderTag> m_compute;
    renderer::ResourceHandle<renderer::ShaderTag> m_copy;
    renderer::ResourceHandle<renderer::TextureTag> m_output;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_state;
    HWND m_window = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_frameOpen = false;
};

TEST_F(BufferReadTest, CpuUpdatesPublishNewSnapshotsWithoutChangingRecordedReads)
{
    const math::Vector3 initial{1, 2, 3};
    const uint32_t initialIndex = 17;
    const auto vertices = m_resources->CreateVertexBuffer(&initial, sizeof(initial), sizeof(initial));
    const auto indices = m_resources->CreateIndexBuffer(&initialIndex, 1);
    ASSERT_TRUE(vertices.IsValid() && indices.IsValid());
    auto* vertexBuffer = m_resources->Get(vertices);
    ASSERT_NE(vertexBuffer, nullptr);
    EXPECT_EQ(vertexBuffer->GetContentVersion(), 1u);
    const uint32_t initialSrv = vertexBuffer->GetBindlessSrvIndex();
    ASSERT_NE(initialSrv, renderer::INVALID_BINDLESS_INDEX);
    EXPECT_EQ(vertexBuffer->GetBindlessSrvIndex(), initialSrv);
    const auto gpuWritable = m_resources->CreateGpuWritableVertexBuffer(sizeof(initial), sizeof(initial));
    ASSERT_TRUE(gpuWritable.IsValid());
    EXPECT_EQ(m_resources->Get(gpuWritable)->GetContentVersion(), 0u);
    math::Vector3 copied{-1, -1, -1};
    ASSERT_TRUE(vertexBuffer->CopyData(0, sizeof(copied), &copied));
    EXPECT_VEC3_NEAR(copied, initial, 0.0f);
    EXPECT_FALSE(vertexBuffer->CopyData(sizeof(initial), sizeof(copied), &copied));
    EXPECT_VEC3_NEAR(copied, initial, 0.0f);
    EXPECT_FALSE(m_resources->Get(gpuWritable)->CopyData(0, sizeof(copied), &copied));

    BeginFrame();
    Read(vertices, indices, 0);
    const math::Vector3 second{4, 5, 6};
    const uint32_t secondIndex = 23;
    m_resources->Update(vertices, &second, sizeof(second));
    ASSERT_TRUE(vertexBuffer->CopyData(0, sizeof(copied), &copied));
    EXPECT_VEC3_NEAR(copied, second, 0.0f);
    m_resources->Update(indices, &secondIndex, sizeof(secondIndex));
    EXPECT_EQ(vertexBuffer->GetContentVersion(), 2u);
    const uint32_t secondSrv = vertexBuffer->GetBindlessSrvIndex();
    ASSERT_NE(secondSrv, renderer::INVALID_BINDLESS_INDEX);
    EXPECT_NE(secondSrv, initialSrv);
    EXPECT_EQ(vertexBuffer->GetBindlessSrvIndex(), secondSrv);
    Read(vertices, indices, 1);
    const math::Vector3 third{7, 8, 9};
    const uint32_t thirdIndex = 29;
    m_resources->Update(vertices, &third, sizeof(third));
    m_resources->Update(indices, &thirdIndex, sizeof(thirdIndex));
    EXPECT_EQ(vertexBuffer->GetContentVersion(), 3u);
    Read(vertices, indices, 2);
    m_resources->Release(vertices);
    m_resources->Release(indices);

    const auto result = FinishAndReadback();
    ASSERT_EQ(result.size(), 3u);
    EXPECT_VEC3_NEAR((math::Vector3{result[0].x, result[0].y, result[0].z}), initial, 0.0f);
    EXPECT_VEC3_NEAR((math::Vector3{result[1].x, result[1].y, result[1].z}), second, 0.0f);
    EXPECT_VEC3_NEAR((math::Vector3{result[2].x, result[2].y, result[2].z}), third, 0.0f);
    EXPECT_FLOAT_EQ(result[0].w, static_cast<float>(initialIndex));
    EXPECT_FLOAT_EQ(result[1].w, static_cast<float>(secondIndex));
    EXPECT_FLOAT_EQ(result[2].w, static_cast<float>(thirdIndex));
}

TEST_F(BufferReadTest, StaleIndirectDependenciesRejectTheDispatchBeforeWriting)
{
    const math::Vector3 initial{1, 2, 3};
    const uint32_t initialIndex = 17;
    const auto vertices = m_resources->CreateVertexBuffer(&initial, sizeof(initial), sizeof(initial));
    const auto indices = m_resources->CreateIndexBuffer(&initialIndex, 1);
    ASSERT_TRUE(vertices.IsValid() && indices.IsValid());
    BeginFrame();
    Read(vertices, indices, 2, true);
    const auto staleVertices = vertices;
    m_resources->Release(vertices);
    const math::Vector3 replacement{4, 5, 6};
    const auto currentVertices = m_resources->CreateVertexBuffer(&replacement, sizeof(replacement), sizeof(replacement));
    ASSERT_TRUE(currentVertices.IsValid());
    /// @note 無効な間接依存の拒否で診断ログが出るのは想定内。以前の pixel 値を保存することを確認する。
    Read(currentVertices, indices, 2, true, staleVertices);
    const auto result = FinishAndReadback();
    ASSERT_EQ(result.size(), 3u);
    EXPECT_VEC3_NEAR((math::Vector3{result[2].x, result[2].y, result[2].z}), initial, 0.0f);
    EXPECT_FLOAT_EQ(result[2].w, static_cast<float>(initialIndex));
}

} /// @note namespace
} /// @note namespace fbzz::tests
