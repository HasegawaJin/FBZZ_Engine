/// @file    ThreadedRenderingTests.cpp
/// @brief   実 DX12 のフレーム提出・定数転送・ImGui 入力所有権を画像で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include "../../../Graphics/src/Renderer/QueuedComputeComposite.hpp"
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/Material.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Math/Matrix4.hpp>
#include <imgui.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace fbzz::tests {
namespace {

/// @note hidden window の Present も送信メッセージを待ち得るため、実 host と同じ完了待ち契約を使う。
void PumpGpuWaitMessages()
{
    MSG message{};
    (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
}

struct GuiContextDeleter {
    void operator()(ImGuiContext* context) const { if (context) ImGui::DestroyContext(context); }
};

class ThreadedRenderingTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Threaded rendering", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        auto native = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 32, 32);
        ASSERT_NE(native.renderer, nullptr);
        m_bundle = renderer::EnableRenderThread(std::move(native));
        m_bundle.renderer->SetRenderWaitPump(&PumpGpuWaitMessages);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        m_output = m_resources->CreateRenderTarget(16, 16, {1, renderer::Format::RGBA8, false});
        ASSERT_TRUE(m_output.IsValid());
    }
    void TearDown() override
    {
        if (m_bundle.renderer) {
            if (m_open) m_bundle.renderer->EndFrame();
            m_bundle.renderer->WaitForCpuFrame();
        }
        if (m_bundle.imguiRenderer) m_bundle.imguiRenderer->ImGuiShutdown();
        m_context.reset();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        testkit::Fixture::TearDown();
    }
    void Begin(uint64_t serial)
    {
        m_resources->AdvanceFrame();
        m_bundle.renderer->BeginCpuFrame(serial);
        m_bundle.renderer->BeginFrame();
        m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
        m_bundle.renderer->Clear({0, 0, 0, 1});
        m_open = true;
    }
    void End() { m_bundle.renderer->EndFrame(); m_open = false; }
    math::Vector4 Read()
    {
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        if (!m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_output, *m_resources, pixels, width, height)
            || width != 16 || height != 16) { ADD_FAILURE() << "Threaded readback failed"; return {}; }
        const size_t index = (8 * 16 + 8) * 4;
        return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
    }
    HWND m_window = nullptr;
    HRESULT m_com = E_FAIL;
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    std::unique_ptr<ImGuiContext, GuiContextDeleter> m_context;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_output;
    bool m_open = false;
};

TEST_F(ThreadedRenderingTest, RecordsRealDrawAndKeepsSubmittedConstantBytes)
{
    const auto* material = m_resources->GetErrorMaterial(false);
    ASSERT_NE(material, nullptr);
    const auto state = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
    const std::array<math::Vector3, 3> positions{{{-1, -1, 0.5f}, {-1, 3, 0.5f}, {3, -1, 0.5f}}};
    std::array<renderer::Vertex, 3> vertices{};
    for (size_t index = 0; index < vertices.size(); ++index) vertices[index].position = positions[index];
    const uint32_t indices[] = {0, 1, 2};
    const auto vertex = m_resources->CreateVertexBuffer(vertices.data(), sizeof(vertices), sizeof(renderer::Vertex));
    const auto index = m_resources->CreateIndexBuffer(indices, 3);
    const auto cameraBuffer = m_resources->CreateConstantBuffer(sizeof(math::Matrix4) * 3);
    const auto objectBuffer = m_resources->CreateConstantBuffer(sizeof(math::Matrix4));
    const std::array<math::Matrix4, 3> camera{math::Matrix4::Identity(), math::Matrix4::Identity(), math::Matrix4::Identity()};
    auto world = math::Matrix4::Identity();
    renderer::DrawCall call;
    call.shader = material->shader; call.pipelineState = state;
    call.vertexBuffer = vertex; call.indexBuffer = index; call.indexCount = 3;
    call.constantBuffers[0] = cameraBuffer; call.constantBuffers[1] = objectBuffer;
    Begin(1);
    m_resources->Update(cameraBuffer, camera.data(), sizeof(camera));
    m_resources->Update(objectBuffer, &world, sizeof(world));
    m_bundle.renderer->Submit(call, *m_resources);
    End();
    world.m[0][3] = 4.0f;
    const auto first = Read();
    EXPECT_NEAR(first.x, 1.0f, 0.02f);
    EXPECT_NEAR(first.y, 0.0f, 0.02f);
    EXPECT_NEAR(first.z, 1.0f, 0.02f);
    Begin(2);
    m_resources->Update(cameraBuffer, camera.data(), sizeof(camera));
    m_resources->Update(objectBuffer, &world, sizeof(world));
    m_bundle.renderer->Submit(call, *m_resources);
    End();
    const auto second = Read();
    EXPECT_NEAR(second.x, 0.0f, 0.02f);
    EXPECT_NEAR(second.y, 0.0f, 0.02f);
    EXPECT_NEAR(second.z, 0.0f, 0.02f);
}

TEST_F(ThreadedRenderingTest, QueuedComputeCompositeUsesCurrentPixelsAndSkipsRejectedDispatch)
{
    testkit::TempDir temporary;
    ASSERT_TRUE(temporary.IsValid());
    const auto path = temporary.File("QueuedComposite.cs.hlsl");
    {
        std::ofstream shader(path, std::ios::binary);
        shader << R"hlsl(/// @file    QueuedComposite.cs.hlsl
/// @brief   Produce a frame-specific color for conditional composite readback.
/// @author  Hasegawa Jin
/// @date    2026-10-30
cbuffer Indices : register(b14) { uint4 pixel[8]; uint4 vertex[1]; uint4 uav[2]; };
cbuffer Input : register(b5) { float4 tint; };
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    RWTexture2D<float4> output = ResourceDescriptorHeap[uav[0].x];
    output[id.xy] = tint;
}
)hlsl";
        ASSERT_TRUE(shader.good());
    }
    const auto computeShader = m_resources->LoadShader(path.generic_string());
    const auto copyShader = m_resources->LoadShader((std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT)
        / "PostProcess/Color/CopyColor.hlsl").generic_string());
    const auto texture = m_resources->CreateComputeTexture(16, 16);
    const auto constants = m_resources->CreateConstantBuffer(sizeof(math::Vector4));
    const auto state = m_resources->CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
    ASSERT_TRUE(computeShader && copyShader && texture && constants && state);
    renderer::ComputeCall compute;
    compute.shader = computeShader;
    compute.constantBuffers[5] = constants;
    compute.uavOutputs[0] = texture;
    compute.dispatchX = compute.dispatchY = 2;
    renderer::DrawCall draw;
    draw.shader = copyShader; draw.pipelineState = state; draw.vertexCount = 3;
    draw.textures[5] = texture;
    for (uint64_t frame = 1; frame <= 3; ++frame) {
        SCOPED_TRACE(frame);
        Begin(frame);
        const math::Vector4 tint = frame == 1 ? math::Vector4{1, 0, 0, 1} : math::Vector4{0, 0, 1, 1};
        m_resources->Update(constants, &tint, sizeof(tint));
        compute.shader = frame == 2 ? renderer::ResourceHandle<renderer::ShaderTag>{} : computeShader;
        renderer::compute_composite_detail::Submit(*m_bundle.renderer, *m_resources, compute, m_output, draw);
        End();
        const auto pixel = Read();
        EXPECT_NEAR(pixel.x, frame == 1 ? 1.0f : 0.0f, 0.02f);
        EXPECT_NEAR(pixel.y, 0.0f, 0.02f);
        EXPECT_NEAR(pixel.z, frame == 3 ? 1.0f : 0.0f, 0.02f);
    }
}

TEST_F(ThreadedRenderingTest, ColorAttachmentsPreserveSelectionAfterRejectionAndRestoreOrdinaryTargets)
{
    auto& r = *m_bundle.renderer;
    auto& resources = *m_resources;
    const auto other = resources.CreateRenderTarget(16, 16, {2, renderer::Format::RGBA8, false});
    const auto smallerTarget = resources.CreateRenderTarget(8, 8, {1, renderer::Format::RGBA8, false});
    const auto hdr = resources.CreateRenderTarget(16, 16, {1, renderer::Format::RGBA16F, false});
    const auto depth = resources.CreateRenderTarget(16, 16, {1, renderer::Format::RGBA8, true});
    const auto retired = resources.CreateRenderTarget(16, 16, {1, renderer::Format::RGBA8, false});
    ASSERT_TRUE(other && smallerTarget && hdr && depth && retired);
    resources.Release(retired);
    using Attachment = renderer::RenderTargetColorAttachment;
    const std::array selected{Attachment{m_output, 0}, Attachment{other, 1}};
    const std::array duplicate{Attachment{m_output, 0}, Attachment{m_output, 0}};
    const std::array wrongSize{Attachment{m_output, 0}, Attachment{smallerTarget, 0}};
    const std::array wrongFormat{Attachment{m_output, 0}, Attachment{hdr, 0}};
    const std::array withDepth{Attachment{m_output, 0}, Attachment{depth, 0}};
    const std::array lost{Attachment{m_output, 0}, Attachment{retired, 0}};
    const std::array wrongIndex{Attachment{m_output, 0}, Attachment{other, 2}};
    const std::array<Attachment, 9> oversized{};
    Begin(1);
    r.SetRenderTarget(other, resources);
    r.Clear({0, 0, 1, 1});
    ASSERT_TRUE(r.SetRenderTargetColors(selected, resources));
    EXPECT_FALSE(r.SetRenderTargetColors({}, resources));
    EXPECT_FALSE(r.SetRenderTargetColors(duplicate, resources));
    EXPECT_FALSE(r.SetRenderTargetColors(wrongSize, resources));
    EXPECT_FALSE(r.SetRenderTargetColors(wrongFormat, resources));
    EXPECT_FALSE(r.SetRenderTargetColors(withDepth, resources));
    EXPECT_FALSE(r.SetRenderTargetColors(lost, resources));
    EXPECT_FALSE(r.SetRenderTargetColors(wrongIndex, resources));
    EXPECT_FALSE(r.SetRenderTargetColors(oversized, resources));
    r.Clear({0, 1, 0, 1});
    r.ClearDepth();
    r.SetRenderTarget({}, resources);
    End();
    const auto selectedColor = Read();
    EXPECT_NEAR(selectedColor.x, 0, 0.002f); EXPECT_NEAR(selectedColor.y, 1, 0.002f);
    std::vector<float> untouched;
    uint32_t width = 0, height = 0;
    ASSERT_TRUE(r.CaptureRenderTargetToLinearRGBA(other, resources, untouched, width, height));
    ASSERT_EQ(width, 16u); ASSERT_EQ(height, 16u);
    const auto center = (8u * width + 8u) * 4u;
    EXPECT_NEAR(untouched[center + 1], 0, 0.002f); EXPECT_NEAR(untouched[center + 2], 1, 0.002f);
    const auto copy = resources.LoadShader(std::string(FBZZ_GRAPHICS_SHADER_ROOT) + "/PostProcess/Color/CopyColor.hlsl");
    const auto state = resources.CreatePipelineState({renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_OFF});
    ASSERT_TRUE(copy && state);
    renderer::DrawCall draw;
    draw.shader = copy; draw.pipelineState = state; draw.vertexCount = 3;
    draw.textures[5] = resources.GetColorTexture(other, 1);
    Begin(2);
    r.Submit(draw, resources);
    End();
    EXPECT_NEAR(Read().y, 1, 0.002f);
    Begin(3);
    ASSERT_TRUE(r.SetRenderTargetColors(selected, resources));
    r.Clear({1, 0, 0, 1});
    r.SetRenderTarget(m_output, resources);
    r.Clear({0, 0, 1, 1});
    End();
    EXPECT_NEAR(Read().z, 1, 0.002f);
    Begin(4);
    r.Submit(draw, resources);
    End();
    const auto retainedColor = Read();
    EXPECT_NEAR(retainedColor.x, 1, 0.002f); EXPECT_NEAR(retainedColor.z, 0, 0.002f);
}

TEST_F(ThreadedRenderingTest, RecordedProfilerViewsKeepPixelsAndRejectedViewOutOfCompletedSnapshot)
{
    auto& r = *m_bundle.renderer;
    for (uint64_t frame = 1; frame <= 5; ++frame) {
        Begin(frame);
        renderer::GpuProfilerViewMetadata metadata;
        metadata.applicationFrameSerial = frame;
        metadata.sceneGeneration = 9;
        metadata.planGeneration = 3;
        metadata.resourceEpoch = m_resources->GetResetVersion();
        metadata.outputId = m_output.id; metadata.outputGeneration = m_output.gen;
        metadata.viewId = 101; metadata.width = metadata.height = 16;

        ASSERT_TRUE(r.GpuProfRecordView(metadata));
        metadata.viewId = 999;
        r.GpuProfBeginPass("first"); r.Clear({0.25f, 0.5f, 0.75f, 1}); r.GpuProfEndPass("first");
        r.GpuProfEndRecordedView();
        metadata.width = 0;
        ASSERT_TRUE(r.GpuProfRecordView(metadata));
        r.GpuProfBeginPass("rejected"); r.Clear({0, 0, 0, 1}); r.GpuProfEndPass("rejected");
        r.GpuProfEndRecordedView();
        metadata.viewId = 102; metadata.width = 16;
        ASSERT_TRUE(r.GpuProfRecordView(metadata));
        r.GpuProfBeginPass("second"); r.Clear({0.75f, 0.5f, 0.25f, 1}); r.GpuProfEndPass("second");
        r.GpuProfEndRecordedView(); End();

        const auto pixel = Read();
        EXPECT_VEC3_NEAR(math::Vector3(pixel.x, pixel.y, pixel.z), math::Vector3(0.75f, 0.5f, 0.25f), 0.01f);
        if (frame >= 3) {
            const auto snapshot = r.GpuProfGetSnapshot();
            ASSERT_TRUE(snapshot.available && snapshot.complete);
            ASSERT_EQ(snapshot.passes.size(), 2u);
            EXPECT_EQ(snapshot.passes[0].name, "first"); EXPECT_EQ(snapshot.passes[0].metadata.viewId, 101u);
            EXPECT_EQ(snapshot.passes[1].name, "second"); EXPECT_EQ(snapshot.passes[1].metadata.viewId, 102u);
            for (const auto& pass : snapshot.passes) {
                EXPECT_TRUE(pass.available);
                EXPECT_EQ(pass.metadata.sceneGeneration, 9u); EXPECT_EQ(pass.metadata.planGeneration, 3u);
                EXPECT_EQ(pass.metadata.outputGeneration, m_output.gen);
            }
        }
    }
}

TEST_F(ThreadedRenderingTest, ImGuiFrameSurvivesNextUiFrameOnProducer)
{
    m_context.reset(ImGui::CreateContext());
    ImGui::SetCurrentContext(m_context.get());
    ImGui::GetIO().IniFilename = nullptr;
    m_bundle.imguiRenderer->ImGuiInit(m_window);
    Begin(1);
    m_bundle.imguiRenderer->ImGuiNewFrame();
    ImGui::GetIO().DisplaySize = {16, 16};
    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {16, 16}, IM_COL32(255, 0, 0, 255));
    ImGui::Render();
    m_bundle.imguiRenderer->ImGuiRenderDrawData();
    End();
    m_bundle.renderer->BeginCpuFrame(2);
    m_bundle.imguiRenderer->ImGuiNewFrame();
    ImGui::GetIO().DisplaySize = {16, 16};
    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {16, 16}, IM_COL32(0, 0, 255, 255));
    const auto first = Read();
    EXPECT_NEAR(first.x, 1.0f, 0.02f);
    EXPECT_NEAR(first.z, 0.0f, 0.02f);
    m_bundle.renderer->BeginFrame();
    m_bundle.renderer->SetRenderTarget(m_output, *m_resources);
    m_bundle.renderer->Clear({0, 0, 0, 1});
    m_open = true;
    ImGui::Render();
    m_bundle.imguiRenderer->ImGuiRenderDrawData();
    End();
    const auto second = Read();
    EXPECT_NEAR(second.x, 0.0f, 0.02f);
    EXPECT_NEAR(second.z, 1.0f, 0.02f);
}

TEST_F(ThreadedRenderingTest, AdditionalNativeWindowCanRenderResizeAndClose)
{
    m_context.reset(ImGui::CreateContext());
    ImGui::SetCurrentContext(m_context.get());
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    m_bundle.imguiRenderer->ImGuiInit(m_window);
    /// @note 実 OS window と swap chain を検証するが、テスト用の追加 window は表示しない。
    ImGui::GetPlatformIO().Platform_ShowWindow = [](ImGuiViewport*) {};
    for (uint64_t frame = 1; frame <= 6; ++frame) {
        Begin(frame);
        m_bundle.imguiRenderer->ImGuiNewFrame();
        ImGui::NewFrame();
        if (frame <= 4) {
            ImGui::SetNextWindowPos({100, 100}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(frame <= 2 ? ImVec2{100, 100} : ImVec2{120, 100}, ImGuiCond_Always);
            ImGui::Begin("Detached native test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("Owned viewport input");
            ImGui::End();
        }
        ImGui::Render();
        m_bundle.imguiRenderer->ImGuiRenderDrawData();
        m_bundle.imguiRenderer->ImGuiRenderPlatformWindows();
        if (frame == 2) EXPECT_GT(ImGui::GetPlatformIO().Viewports.Size, 1);
        End();
    }
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(ImGui::GetPlatformIO().Viewports.Size, 1);
}

} /// @note namespace
} /// @note namespace fbzz::tests
