/// @file    BufferReadTests.cpp
/// @brief   Raw Buffer SRV と同期転送中の記録済み GPU 資源の寿命を実デバイスで検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/ILogSink.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <filesystem>
#include <memory>
#include <array>
#include <string>
#include <vector>
#include <fstream>
#include <Graphics/Renderer/BindlessIndices.hpp>

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

TEST_F(BufferReadTest, InputAssemblyBindingsFollowBufferSnapshotsTopologyAndFrameChanges)
{
    testkit::TempDir temporary("graphics_input_assembly");
    ASSERT_TRUE(temporary.IsValid());
    const auto path = temporary.File("InputAssembly.hlsl");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"hlsl(/// @file    InputAssembly.hlsl
/// @brief   Observe input-assembler vertex and index snapshots through native rasterization.
/// @author  Hasegawa Jin
/// @date    2026-10-23
struct Input { float4 position : POSITION; float4 color : COLOR; };
struct Output { float4 position : SV_Position; float4 color : COLOR; };
Output VSMain(Input input) { Output result; result.position = input.position; result.color = input.color; return result; }
float4 PSMain(Output input) : SV_Target0 { return input.color; }
)hlsl";
        ASSERT_TRUE(stream.good());
    }
    const auto computePath = temporary.File("InputAssembly.cs.hlsl");
    {
        std::ofstream stream(computePath, std::ios::binary);
        stream << R"hlsl(/// @file    InputAssembly.cs.hlsl
/// @brief   Change compute state between draws which share input-assembler resources.
/// @author  Hasegawa Jin
/// @date    2026-10-23
cbuffer Indices : register(b14) { uint4 pixel[8]; uint4 vertex[1]; uint4 uav[2]; };
[numthreads(1, 1, 1)] void CSMain() {
    RWTexture2D<float4> output = ResourceDescriptorHeap[uav[0].x]; output[uint2(0, 0)] = 1;
}
)hlsl";
        ASSERT_TRUE(stream.good());
    }
    const auto shader = m_resources->LoadShader(path.generic_string());
    const auto compute = m_resources->LoadShader(computePath.generic_string());
    ASSERT_TRUE(shader.IsValid() && compute.IsValid());
    struct Vertex { math::Vector4 position; math::Vector4 color; };
    struct PaddedVertex { Vertex value; math::Vector4 padding; };
    const std::array<math::Vector4, 5> positions{{{-1, -1, 0, 1}, {-1, 3, 0, 1}, {3, -1, 0, 1}, {-1, 0, 0, 1}, {1, 0, 0, 1}}};
    std::array<Vertex, 5> red{}, green{}, blue{};
    std::array<PaddedVertex, 5> yellow{};
    for (size_t index = 0; index < positions.size(); ++index) {
        red[index] = {positions[index], {1, 0, 0, 1}};
        green[index] = {positions[index], {0, 1, 0, 1}};
        blue[index] = {positions[index], {0, 0, 1, 1}};
        yellow[index] = {{positions[index], {1, 1, 0, 1}}, {}};
    }
    const uint32_t triangle[]{0, 1, 2}, degenerate[]{0, 0, 0}, line[]{3, 4, 4};
    const uint32_t arguments[]{3, 1, 0, 0, 0};
    const auto firstVertices = m_resources->CreateVertexBuffer(red.data(), sizeof(red), sizeof(Vertex));
    const auto secondVertices = m_resources->CreateVertexBuffer(green.data(), sizeof(green), sizeof(Vertex));
    const auto paddedVertices = m_resources->CreateVertexBuffer(yellow.data(), sizeof(yellow), sizeof(PaddedVertex));
    const auto indices = m_resources->CreateIndexBuffer(triangle, 3);
    const auto lineIndices = m_resources->CreateIndexBuffer(line, 3);
    const auto indirect = m_resources->CreateRWStructuredBuffer(arguments, 1, sizeof(arguments));
    ASSERT_TRUE(firstVertices.IsValid() && secondVertices.IsValid() && paddedVertices.IsValid()
        && indices.IsValid() && lineIndices.IsValid() && indirect.IsValid());
    std::array<renderer::ResourceHandle<renderer::RenderTargetTag>, 11> targets;
    for (auto& target : targets) {
        target = m_resources->CreateRenderTarget(8, 8, {1, renderer::Format::RGBA16F, false});
        ASSERT_TRUE(target.IsValid());
    }
    m_bundle.renderer->SetRenderWhenOccluded(true);

    /// @note Read back only after all rows: earlier draws must retain old CPU-updated snapshots across the frame ring.
    for (uint32_t frame = 0; frame < 4; ++frame) {
        SCOPED_TRACE(frame);
        m_resources->Update(secondVertices, green.data(), sizeof(green));
        m_resources->Update(indices, triangle, sizeof(triangle));
        BeginFrame();
        for (size_t row = 0; row < targets.size(); ++row) {
            if (row == 3) {
                m_resources->Update(secondVertices, blue.data(), sizeof(blue));
                renderer::ComputeCall dispatch;
                dispatch.shader = compute; dispatch.uavOutputs[0] = m_output;
                ASSERT_TRUE(m_bundle.renderer->TryDispatch(dispatch, *m_resources));
            }
            if (row == 4) m_resources->Update(indices, degenerate, sizeof(degenerate));
            if (row == 5) m_resources->Update(indices, triangle, sizeof(triangle));
            renderer::DrawCall draw;
            draw.shader = shader; draw.pipelineState = m_state;
            draw.vertexBuffer = row < 2 ? firstVertices : row == 10 ? paddedVertices : secondVertices;
            draw.indexBuffer = row == 6 ? lineIndices : indices;
            draw.indexCount = row == 6 ? 2u : 3u;
            draw.topology = row == 6 ? renderer::PrimitiveTopology::LINE_LIST : renderer::PrimitiveTopology::TRIANGLE_LIST;
            if (row == 8) draw.indirectArguments = indirect;
            m_bundle.renderer->SetRenderTarget(targets[row], *m_resources);
            m_bundle.renderer->Clear({0, 0, 0, 0});
            m_bundle.renderer->Submit(draw, *m_resources);
        }
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame();
        m_frameOpen = false;

        for (size_t row = 0; row < targets.size(); ++row) {
            SCOPED_TRACE(row);
            std::vector<float> rgba;
            uint32_t width = 0, height = 0;
            ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(targets[row], *m_resources, rgba, width, height));
            ASSERT_EQ(width, 8u); ASSERT_EQ(height, 8u); ASSERT_EQ(rgba.size(), 256u);
            const math::Vector4 expected = row < 2 ? math::Vector4{1, 0, 0, 1}
                : row == 2 ? math::Vector4{0, 1, 0, 1} : row == 10 ? math::Vector4{1, 1, 0, 1} : math::Vector4{0, 0, 1, 1};
            uint32_t colored = 0;
            for (size_t pixel = 0; pixel < rgba.size(); pixel += 4) {
                if (rgba[pixel + 3] == 0) continue;
                ++colored;
                EXPECT_FLOAT_EQ(rgba[pixel], expected.x); EXPECT_FLOAT_EQ(rgba[pixel + 1], expected.y);
                EXPECT_FLOAT_EQ(rgba[pixel + 2], expected.z); EXPECT_FLOAT_EQ(rgba[pixel + 3], 1);
            }
            if (row == 4) EXPECT_EQ(colored, 0u);
            else if (row == 6) { EXPECT_GT(colored, 0u); EXPECT_LT(colored, 64u); }
            else EXPECT_EQ(colored, 64u);
        }
    }
}

TEST_F(BufferReadTest, ConsecutiveGraphicsBindingsKeepEveryIndexAndSurviveFrameAndComputeChanges)
{
    testkit::TempDir temporary("graphics_bindless_indices");
    ASSERT_TRUE(temporary.IsValid());
    const auto path = temporary.File("Indices.hlsl");
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"hlsl(/// @file    Indices.hlsl
/// @brief   Observe the complete graphics b14 payload through a native draw.
/// @author  Hasegawa Jin
/// @date    2026-10-23
cbuffer Indices : register(b14) { uint4 pixel[8]; uint4 vertex[1]; uint4 uav[2]; };
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
uint Mix(uint value, uint index) { return (value * 17 + index % 251) % 251; }
float4 PSMain() : SV_Target0 {
    uint p = 0, v = 0, u = 0;
    for (uint i = 0; i < 32; ++i) p = Mix(p, pixel[i >> 2][i & 3]);
    for (uint j = 0; j < 4; ++j) v = Mix(v, vertex[0][j]);
    for (uint k = 0; k < 8; ++k) u = Mix(u, uav[k >> 2][k & 3]);
    return float4(p, v, u, 1);
}
)hlsl";
        ASSERT_TRUE(stream.good());
    }
    const auto shader = m_resources->LoadShader(path.generic_string());
    ASSERT_TRUE(shader.IsValid());
    const auto computePath = temporary.File("Change.cs.hlsl");
    {
        std::ofstream stream(computePath, std::ios::binary);
        stream << R"hlsl(/// @file    Change.cs.hlsl
/// @brief   Change native compute bindings between identical graphics payloads.
/// @author  Hasegawa Jin
/// @date    2026-10-23
cbuffer Indices : register(b14) { uint4 pixel[8]; uint4 vertex[1]; uint4 uav[2]; };
[numthreads(1, 1, 1)] void CSMain() {
    RWTexture2D<float4> output = ResourceDescriptorHeap[uav[0].x];
    output[uint2(0, 0)] = float4(1, 2, 3, 4);
}
)hlsl";
        ASSERT_TRUE(stream.good());
    }
    const auto compute = m_resources->LoadShader(computePath.generic_string());
    ASSERT_TRUE(compute.IsValid());
    const math::Vector4 value{1, 2, 3, 4};
    const auto structured = m_resources->CreateStructuredBuffer(&value, 1, sizeof(value));
    const auto raw = m_resources->CreateVertexBuffer(&value, sizeof(value), sizeof(value));
    ASSERT_TRUE(structured.IsValid() && raw.IsValid());
    std::array<renderer::ResourceHandle<renderer::RenderTargetTag>, 8> targets;
    for (auto& target : targets) {
        target = m_resources->CreateRenderTarget(1, 1, {1, renderer::Format::RGBA16F, false});
        ASSERT_TRUE(target.IsValid());
    }
    const auto fingerprint = [](const auto& indices) {
        uint32_t result = 0;
        for (const uint32_t index : indices) result = (result * 17 + index % 251) % 251;
        return static_cast<float>(result);
    };
    /// @note Four frames cross the frame ring; readback occurs after all draws to detect overwritten prior payloads.
    for (uint32_t frame = 0; frame < 4; ++frame) {
        SCOPED_TRACE(frame);
        BeginFrame();
        std::array<math::Vector4, 8> expected;
        for (size_t row = 0; row < targets.size(); ++row) {
            SCOPED_TRACE(row);
            renderer::DrawCall draw;
            draw.shader = shader;
            draw.pipelineState = m_state;
            draw.vertexCount = 3;
            renderer::BindlessIndicesConstants indices;
            indices.Reset();
            if (row >= 2 && row <= 6) {
                draw.textures[0] = m_output;
                indices.pixel[0] = m_resources->Get(m_output)->GetBindlessIndex();
            }
            if (row == 4) {
                draw.textures[31] = m_output;
                indices.pixel[31] = indices.pixel[0];
            }
            if (row == 5 || row == 6) {
                draw.psBuffers[0] = structured;
                draw.auxiliaryPsBuffers.back() = structured;
                draw.vsBuffers.back() = structured;
                indices.pixel[renderer::kPsBufferBaseSlot] = m_resources->Get(structured)->GetBindlessIndex();
                indices.pixel[renderer::kAuxiliaryPsBufferBaseSlot + draw.auxiliaryPsBuffers.size() - 1] = indices.pixel[renderer::kPsBufferBaseSlot];
                indices.vertex[draw.vsBuffers.size()] = indices.pixel[renderer::kPsBufferBaseSlot];
            }
            if (row == 6) {
                draw.instanceBuffer = structured;
                draw.vsBuffers.back() = {};
                draw.vsRawBuffers.back() = raw;
                indices.vertex[0] = m_resources->Get(structured)->GetBindlessIndex();
                indices.vertex[draw.vsRawBuffers.size()] = m_resources->Get(raw)->GetBindlessSrvIndex();
            }
            if (row == 3) {
                renderer::ComputeCall dispatch;
                dispatch.shader = compute;
                dispatch.uavOutputs[0] = m_output;
                ASSERT_TRUE(m_bundle.renderer->TryDispatch(dispatch, *m_resources));
            }
            expected[row] = {fingerprint(indices.pixel), fingerprint(indices.vertex), fingerprint(indices.uav), 1};
            m_bundle.renderer->SetRenderTarget(targets[row], *m_resources);
            m_bundle.renderer->Submit(draw, *m_resources);
        }
        m_bundle.renderer->SetRenderTarget({}, *m_resources);
        m_bundle.renderer->EndFrame();
        m_frameOpen = false;
        for (size_t row = 0; row < targets.size(); ++row) {
            SCOPED_TRACE(row);
            std::vector<float> rgba;
            uint32_t width = 0, height = 0;
            ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(targets[row], *m_resources, rgba, width, height));
            ASSERT_EQ(rgba.size(), 4u);
            EXPECT_FLOAT_EQ(rgba[0], expected[row].x);
            EXPECT_FLOAT_EQ(rgba[1], expected[row].y);
            EXPECT_FLOAT_EQ(rgba[2], expected[row].z);
            EXPECT_FLOAT_EQ(rgba[3], 1);
        }
    }
}

class SynchronousUploadTest : public BufferReadTest, private core::ILogSink {
protected:
    void SetUp() override
    {
        /// @note Force the existing DIRECT fallback only during renderer creation, then restore the process environment.
        struct CopyQueueEnvironment {
            std::wstring previous;
            bool hadValue = false;
            bool changed = false;
            ~CopyQueueEnvironment()
            {
                if (changed)
                    EXPECT_TRUE(SetEnvironmentVariableW(L"FBZZ_COPY_QUEUE", hadValue ? previous.c_str() : nullptr));
            }
        } environment;
        SetLastError(ERROR_SUCCESS);
        const DWORD capacity = GetEnvironmentVariableW(L"FBZZ_COPY_QUEUE", nullptr, 0);
        if (capacity == 0) {
            const DWORD error = GetLastError();
            ASSERT_TRUE(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND);
            environment.hadValue = error != ERROR_ENVVAR_NOT_FOUND;
        } else {
            environment.hadValue = true;
            environment.previous.resize(capacity);
            const DWORD length = GetEnvironmentVariableW(L"FBZZ_COPY_QUEUE", environment.previous.data(), capacity);
            ASSERT_LT(length, capacity);
            ASSERT_GT(length, 0u);
            environment.previous.resize(length);
        }
        ASSERT_TRUE(SetEnvironmentVariableW(L"FBZZ_COPY_QUEUE", L"0"));
        environment.changed = true;
        core::Logger::AddSink(this);
        m_sinkAdded = true;
        BufferReadTest::SetUp();
        if (m_bundle.renderer) m_bundle.renderer->SetRenderWhenOccluded(true);
    }

    void TearDown() override
    {
        BufferReadTest::TearDown();
        if (m_sinkAdded) core::Logger::RemoveSink(this);
        EXPECT_EQ(m_gpuErrors, 0u);
    }

    void OnLog(const core::LogEntry& entry) override
    {
        const std::string text = entry.message;
        if (entry.level == core::LogLevel::LOG_ERROR
            || (entry.level == core::LogLevel::WARNING
                && (text.find("[WARNING]") != std::string::npos
                    || text.find("[ERROR]") != std::string::npos
                    || text.find("[CORRUPTION]") != std::string::npos))) ++m_gpuErrors;
    }

    void DrawTexture(renderer::ResourceHandle<renderer::TextureTag> texture,
                     renderer::ResourceHandle<renderer::RenderTargetTag> target)
    {
        m_bundle.renderer->SetRenderTarget(target, *m_resources);
        m_bundle.renderer->Clear({0, 0, 0, 0});
        renderer::DrawCall draw;
        draw.shader = m_copy;
        draw.pipelineState = m_state;
        draw.vertexCount = 3;
        draw.textures[5] = texture;
        m_bundle.renderer->Submit(draw, *m_resources);
    }

    void ExpectColor(renderer::ResourceHandle<renderer::RenderTargetTag> target,
                     const math::Vector3& expected)
    {
        std::vector<float> rgba;
        uint32_t width = 0, height = 0;
        ASSERT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(target, *m_resources, rgba, width, height));
        ASSERT_EQ(width, 3u);
        ASSERT_EQ(height, 1u);
        ASSERT_EQ(rgba.size(), 12u);
        for (size_t pixel = 0; pixel < 3; ++pixel)
            EXPECT_VEC3_NEAR((math::Vector3{rgba[pixel * 4], rgba[pixel * 4 + 1], rgba[pixel * 4 + 2]}), expected, 0.001f);
    }

    uint32_t m_gpuErrors = 0;
    bool m_sinkAdded = false;
};

TEST_F(SynchronousUploadTest, TextureAndBufferWaitsKeepRecordingFrameRetirementPending)
{
    const std::array<uint8_t, 4> red{255, 0, 0, 255};
    const std::array<uint8_t, 4> green{0, 255, 0, 255};
    const std::array<uint8_t, 4> blue{0, 0, 255, 255};
    const auto oldTexture = m_resources->CreateTexture(red.data(), 1, 1);
    const auto oldTarget = m_resources->CreateRenderTarget(3, 1,
        renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
    const auto newTarget = m_resources->CreateRenderTarget(3, 1,
        renderer::RenderTargetDesc{1, renderer::Format::RGBA16F, false});
    ASSERT_TRUE(oldTexture && oldTarget && newTarget);
    const uint32_t oldDescriptor = m_resources->Get(oldTexture)->GetBindlessIndex();
    ASSERT_NE(oldDescriptor, renderer::INVALID_BINDLESS_INDEX);
    const math::Vector3 value{1, 2, 3};
    const uint32_t index = 17;
    const auto vertices = m_resources->CreateVertexBuffer(&value, sizeof(value), sizeof(value));
    const auto indices = m_resources->CreateIndexBuffer(&index, 1);
    ASSERT_TRUE(vertices && indices);

    BeginFrame();
    DrawTexture(oldTexture, oldTarget);
    m_resources->Release(oldTexture);
    const renderer::TextureMipData mip{green.data(), 1, 1};
    uint64_t pendingToken = 0;
    const auto pending = m_resources->BeginTextureUpload(&mip, 1, pendingToken);
    ASSERT_TRUE(pending);
    ASSERT_NE(pendingToken, 0u);
    /// @note DIRECT fallback tokens share the frame submission boundary, including the retired texture's recorded draw.
    EXPECT_FALSE(m_resources->IsUploadComplete(pendingToken));
    const auto newTexture = m_resources->CreateTexture(blue.data(), 1, 1);
    ASSERT_TRUE(newTexture);
    EXPECT_NE(m_resources->Get(newTexture)->GetBindlessIndex(), oldDescriptor);
    EXPECT_NE(m_resources->Get(pending)->GetBindlessIndex(), oldDescriptor);
    EXPECT_FALSE(m_resources->IsUploadComplete(pendingToken));
    DrawTexture(newTexture, newTarget);
    m_resources->Release(newTexture);

    BufferReadInput input;
    input.vertexDescriptor = m_resources->Get(vertices)->GetBindlessSrvIndex();
    input.indexDescriptor = m_resources->Get(indices)->GetBindlessSrvIndex();
    input.outputPixel = 2;
    const auto table = m_resources->CreateGpuLocalStructuredBuffer(&input, 1, sizeof(input));
    ASSERT_TRUE(table);
    EXPECT_FALSE(m_resources->IsUploadComplete(pendingToken));
    renderer::ComputeCall dispatch;
    dispatch.shader = m_compute;
    dispatch.srvBuffers[14] = table;
    dispatch.srvRawBuffers[0] = vertices;
    dispatch.srvRawBuffers[1] = indices;
    dispatch.uavOutputs[0] = m_output;
    m_bundle.renderer->Dispatch(dispatch, *m_resources);
    m_resources->Release(table);
    const auto result = FinishAndReadback();
    ASSERT_EQ(result.size(), 3u);
    EXPECT_VEC3_NEAR((math::Vector3{result[2].x, result[2].y, result[2].z}), value, 0.0f);
    EXPECT_FLOAT_EQ(result[2].w, static_cast<float>(index));
    EXPECT_TRUE(m_resources->IsUploadComplete(pendingToken));
    ExpectColor(oldTarget, {1, 0, 0});
    ExpectColor(newTarget, {0, 0, 1});
}

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
