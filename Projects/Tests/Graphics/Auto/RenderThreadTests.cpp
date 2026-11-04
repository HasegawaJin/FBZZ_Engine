/// @file    RenderThreadTests.cpp
/// @brief   別スレッドの並行実行、入力所有権、CPU 完了と終了時の寿命を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <Graphics/Renderer/RenderThread.hpp>
#include <Graphics/Renderer/DebugDraw.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include "../../../Graphics/src/Passes/Geometry/OwnedSkinningBatch.hpp"
#include <Graphics/Renderer/IShader.hpp>
#include <cstdlib>
#include <fstream>
#include <Graphics/Renderer/AssetPathService.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/RenderCpuTrace.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/Material.hpp>
#include "../../../Graphics/src/Renderer/MaterialTransferPayload.hpp"
#include "../../../Graphics/src/Renderer/BufferStateReadScope.hpp"
#include "../../../Graphics/src/Renderer/QueuedComputeComposite.hpp"
#include <Core/Profiler/Profiler.hpp>
#include <Core/ExternalCallbackBoundary.hpp>
#include <Core/Util/FileSystem.hpp>
#include <atomic>
#include <array>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>
#include <cstring>
#include <cmath>
#include <string>
#include <optional>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>
#include <algorithm>
#include <unordered_map>
#include <imgui.h>

namespace fbzz::tests {
namespace {

struct RenderFrameResult {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    std::atomic<bool> updateRan{false};
    std::atomic<bool> destroyed{false};
    std::thread::id executionThread;
    std::vector<int> values;
    bool sawUpdate = false;
};

class OwnedRenderFrame final : public renderer::IRenderFrame {
public:
    OwnedRenderFrame(std::vector<int> values, std::shared_ptr<RenderFrameResult> result)
        : m_values(std::move(values)), m_result(std::move(result)) {}
    ~OwnedRenderFrame() override { m_result->destroyed.store(true); }
    void Execute() override
    {
        m_result->executionThread = std::this_thread::get_id();
        m_result->entered.release();
        m_result->resume.acquire();
        m_result->values = m_values;
        m_result->sawUpdate = m_result->updateRan.load();
    }
private:
    std::vector<int> m_values;
    std::shared_ptr<RenderFrameResult> m_result;
};

class RenderThreadTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_result = std::make_shared<RenderFrameResult>();
        m_thread = std::make_unique<renderer::RenderThread>();
    }
    void TearDown() override
    {
        if (!m_resumed) m_result->resume.release();
        m_thread.reset();
        m_result.reset();
        testkit::Fixture::TearDown();
    }
    void Resume()
    {
        m_resumed = true;
        m_result->resume.release();
    }
    std::unique_ptr<renderer::RenderThread> m_thread;
    std::shared_ptr<RenderFrameResult> m_result;
    bool m_resumed = false;
};

TEST_F(RenderThreadTest, ExecutesOnAnotherThreadWhileNextGameUpdateProceeds)
{
    const auto producer = std::this_thread::get_id();
    ASSERT_TRUE(m_thread->Submit(1, std::make_unique<OwnedRenderFrame>(std::vector<int>{7}, m_result)));
    m_result->entered.acquire();
    m_result->updateRan.store(true);
    Resume();
    ASSERT_TRUE(m_thread->WaitUntilCompleted(1));
    EXPECT_NE(m_result->executionThread, producer);
    EXPECT_TRUE(m_result->sawUpdate);
}

TEST_F(RenderThreadTest, OwnsInputAfterProducerChangesAndReleasesSource)
{
    std::vector<int> source{2, 3, 5};
    ASSERT_TRUE(m_thread->Submit(4, std::make_unique<OwnedRenderFrame>(source, m_result)));
    m_result->entered.acquire();
    source.assign(200, 99);
    source.clear();
    source.shrink_to_fit();
    Resume();
    ASSERT_TRUE(m_thread->WaitUntilCompleted(4));
    EXPECT_EQ(m_result->values, (std::vector<int>{2, 3, 5}));
}

TEST_F(RenderThreadTest, CompletionIncludesInputDestruction)
{
    ASSERT_TRUE(m_thread->Submit(2, std::make_unique<OwnedRenderFrame>(std::vector<int>{}, m_result)));
    m_result->entered.acquire();
    EXPECT_FALSE(m_result->destroyed.load());
    Resume();
    m_thread->WaitIdle();
    EXPECT_TRUE(m_result->destroyed.load());
    EXPECT_TRUE(m_thread->WaitUntilCompleted(2));
    EXPECT_FALSE(m_thread->WaitUntilCompleted(3));
}

TEST_F(RenderThreadTest, RejectsInvalidInputsAndNonIncreasingSerials)
{
    EXPECT_FALSE(m_thread->Submit(0, std::make_unique<OwnedRenderFrame>(std::vector<int>{}, m_result)));
    EXPECT_FALSE(m_thread->Submit(1, {}));
    ASSERT_TRUE(m_thread->Submit(10, std::make_unique<OwnedRenderFrame>(std::vector<int>{}, m_result)));
    m_result->entered.acquire();
    Resume();
    ASSERT_TRUE(m_thread->WaitUntilCompleted(10));
    EXPECT_FALSE(m_thread->Submit(10, std::make_unique<OwnedRenderFrame>(std::vector<int>{}, m_result)));
    EXPECT_FALSE(m_thread->Submit(9, std::make_unique<OwnedRenderFrame>(std::vector<int>{}, m_result)));
}

TEST_F(RenderThreadTest, StopDrainsAcceptedFrameAndClosesSubmission)
{
    ASSERT_TRUE(m_thread->Submit(1, std::make_unique<OwnedRenderFrame>(std::vector<int>{11}, m_result)));
    m_result->entered.acquire();
    Resume();
    m_thread->Stop();
    EXPECT_TRUE(m_result->destroyed.load());
    EXPECT_EQ(m_result->values, (std::vector<int>{11}));
    EXPECT_TRUE(m_thread->WaitUntilCompleted(1));
    EXPECT_FALSE(m_thread->Submit(2, std::make_unique<OwnedRenderFrame>(std::vector<int>{}, m_result)));
    m_thread->Stop();
}

struct ExternalPumpOrder {
    std::binary_semaphore* resume = nullptr;
    bool* resumed = nullptr;
    uint32_t notifications = 0, pumps = 0;
    bool ordered = true;
};
thread_local ExternalPumpOrder* s_externalPumpOrder = nullptr;

void ObservePreparedPublicationPump()
{
    auto& state = *s_externalPumpOrder;
    state.ordered &= state.notifications > state.pumps;
    ++state.pumps;
    if (!*state.resumed) {
        *state.resumed = true;
        state.resume->release();
    }
}

TEST_F(RenderThreadTest, PublishesExternalBoundaryBeforeWaitPumpEvenWhenHandlerFails)
{
    ASSERT_TRUE(m_thread->Submit(1, std::make_unique<OwnedRenderFrame>(std::vector<int>{7}, m_result)));
    m_result->entered.acquire();
    ExternalPumpOrder state{&m_result->resume, &m_resumed};
    s_externalPumpOrder = &state;
    m_thread->SetWaitPump(&ObservePreparedPublicationPump);
    {
        const core::ExternalCallbackScope observer([](void* context) {
            ++static_cast<ExternalPumpOrder*>(context)->notifications;
            return false;
        }, &state);
        EXPECT_TRUE(m_thread->WaitUntilCompleted(1));
    }
    m_thread->SetWaitPump(nullptr);
    s_externalPumpOrder = nullptr;
    EXPECT_GT(state.pumps, 0u);
    EXPECT_TRUE(state.ordered);
    EXPECT_EQ(state.notifications, state.pumps);
    EXPECT_TRUE(m_result->destroyed.load());
}

struct GuiCapture {
    ImGuiID viewport = 0;
    ImU32 color = 0;
    const ImDrawVert* buffer = nullptr;
    std::thread::id thread;
    const void* platformUserData = nullptr;
    const void* rendererUserData = nullptr;
};

struct RhiResult {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    int currentValue = 0;
    std::vector<int> draws;
    std::vector<std::string> operations;
    std::vector<std::thread::id> threads;
    bool blockFirst = false;
    size_t blockTransferNumber = 0;
    std::vector<int> transfers;
    std::vector<int> releasedValues;
    int structuredValue = 0;
    std::function<void()> transferProbe;
    std::function<void()> bufferSizeProbe;
    size_t bufferSizeProbeSkip = 0;
    std::function<void()> structuredSizeProbe;
    bool bufferTracksWrites = false;
    bool tryDispatchResult = false;
    bool blockFirstCompute = false;
    std::vector<uint32_t> computeGroups;
    std::vector<uint32_t> submittedVertices;
    uint32_t compositeTargets = 0;
    std::vector<GuiCapture> gui;
    std::vector<std::thread::id> viewportChanges;
};

struct ReentrantWaitPumpState {
    renderer::IRenderer* renderer = nullptr;
    renderer::ResourceManager* resources = nullptr;
    std::binary_semaphore* resume = nullptr;
    bool* resumed = nullptr;
    bool invoked = false;
    bool nestedResult = false;
};

struct PostSubmitRecordWaitPumpState {
    renderer::IRenderer* renderer = nullptr;
    renderer::ResourceManager* resources = nullptr;
    renderer::ResourceHandle<renderer::ConstantBufferTag> constantBuffer;
    std::binary_semaphore* resume = nullptr;
    bool* resumed = nullptr;
    bool invoked = false;
};

thread_local ReentrantWaitPumpState* s_reentrantWaitPumpState = nullptr;
thread_local PostSubmitRecordWaitPumpState* s_postSubmitRecordWaitPumpState = nullptr;

void ReentrantSyncWaitPump()
{
    auto* state = s_reentrantWaitPumpState;
    if (!state || state->invoked) return;
    state->invoked = true;
    *state->resumed = true;
    state->resume->release();
    state->nestedResult = state->renderer->TryDispatch({}, *state->resources);
}

void PostSubmitRecordWaitPump()
{
    auto* state = s_postSubmitRecordWaitPumpState;
    if (!state || state->invoked) return;
    state->invoked = true;
    int value = 47;
    state->resources->Update(state->constantBuffer, &value, sizeof(value));
    renderer::DrawCall call;
    call.constantBuffers[0] = state->constantBuffer;
    state->renderer->Submit(call, *state->resources);
    *state->resumed = true;
    state->resume->release();
}

class TestConstantBuffer final : public renderer::IConstantBuffer {
public:
    explicit TestConstantBuffer(RhiResult& result) : m_result(result) {}
    ~TestConstantBuffer() override { m_result.releasedValues.push_back(m_result.currentValue); }
    void Update(const void* data, size_t size) override
    {
        if (size == sizeof(int)) std::memcpy(&m_result.currentValue, data, size);
        m_result.threads.push_back(std::this_thread::get_id());
        m_result.transfers.push_back(m_result.currentValue);
        if (m_result.transferProbe) { auto probe = std::exchange(m_result.transferProbe, {}); probe(); }
        if (m_result.blockTransferNumber == m_result.transfers.size()) {
            m_result.entered.release();
            m_result.resume.acquire();
        }
    }
    size_t GetSize() const override { return sizeof(int); }
private:
    RhiResult& m_result;
};

class TraceTestBuffer final : public renderer::IBuffer {
public:
    TraceTestBuffer(RhiResult& result, const void* data, size_t bytes, uint32_t stride)
        : m_result(result), m_data(bytes), m_stride(stride)
    { if (data && bytes) std::memcpy(m_data.data(), data, bytes); }
    void Update(const void* data, size_t bytes) override
    {
        if (data && bytes <= m_data.size()) {
            std::memcpy(m_data.data(), data, bytes);
            if (m_result.bufferTracksWrites) ++m_version;
        }
    }
    size_t GetSize() const override
    {
        const size_t bytes = m_data.size();
        if (m_result.bufferSizeProbe) {
            if (m_result.bufferSizeProbeSkip != 0) --m_result.bufferSizeProbeSkip;
            else { auto probe = std::exchange(m_result.bufferSizeProbe, {}); probe(); }
        }
        return bytes;
    }
    uint32_t GetStride() const override { return m_stride; }
    uint64_t GetContentVersion() const override { return m_version; }
    void NotifyGpuWrite() override { if (m_result.bufferTracksWrites) ++m_version; }
    bool CopyData(size_t offset, size_t bytes, void* out) const override
    {
        if (!out || offset > m_data.size() || bytes > m_data.size() - offset) return false;
        std::memcpy(out, m_data.data() + offset, bytes);
        return true;
    }
private:
    RhiResult& m_result;
    std::vector<uint8_t> m_data;
    uint32_t m_stride = 0;
    uint64_t m_version = 1;
};

class TestStructuredBuffer final : public renderer::IStructuredBuffer {
public:
    TestStructuredBuffer(RhiResult& result, uint32_t count, uint32_t stride)
        : m_result(result), m_count(count), m_stride(stride) {}
    void Update(const void* data, size_t bytes) override
    { if (data && bytes == sizeof(int)) std::memcpy(&m_result.structuredValue, data, bytes); }
    uint32_t GetElementCount() const override { return m_count; }
    uint32_t GetStride() const override { return m_stride; }
    size_t GetSize() const override
    {
        const size_t bytes = static_cast<size_t>(m_count) * m_stride;
        if (m_result.structuredSizeProbe) { auto probe = std::exchange(m_result.structuredSizeProbe, {}); probe(); }
        return bytes;
    }
private:
    RhiResult& m_result;
    uint32_t m_count, m_stride;
};

class TestRhi : public renderer::IRenderer {
public:
    explicit TestRhi(RhiResult& result) : m_result(result) {}
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall& call, renderer::ResourceManager&) override
    {
        profiler::Profiler::PushMarker(profiler::ProfilerMarker("NativeSubmit", "Render Thread Test"));
        m_result.threads.push_back(std::this_thread::get_id());
        m_result.draws.push_back(m_result.currentValue);
        m_result.submittedVertices.push_back(call.vertexCount);
        m_result.operations.push_back("draw:" + std::to_string(m_result.currentValue));
        if (m_result.blockFirst && m_result.draws.size() == 1) {
            m_result.entered.release();
            m_result.resume.acquire();
        }
    }
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    bool TryDispatch(const renderer::ComputeCall& call, renderer::ResourceManager&) override
    {
        m_result.computeGroups.push_back(call.dispatchX);
        m_result.operations.push_back("sync:" + std::to_string(m_result.currentValue));
        if (m_result.blockFirstCompute && m_result.computeGroups.size() == 1) {
            m_result.entered.release();
            m_result.resume.acquire();
        }
        return m_result.tryDispatchResult;
    }
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override { ++m_result.compositeTargets; }
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
private:
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void* data, size_t bytes, uint32_t stride) override
    { return std::make_unique<TraceTestBuffer>(m_result, data, bytes, stride); }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override
    { return std::make_unique<TestConstantBuffer>(m_result); }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t count, uint32_t stride) override
    { return std::make_unique<TestStructuredBuffer>(m_result, count, stride); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    RhiResult& m_result;
};


class TexturePathTestTexture final : public renderer::ITexture {
public:
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    /// @note Model immutable texture provenance so reload exercises the same-handle content-version contract.
    uint64_t GetContentVersion() const override { return m_version; }
    void AdoptContentVersion(uint64_t previousVersion) override { m_version = previousVersion + 1; }
private:
    uint64_t m_version = 1;
};

class TexturePathTestRhi final : public TestRhi {
public:
    explicit TexturePathTestRhi(RhiResult& result) : TestRhi(result) {}
    std::vector<std::string> sourcePaths;
private:
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string& path) override
    {
        sourcePaths.push_back(path);
        return std::make_unique<TexturePathTestTexture>();
    }
};

struct TexturePathServiceInputs {
    std::vector<std::string> assetPaths;
    std::vector<std::string> texturePaths;
};
TexturePathServiceInputs* s_texturePathServiceInputs = nullptr;

/// @note GraphicsAuto has no Engine asset registration; restore its empty service even after a fatal test assertion.
class TexturePathTestServiceScope {
public:
    explicit TexturePathTestServiceScope(TexturePathServiceInputs& inputs)
    {
        s_texturePathServiceInputs = &inputs;
        renderer::AssetPathService service;
        service.resolveAssetPath = [](const std::string& path) {
            s_texturePathServiceInputs->assetPaths.push_back(path);
            return path == "CaseSensitiveAlias/FirstRaw.dds"
                ? std::string("CaseSensitiveSource/FirstRaw.dds") : std::string{};
        };
        service.resolveTextureSource = [](std::string_view path, std::string& output) {
            s_texturePathServiceInputs->texturePaths.emplace_back(path);
            output.assign(path);
            return path == "CaseSensitiveSource/FirstRaw.dds";
        };
        renderer::SetAssetPathService(service);
    }
    ~TexturePathTestServiceScope()
    {
        renderer::SetAssetPathService({});
        s_texturePathServiceInputs = nullptr;
    }
    TexturePathTestServiceScope(const TexturePathTestServiceScope&) = delete;
    TexturePathTestServiceScope& operator=(const TexturePathTestServiceScope&) = delete;
};

class TextureCachePathTest : public testkit::Fixture {};

TEST_F(TextureCachePathTest, CacheIdentityPreservesCaseSensitiveSourceResolverInputsAndReload)
{
    TexturePathServiceInputs inputs;
    {
        TexturePathTestServiceScope service(inputs);
        RhiResult result;
        TexturePathTestRhi backend(result);
        renderer::ResourceManager resources(backend);
        const std::string reference = "CaseSensitiveAlias\\FirstRaw.dds";
        const auto texture = resources.LoadTexture(reference);
        ASSERT_NE(resources.Get(texture), nullptr);
        const auto version = resources.Get(texture)->GetContentVersion();
        ASSERT_EQ(inputs.assetPaths.size(), 1u);
        ASSERT_EQ(inputs.texturePaths.size(), 1u);
        ASSERT_EQ(backend.sourcePaths.size(), 1u);
        EXPECT_EQ(inputs.assetPaths[0], "CaseSensitiveAlias/FirstRaw.dds");
        EXPECT_EQ(inputs.texturePaths[0], "CaseSensitiveSource/FirstRaw.dds");
        EXPECT_EQ(backend.sourcePaths[0], "CaseSensitiveSource/FirstRaw.dds");
#if defined(_WIN32)
        EXPECT_EQ(resources.FindCachedTexture("casesensitivealias/firstrAw.dds"), texture);
#else
        EXPECT_FALSE(resources.FindCachedTexture("casesensitivealias/firstrAw.dds").IsValid());
#endif
        EXPECT_EQ(resources.ReloadTexture(reference), texture);
        ASSERT_NE(resources.Get(texture), nullptr);
        EXPECT_GT(resources.Get(texture)->GetContentVersion(), version);
        ASSERT_EQ(inputs.assetPaths.size(), 2u);
        ASSERT_EQ(inputs.texturePaths.size(), 2u);
        ASSERT_EQ(backend.sourcePaths.size(), 2u);
        EXPECT_EQ(inputs.assetPaths[1], inputs.assetPaths[0]);
        EXPECT_EQ(inputs.texturePaths[1], inputs.texturePaths[0]);
        EXPECT_EQ(backend.sourcePaths[1], backend.sourcePaths[0]);
#if defined(_WIN32)
        EXPECT_EQ(resources.LoadTexture("casesensitivealias/firstrAw.dds"), texture);
#else
        EXPECT_EQ(resources.LoadTexture(reference), texture);
#endif
        EXPECT_EQ(inputs.assetPaths.size(), 2u);
        EXPECT_EQ(inputs.texturePaths.size(), 2u);
        EXPECT_EQ(backend.sourcePaths.size(), 2u);
    }
    EXPECT_EQ(s_texturePathServiceInputs, nullptr);
    EXPECT_EQ(renderer::ResolveAssetPath("CaseSensitiveAlias/FirstRaw.dds"), "CaseSensitiveAlias/FirstRaw.dds");
    std::string source;
    EXPECT_TRUE(renderer::ResolveTextureSource("FirstRaw.dds", source));
    EXPECT_EQ(source, "FirstRaw.dds");
}

/// @note An unknown native facade observes only existing public entry points; it cannot create a classification scope.
class TransferProbeRhi final : public TestRhi {
public:
    explicit TransferProbeRhi(RhiResult& result) : TestRhi(result) {}
    bool IsRenderThread() const override { return m_executing; }
    void ExecuteResourceOperation(const std::function<void()>& operation) override
    {
        const bool previous = std::exchange(m_executing, true);
        operation();
        m_executing = previous;
    }
    bool RecordResourceUpdate(std::function<void()> operation) override
    {
        if (onRecord) onRecord();
        ExecuteResourceOperation(operation);
        return true;
    }
    std::function<void()> onRecord;
private:
    bool m_executing = false;
};

static_assert(!std::is_constructible_v<renderer::CpuShadowTransferScope, renderer::IRenderer&>);
static_assert(!std::is_copy_constructible_v<renderer::CpuShadowTransferScope>);

class TestGui final : public renderer::IImGuiRenderer {
public:
    explicit TestGui(RhiResult& result) : m_result(result) {}
    void ImGuiInit(void*) override
    {
        ImGui::GetIO().BackendRendererUserData = &m_result;
        auto& platform = ImGui::GetPlatformIO();
        platform.Renderer_CreateWindow = &ChangeViewport;
        platform.Renderer_DestroyWindow = &ChangeViewport;
        platform.Renderer_SetWindowSize = [](ImGuiViewport* viewport, ImVec2) { ChangeViewport(viewport); };
        platform.Renderer_RenderWindow = [](ImGuiViewport* viewport, void*) { Capture(*viewport->DrawData); };
        platform.Renderer_SwapBuffers = [](ImGuiViewport*, void*) {};
    }
    void ImGuiShutdown() override { ImGui::GetIO().BackendRendererUserData = nullptr; }
    void ImGuiNewFrame() override {}
    void ImGuiRenderDrawData() override {}
    void ImGuiPrepareTextures(ImDrawData& data) override
    {
        if (!data.Textures) return;
        for (auto* texture : *data.Textures) {
            texture->SetTexID(1);
            texture->SetStatus(ImTextureStatus_OK);
        }
    }
    void ImGuiRenderDrawData(ImDrawData& data) override { Capture(data); }
    void ImGuiRenderPlatformWindows() override {}
    void* GetImTextureID(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&, int) override { return nullptr; }
    void* GetImTextureID(renderer::ResourceHandle<renderer::TextureTag>, renderer::ResourceManager&) override { return nullptr; }
private:
    static void ChangeViewport(ImGuiViewport*)
    {
        static_cast<RhiResult*>(ImGui::GetIO().BackendRendererUserData)->viewportChanges.push_back(std::this_thread::get_id());
    }
    static void Capture(const ImDrawData& data)
    {
        auto& result = *static_cast<RhiResult*>(ImGui::GetIO().BackendRendererUserData);
        if (data.CmdLists.empty() || data.CmdLists[0]->VtxBuffer.empty()) return;
        const auto* vertices = data.CmdLists[0]->VtxBuffer.Data;
        result.gui.push_back({data.OwnerViewport->ID, vertices[0].col, vertices, std::this_thread::get_id(),
            data.OwnerViewport->PlatformUserData, data.OwnerViewport->RendererUserData});
    }
    RhiResult& m_result;
};

class ThreadedRhiTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        renderer::RendererBundle native{std::make_unique<TestRhi>(m_result), std::make_unique<TestGui>(m_result)};
        m_bundle = renderer::EnableRenderThread(std::move(native));
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        m_constants = m_resources->CreateConstantBuffer(sizeof(int));
    }
    void TearDown() override
    {
        if ((m_result.blockFirst || m_result.blockFirstCompute || m_result.blockTransferNumber != 0) && !m_resumed) m_result.resume.release();
        m_bundle.renderer->WaitForCpuFrame();
        m_bundle.imguiRenderer->ImGuiShutdown();
        if (m_guiContext) { ImGui::DestroyContext(m_guiContext); ImGui::SetCurrentContext(nullptr); }
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        m_bundle.renderer.reset();
        testkit::Fixture::TearDown();
    }
    void Draw(int value)
    {
        m_resources->Update(m_constants, &value, sizeof(value));
        renderer::DrawCall call;
        call.constantBuffers[0] = m_constants;
        m_bundle.renderer->Submit(call, *m_resources);
    }
    void InitGui()
    {
        m_guiContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_guiContext);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {32, 32};
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(1);
        m_bundle.imguiRenderer->ImGuiInit(nullptr);
    }
    void GuiFrame(ImU32 color)
    {
        m_bundle.imguiRenderer->ImGuiNewFrame();
        ImGui::NewFrame();
        ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {32, 32}, color);
        ImGui::Render();
        m_bundle.imguiRenderer->ImGuiRenderDrawData();
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_constants;
    RhiResult m_result;
    bool m_resumed = false;
    ImGuiContext* m_guiContext = nullptr;
};

TEST_F(ThreadedRhiTest, QueuedComputeCompositeOwnsInputsAndPreservesTransferOrderWithoutProducerWait)
{
    auto& r = *m_bundle.renderer;
    m_result.tryDispatchResult = true;
    m_result.blockFirstCompute = true;
    r.BeginCpuFrame(1);
    r.BeginFrame();
    renderer::ComputeCall compute;
    renderer::DrawCall draw;
    draw.vertexCount = 3;
    int value = 17;
    m_resources->Update(m_constants, &value, sizeof(value));
    compute.dispatchX = 17;
    renderer::compute_composite_detail::Submit(r, *m_resources, compute, {}, draw);
    m_result.entered.acquire();
    compute.dispatchX = 999;
    draw.vertexCount = 999;
    EXPECT_EQ(m_result.operations, (std::vector<std::string>{"sync:17"}));
    EXPECT_TRUE(m_result.draws.empty());
    m_resumed = true;
    m_result.resume.release();
    value = 29;
    m_resources->Update(m_constants, &value, sizeof(value));
    compute.dispatchX = 29;
    draw.vertexCount = 3;
    renderer::compute_composite_detail::Submit(r, *m_resources, compute, {}, draw);
    compute.dispatchX = 999;
    draw.vertexCount = 999;
    r.EndFrame();
    r.WaitForCpuFrame();
    EXPECT_EQ(m_result.operations, (std::vector<std::string>{"sync:17", "draw:17", "sync:29", "draw:29"}));
    EXPECT_EQ(m_result.computeGroups, (std::vector<uint32_t>{17, 29}));
    EXPECT_EQ(m_result.submittedVertices, (std::vector<uint32_t>{3, 3}));
    EXPECT_EQ(m_result.compositeTargets, 2u);
}

TEST_F(ThreadedRhiTest, RejectedQueuedComputeDoesNotCompositeStaleOutputOrChangeTarget)
{
    auto& r = *m_bundle.renderer;
    r.BeginCpuFrame(1);
    r.BeginFrame();
    const int value = 47;
    m_resources->Update(m_constants, &value, sizeof(value));
    renderer::compute_composite_detail::Submit(r, *m_resources, {}, {}, {});
    r.EndFrame();
    r.WaitForCpuFrame();
    EXPECT_EQ(m_result.operations, (std::vector<std::string>{"sync:47"}));
    EXPECT_TRUE(m_result.draws.empty());
    EXPECT_EQ(m_result.compositeTargets, 0u);
}

TEST_F(ThreadedRhiTest, NativeComputeCompositeAlsoGatesDrawOnCurrentDispatch)
{
    RhiResult result;
    TestRhi native(result);
    renderer::ResourceManager resources(native);
    renderer::ComputeCall compute;
    renderer::DrawCall draw;
    compute.dispatchX = 7;
    draw.vertexCount = 3;
    result.tryDispatchResult = true;
    renderer::compute_composite_detail::Submit(native, resources, compute, {}, draw);
    result.tryDispatchResult = false;
    compute.dispatchX = 8;
    renderer::compute_composite_detail::Submit(native, resources, compute, {}, draw);
    EXPECT_EQ(result.operations, (std::vector<std::string>{"sync:0", "draw:0", "sync:0"}));
    EXPECT_EQ(result.computeGroups, (std::vector<uint32_t>{7, 8}));
    EXPECT_EQ(result.submittedVertices, (std::vector<uint32_t>{3}));
    EXPECT_EQ(result.compositeTargets, 1u);
}

/// @note Probe resources validate the public synchronous operation boundary without exposing a production test getter.
struct OwnedSkinningProbe {
    std::function<void()> beforeOperation;
    float sourceValue = 0, paletteValue = 0;
    std::array<uint32_t, 4> constants{};
    uint32_t begin = 0, end = 0, attempts = 0, operations = 0;
    bool rejectAll = false, rejectGroup = false, bindless = false;
};

class OwnedProbeShader final : public renderer::IShader {
public:
    explicit OwnedProbeShader(std::string path) : m_path(std::move(path)) {}
    const std::string& GetPath() const override { return m_path; }
    const renderer::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
private:
    std::string m_path;
    renderer::ShaderDescriptor m_descriptor;
};

class OwnedProbeOutput final : public renderer::IBuffer {
public:
    OwnedProbeOutput(size_t bytes, uint32_t stride, uint32_t descriptor) : m_bytes(bytes), m_stride(stride), m_descriptor(descriptor) {}
    void Update(const void*, size_t) override {}
    size_t GetSize() const override { return m_bytes; }
    uint32_t GetStride() const override { return m_stride; }
    uint32_t GetBindlessUavIndex() const override { return m_descriptor; }
    uint64_t GetContentVersion() const override { return m_version; }
    void NotifyGpuWrite() override { ++m_version; }
private:
    size_t m_bytes;
    uint32_t m_stride, m_descriptor;
    uint64_t m_version = 5;
};

class OwnedProbeStructured final : public renderer::IStructuredBuffer {
public:
    OwnedProbeStructured(OwnedSkinningProbe& probe, const void* data, uint32_t count, uint32_t stride)
        : m_probe(probe), m_count(count), m_stride(stride) { if (data) Update(data, static_cast<size_t>(count) * stride); }
    void Update(const void* data, size_t bytes) override
    {
        if (!data || bytes < sizeof(float)) return;
        if (m_stride == sizeof(renderer::SkinnedVertex)) std::memcpy(&m_probe.sourceValue, data, sizeof(float));
        if (m_stride == sizeof(math::Matrix4)) std::memcpy(&m_probe.paletteValue, data, sizeof(float));
    }
    uint32_t GetElementCount() const override { return m_count; }
    uint32_t GetStride() const override { return m_stride; }
    size_t GetSize() const override { return static_cast<size_t>(m_count) * m_stride; }
private:
    OwnedSkinningProbe& m_probe;
    uint32_t m_count, m_stride;
};

class OwnedProbeConstants final : public renderer::IConstantBuffer {
public:
    OwnedProbeConstants(OwnedSkinningProbe& probe, size_t bytes) : m_probe(probe), m_bytes(bytes) {}
    void Update(const void* data, size_t bytes) override
    { if (data && bytes == sizeof(m_probe.constants) && bytes <= m_bytes) std::memcpy(m_probe.constants.data(), data, bytes); }
    size_t GetSize() const override { return m_bytes; }
private:
    OwnedSkinningProbe& m_probe;
    size_t m_bytes;
};

class OwnedProbeRhi final : public TestRhi {
public:
    OwnedProbeRhi(RhiResult& result, OwnedSkinningProbe& probe) : TestRhi(result), m_probe(probe) {}
    renderer::GraphicsCapabilities GetCapabilities() const override
    { m_capabilities.bindless = m_probe.bindless; return m_capabilities; }
    bool IsRenderThread() const override { return m_executing; }
    bool RecordResourceUpdate(std::function<void()> operation) override
    { if (m_executing) return false; m_pending.push_back(std::move(operation)); return true; }
    void WaitForCpuFrame() override { if (!m_executing && !m_pending.empty()) ExecuteResourceOperation([] {}); }
    void ExecuteResourceOperation(const std::function<void()>& operation) override
    {
        if (!m_executing) ++m_probe.operations;
        if (m_probe.beforeOperation) { auto callback = std::exchange(m_probe.beforeOperation, {}); callback(); }
        const bool previous = m_executing;
        m_executing = true;
        auto pending = std::move(m_pending);
        m_pending.clear();
        for (auto& transfer : pending) transfer();
        operation();
        m_executing = previous;
    }
    void BeginComputeBatch() override { ++m_probe.begin; }
    void EndComputeBatch() override { ++m_probe.end; }
    bool TryDispatch(const renderer::ComputeCall& call, renderer::ResourceManager& resources) override
    {
        if (!m_executing) {
            bool accepted = false;
            ExecuteResourceOperation([&] { accepted = TryDispatch(call, resources); });
            return accepted;
        }
        ++m_probe.attempts;
        if (m_probe.rejectAll || (m_probe.rejectGroup && !call.indirectWriteVertexBuffers.empty())) return false;
        if (auto* output = resources.Get(call.uavVertexBuffer)) output->NotifyGpuWrite();
        for (auto handle : call.indirectWriteVertexBuffers) if (auto* output = resources.Get(handle)) output->NotifyGpuWrite();
        return true;
    }
private:
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t bytes) override
    { return std::make_unique<OwnedProbeConstants>(m_probe, bytes); }
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string& path) override { return std::make_unique<OwnedProbeShader>(path); }
    std::unique_ptr<renderer::IBuffer> CreateNativeGpuWritableVertexBuffer(size_t bytes, uint32_t stride) override
    { return std::make_unique<OwnedProbeOutput>(bytes, stride, ++m_descriptor); }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void* data, uint32_t count, uint32_t stride) override
    { return std::make_unique<OwnedProbeStructured>(m_probe, data, count, stride); }
    OwnedSkinningProbe& m_probe;
    mutable renderer::GraphicsCapabilities m_capabilities;
    std::vector<std::function<void()>> m_pending;
    bool m_executing = false;
    uint32_t m_descriptor = 7;
};

/// @note Set both private mode switches for one test and restore the invoking process's comparison settings.
class SkinningBatchEnvironment {
public:
    SkinningBatchEnvironment(const char* owned, const char* demand = "0")
    {
        if (const char* value = std::getenv("FBZZ_OWNED_SKINNING_BATCH")) m_owned = value;
        if (const char* value = std::getenv("FBZZ_GPU_SKINNING_DEMAND")) m_demand = value;
        _putenv_s("FBZZ_OWNED_SKINNING_BATCH", owned ? owned : "");
        _putenv_s("FBZZ_GPU_SKINNING_DEMAND", demand ? demand : "");
    }
    ~SkinningBatchEnvironment()
    {
        _putenv_s("FBZZ_OWNED_SKINNING_BATCH", m_owned ? m_owned->c_str() : "");
        _putenv_s("FBZZ_GPU_SKINNING_DEMAND", m_demand ? m_demand->c_str() : "");
    }
private:
    std::optional<std::string> m_owned, m_demand;
};

class OwnedSkinningOperationTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_rhi = std::make_unique<OwnedProbeRhi>(m_result, m_probe);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_rhi);
        const auto shaderPath = m_temp.File("OwnedSkinning.hlsl");
        std::ofstream(shaderPath) << "float4 main() : SV_Target { return 0; }";
        m_handles.skinningComputeCS = m_resources->LoadShader(shaderPath.generic_string());
        m_handles.skinningCB = m_resources->CreateConstantBuffer(16);
        ASSERT_TRUE(m_handles.skinningComputeCS.IsValid());
    }
    void TearDown() override
    {
        m_probe.beforeOperation = {};
        if (m_resources) renderer::ReleaseSkinningRequestCaches(*m_resources);
        m_resources.reset(); m_rhi.reset();
        testkit::Fixture::TearDown();
    }
    renderer::RenderSkinningInput Request()
    {
        std::array<renderer::SkinnedVertex, 3> vertices{};
        vertices[0].position.x = 2;
        auto source = m_resources->CreateStructuredBuffer(vertices.data(), 3, sizeof(renderer::SkinnedVertex));
        auto output = m_resources->CreateGpuWritableVertexBuffer(3 * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        return {source, {}, output, 3, 0, 1};
    }
    testkit::TempDir m_temp;
    RhiResult m_result;
    OwnedSkinningProbe m_probe;
    std::unique_ptr<OwnedProbeRhi> m_rhi;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::RenderPassHandles m_handles;
    renderer::Camera m_camera;
    renderer::RenderSettings m_settings;
    bool OwnedRequested() const
    { return renderer::skinning_batch_detail::Enabled(); }
};

/// @note Three distinct source groups and an unbatched legacy palette must all finish in one producer-to-worker operation.
TEST_F(OwnedSkinningOperationTest, DefaultOwnedBatchCompletesDistinctGroupsAndLegacyInputInOneOperation)
{
    const SkinningBatchEnvironment environment(nullptr);
    m_probe.bindless = true;
    renderer::RenderPassContext context{{}, *m_rhi, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    context.skinningRequests = {Request(), Request(), Request(), Request()};
    context.skinningMatrices = {math::Matrix4::Identity()};
    auto& legacy = context.skinningRequests.back();
    legacy.boneCount = 0;
    legacy.bonePalette = m_resources->CreateStructuredBuffer(context.skinningMatrices.data(), 1, sizeof(math::Matrix4));
    m_probe.operations = 0;
    renderer::ExecuteSkinningRequests(context);
    EXPECT_EQ(m_probe.operations, 1u);
    EXPECT_EQ(m_probe.begin, 1u); EXPECT_EQ(m_probe.end, 1u); EXPECT_EQ(m_probe.attempts, 4u);
    EXPECT_EQ(context.statsSkinningDispatchCount, 4u); EXPECT_EQ(context.statsSkinningVertexCount, 12u);
    ASSERT_TRUE(context.skinningBatchReceipt.ownedPath);
    ASSERT_EQ(context.skinningBatchReceipt.writes.size(), 4u);
    for (size_t index = 0; index < context.skinningRequests.size(); ++index) {
        const auto& write = context.skinningBatchReceipt.writes[index];
        EXPECT_EQ(write.output, context.skinningRequests[index].outputVertices);
        EXPECT_TRUE(write.recorded);
        EXPECT_EQ(write.previousVersion, 5u); EXPECT_EQ(write.resultingVersion, 6u);
        ASSERT_NE(m_resources->Get(write.output), nullptr);
        EXPECT_EQ(m_resources->Get(write.output)->GetContentVersion(), 6u);
    }
}

/// @note Explicit control retains each synchronous group failure and individual fallback instead of silently selecting owned work.
TEST_F(OwnedSkinningOperationTest, ExplicitControlPreservesMultipleGroupFailureFallbacks)
{
    const SkinningBatchEnvironment environment("0");
    m_probe.bindless = true; m_probe.rejectGroup = true;
    renderer::RenderPassContext context{{}, *m_rhi, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    context.skinningRequests = {Request(), Request(), Request()};
    context.skinningMatrices = {math::Matrix4::Identity()};
    m_probe.operations = 0;
    renderer::ExecuteSkinningRequests(context);
    EXPECT_GE(m_probe.operations, 6u);
    EXPECT_EQ(m_probe.attempts, 6u); EXPECT_EQ(m_probe.begin, 1u); EXPECT_EQ(m_probe.end, 1u);
    EXPECT_FALSE(context.skinningBatchReceipt.ownedPath);
    EXPECT_EQ(context.statsSkinningDispatchCount, 3u); EXPECT_EQ(context.statsSkinningVertexCount, 9u);
    for (const auto& request : context.skinningRequests) {
        ASSERT_NE(m_resources->Get(request.outputVertices), nullptr);
        EXPECT_EQ(m_resources->Get(request.outputVertices)->GetContentVersion(), 6u);
    }
}

/// @note The failed group and all individual retries share the same synchronous worker operation.
TEST_F(OwnedSkinningOperationTest, OwnedMultipleGroupFailureFallbacksRemainInsideOneOperation)
{
    const SkinningBatchEnvironment environment("1");
    m_probe.bindless = true; m_probe.rejectGroup = true;
    renderer::RenderPassContext context{{}, *m_rhi, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    context.skinningRequests = {Request(), Request(), Request()};
    context.skinningMatrices = {math::Matrix4::Identity()};
    m_probe.operations = 0;
    renderer::ExecuteSkinningRequests(context);
    EXPECT_EQ(m_probe.operations, 1u);
    EXPECT_EQ(m_probe.attempts, 6u); EXPECT_EQ(m_probe.begin, 1u); EXPECT_EQ(m_probe.end, 1u);
    EXPECT_EQ(context.statsSkinningDispatchCount, 3u); EXPECT_EQ(context.statsSkinningVertexCount, 9u);
    ASSERT_EQ(context.skinningBatchReceipt.writes.size(), 3u);
    for (const auto& write : context.skinningBatchReceipt.writes) {
        EXPECT_TRUE(write.recorded);
        EXPECT_EQ(write.previousVersion, 5u); EXPECT_EQ(write.resultingVersion, 6u);
    }
}

/// @note Reset at the synchronous boundary invalidates the captured owner before native validation or dispatch.
TEST_F(OwnedSkinningOperationTest, OwnedBoundaryResetDoesNotPublishAnOldResourceEpoch)
{
    const SkinningBatchEnvironment environment("1");
    renderer::RenderPassContext context{{}, *m_rhi, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    context.skinningRequests = {Request()}; context.skinningMatrices = {math::Matrix4::Identity()};
    const auto epoch = m_resources->GetResetVersion();
    m_probe.beforeOperation = [&] { m_resources->Reset(); };
    renderer::ExecuteSkinningRequests(context);
    EXPECT_NE(m_resources->GetResetVersion(), epoch);
    EXPECT_EQ(m_probe.attempts, 0u); EXPECT_EQ(m_probe.begin, 0u); EXPECT_EQ(m_probe.end, 0u);
    EXPECT_EQ(context.statsSkinningDispatchCount, 0u); EXPECT_EQ(context.statsSkinningVertexCount, 0u);
    EXPECT_TRUE(context.skinningBatchReceipt.ownedPath);
    EXPECT_EQ(context.skinningBatchReceipt.resourceEpoch, epoch);
    EXPECT_TRUE(context.skinningBatchReceipt.writes.empty());
}

/// @note A GPU-demand experiment retains its original control path; explicit owned selection still has precedence.
TEST_F(OwnedSkinningOperationTest, ExplicitModesAndGpuDemandKeepTheirOriginalExecutionBoundaries)
{
    struct Mode { const char* owned; const char* demand; bool expectedOwned; };
    for (const auto& mode : {Mode{"0", "0", false}, Mode{"1", "0", true}, Mode{nullptr, "1", false}, Mode{"1", "1", true}}) {
        SCOPED_TRACE(mode.owned ? mode.owned : "unset");
        SCOPED_TRACE(mode.demand);
        const SkinningBatchEnvironment environment(mode.owned, mode.demand);
        renderer::RenderPassContext context{{}, *m_rhi, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
        context.skinningRequests = {Request()}; context.skinningMatrices = {math::Matrix4::Identity()};
        renderer::ExecuteSkinningRequests(context);
        EXPECT_EQ(context.skinningBatchReceipt.ownedPath, mode.expectedOwned);
        EXPECT_EQ(context.statsSkinningDispatchCount, 1u); EXPECT_EQ(context.statsSkinningVertexCount, 3u);
        ASSERT_NE(m_resources->Get(context.skinningRequests[0].outputVertices), nullptr);
        EXPECT_EQ(m_resources->Get(context.skinningRequests[0].outputVertices)->GetContentVersion(), 6u);
    }
}

TEST_F(OwnedSkinningOperationTest, OwnsRequestsAndMatricesWhileQueuedSourceBytesReachNativeDispatch)
{
    renderer::RenderPassContext context{{}, *m_rhi, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    const auto request = Request();
    context.skinningRequests = {request};
    context.skinningMatrices = {math::Matrix4::Identity()};
    context.skinningMatrices[0].m[0][0] = 3;
    std::array<renderer::SkinnedVertex, 3> updated{};
    updated[0].position.x = 9;
    m_resources->Update(request.sourceVertices, updated.data(), sizeof(updated));
    updated[0].position.x = -20;
    bool callback = false;
    if (OwnedRequested()) m_probe.beforeOperation = [&] {
        callback = true;
        context.skinningRequests.clear(); context.skinningRequests.shrink_to_fit();
        context.skinningMatrices.assign(24, math::Matrix4::Identity());
        context.skinningMatrices[0].m[0][0] = -40;
    };
    renderer::ExecuteSkinningRequests(context);
    EXPECT_EQ(callback, OwnedRequested());
    EXPECT_FLOAT_EQ(m_probe.sourceValue, 9);
    EXPECT_FLOAT_EQ(m_probe.paletteValue, 3);
    EXPECT_EQ(m_probe.constants, (std::array<uint32_t, 4>{3, 0, 0, 0}));
    EXPECT_EQ(context.statsSkinningDispatchCount, 1u); EXPECT_EQ(context.statsSkinningVertexCount, 3u);
    if (OwnedRequested()) {
        ASSERT_EQ(context.skinningBatchReceipt.writes.size(), 1u);
        EXPECT_EQ(context.skinningBatchReceipt.writes[0].output, request.outputVertices);
        EXPECT_TRUE(context.skinningBatchReceipt.writes[0].recorded);
        EXPECT_GT(context.skinningBatchReceipt.writes[0].resultingVersion, context.skinningBatchReceipt.writes[0].previousVersion);
    } else EXPECT_FALSE(context.skinningBatchReceipt.ownedPath);
    EXPECT_EQ(m_probe.begin, 1u); EXPECT_EQ(m_probe.end, 1u);
}

TEST_F(OwnedSkinningOperationTest, RejectedDispatchClosesBatchWithoutPublishingAnOldNativeVersion)
{
    renderer::RenderPassContext context{{}, *m_rhi, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    context.skinningRequests = {Request()}; context.skinningMatrices = {math::Matrix4::Identity()};
    m_probe.rejectAll = true;
    renderer::ExecuteSkinningRequests(context);
    EXPECT_EQ(m_probe.attempts, 1u); EXPECT_EQ(m_probe.begin, 1u); EXPECT_EQ(m_probe.end, 1u);
    EXPECT_EQ(context.statsSkinningDispatchCount, 0u); EXPECT_EQ(context.statsSkinningVertexCount, 0u);
    if (OwnedRequested()) {
        ASSERT_EQ(context.skinningBatchReceipt.writes.size(), 1u);
        EXPECT_EQ(context.skinningBatchReceipt.writes[0].previousVersion, 5u);
        EXPECT_FALSE(context.skinningBatchReceipt.writes[0].recorded);
    } else EXPECT_FALSE(context.skinningBatchReceipt.ownedPath);
    EXPECT_EQ(m_resources->Get(context.skinningRequests[0].outputVertices)->GetContentVersion(), 5u);
}

TEST_F(OwnedSkinningOperationTest, RejectedMeshGroupUsesWorkerFallbackAndConfirmsEachIndependentOutput)
{
    m_probe.bindless = true; m_probe.rejectGroup = true;
    renderer::RenderPassContext context{{}, *m_rhi, *m_resources, m_camera, m_settings, {}, ~0u, m_handles};
    const auto first = Request();
    auto second = first;
    second.outputVertices = m_resources->CreateGpuWritableVertexBuffer(3 * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    context.skinningRequests = {first, second}; context.skinningMatrices = {math::Matrix4::Identity()};
    renderer::ExecuteSkinningRequests(context);
    EXPECT_EQ(m_probe.attempts, 3u); EXPECT_EQ(m_probe.begin, 1u); EXPECT_EQ(m_probe.end, 1u);
    EXPECT_EQ(context.statsSkinningDispatchCount, 2u); EXPECT_EQ(context.statsSkinningVertexCount, 6u);
    if (OwnedRequested()) {
        ASSERT_EQ(context.skinningBatchReceipt.writes.size(), 2u);
        for (const auto& write : context.skinningBatchReceipt.writes) {
            EXPECT_TRUE(write.recorded); EXPECT_GT(write.resultingVersion, write.previousVersion);
        }
        EXPECT_NE(context.skinningBatchReceipt.writes[0].output, context.skinningBatchReceipt.writes[1].output);
    } else EXPECT_FALSE(context.skinningBatchReceipt.ownedPath);
}

void ExpectCapturedCircle(const std::vector<renderer::DebugDrawVertex>& vertices,
                          size_t firstVertex, const math::Vector3& center, float radius,
                          const math::Vector3& planeNormal, const math::Vector4& color,
                          const math::Vector3* cardinalAxis1 = nullptr,
                          const math::Vector3* cardinalAxis2 = nullptr)
{
    constexpr size_t segmentCount = 24;
    constexpr size_t verticesPerCircle = segmentCount * 2;
    constexpr float tolerance = 2.0e-4f;
    ASSERT_GT(radius, 0.0f);
    ASSERT_GE(vertices.size(), firstVertex + verticesPerCircle);
    const float normalLength = planeNormal.Length();
    ASSERT_GT(normalLength, 1.0e-6f);
    const math::Vector3 unitNormal = planeNormal * (1.0f / normalLength);
    const float angleStep = 2.0f * std::acos(-1.0f) / static_cast<float>(segmentCount);
    const float expectedCosine = std::cos(angleStep);
    const float expectedSine = std::sin(angleStep);
    float firstTurn = 0.0f;
    for (size_t segment = 0; segment < segmentCount; ++segment) {
        const size_t startIndex = firstVertex + segment * 2;
        const size_t endIndex = startIndex + 1;
        const size_t nextStartIndex = firstVertex + ((segment + 1) % segmentCount) * 2;
        const auto& start = vertices[startIndex];
        const auto& end = vertices[endIndex];
        const math::Vector3 startRadius = start.position - center;
        const math::Vector3 endRadius = end.position - center;
        EXPECT_VEC4_NEAR(start.color, color, 1.0e-6f);
        EXPECT_VEC4_NEAR(end.color, color, 1.0e-6f);
        EXPECT_NEAR(startRadius.Length(), radius, tolerance);
        EXPECT_NEAR(endRadius.Length(), radius, tolerance);
        EXPECT_NEAR(math::Vector3::Dot(startRadius, unitNormal), 0.0f, tolerance);
        EXPECT_NEAR(math::Vector3::Dot(endRadius, unitNormal), 0.0f, tolerance);
        EXPECT_VEC3_NEAR(end.position, vertices[nextStartIndex].position, 1.0e-6f);
        const math::Vector3 startUnit = startRadius * (1.0f / radius);
        const math::Vector3 endUnit = endRadius * (1.0f / radius);
        EXPECT_NEAR(math::Vector3::Dot(startUnit, endUnit), expectedCosine, tolerance);
        const float turn = math::Vector3::Dot(unitNormal, math::Vector3::Cross(startUnit, endUnit));
        EXPECT_NEAR(std::abs(turn), expectedSine, tolerance);
        if (segment == 0) firstTurn = turn;
        else EXPECT_GT(firstTurn * turn, 0.0f);
    }
    if (cardinalAxis1 && cardinalAxis2) {
        EXPECT_VEC3_NEAR(vertices[firstVertex].position, center + *cardinalAxis1 * radius, tolerance);
        EXPECT_VEC3_NEAR(vertices[firstVertex + 12].position, center + *cardinalAxis2 * radius, tolerance);
        EXPECT_VEC3_NEAR(vertices[firstVertex + 24].position, center - *cardinalAxis1 * radius, tolerance);
        EXPECT_VEC3_NEAR(vertices[firstVertex + 36].position, center - *cardinalAxis2 * radius, tolerance);
    }
}

TEST_F(ThreadedRhiTest, SphereCapturePreservesThreeClosed24SegmentCircles)
{
    const math::Vector3 center{2.25f, -3.5f, 4.75f};
    const float radius = 1.375f;
    const math::Vector4 color{0.2f, 0.4f, 0.7f, 0.9f};
    const math::Vector3 x{1.0f, 0.0f, 0.0f};
    const math::Vector3 y{0.0f, 1.0f, 0.0f};
    const math::Vector3 z{0.0f, 0.0f, 1.0f};
    renderer::DebugDrawCapture capture;
    renderer::DebugDraw::BeginCapture(capture);
    renderer::DebugDraw::Sphere(*m_bundle.renderer, center, radius, color);
    renderer::DebugDraw::EndCapture();

    ASSERT_EQ(capture.overlayLines.size(), 144u);
    EXPECT_TRUE(capture.depthLines.empty());
    EXPECT_TRUE(capture.triangles.empty());
    ExpectCapturedCircle(capture.overlayLines, 0, center, radius, math::Vector3::Cross(x, y), color, &x, &y);
    ExpectCapturedCircle(capture.overlayLines, 48, center, radius, math::Vector3::Cross(x, z), color, &x, &z);
    ExpectCapturedCircle(capture.overlayLines, 96, center, radius, math::Vector3::Cross(y, z), color, &y, &z);
}

TEST_F(ThreadedRhiTest, CircleCapturePreservesNonAxisNormalAndPerCallTransform)
{
    const math::Vector3 firstCenter{1.25f, -2.5f, 3.75f};
    const math::Vector3 firstNormal{1.0f, 2.0f, -3.0f};
    const float firstRadius = 0.625f;
    const math::Vector4 firstColor{0.9f, 0.3f, 0.1f, 0.8f};
    const math::Vector3 secondCenter{-4.5f, 0.75f, 6.25f};
    const math::Vector3 secondNormal{-2.0f, 1.0f, 4.0f};
    const float secondRadius = 1.45f;
    const math::Vector4 secondColor{0.15f, 0.8f, 0.35f, 1.0f};
    renderer::DebugDrawCapture capture;
    renderer::DebugDraw::BeginCapture(capture);
    renderer::DebugDraw::Circle(*m_bundle.renderer, firstCenter, firstNormal, firstRadius, firstColor);
    renderer::DebugDraw::Circle(*m_bundle.renderer, secondCenter, secondNormal, secondRadius, secondColor);
    renderer::DebugDraw::EndCapture();

    ASSERT_EQ(capture.overlayLines.size(), 96u);
    EXPECT_TRUE(capture.depthLines.empty());
    EXPECT_TRUE(capture.triangles.empty());
    ExpectCapturedCircle(capture.overlayLines, 0, firstCenter, firstRadius, firstNormal, firstColor);
    ExpectCapturedCircle(capture.overlayLines, 48, secondCenter, secondRadius, secondNormal, secondColor);
}

TEST_F(ThreadedRhiTest, ArcAndCapsuleKeepTheirExistingSegmentCounts)
{
    const float pi = std::acos(-1.0f);
    renderer::DebugDrawCapture arcCapture;
    renderer::DebugDraw::BeginCapture(arcCapture);
    renderer::DebugDraw::Arc(*m_bundle.renderer, {1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f},
                             {1.0f, 0.0f, 0.0f}, 0.75f, pi * 0.5f, {1.0f, 1.0f, 1.0f, 1.0f});
    renderer::DebugDraw::EndCapture();
    EXPECT_EQ(arcCapture.overlayLines.size(), 12u);

    renderer::DebugDrawCapture capsuleCapture;
    renderer::DebugDraw::BeginCapture(capsuleCapture);
    renderer::DebugDraw::Capsule(*m_bundle.renderer, {0.5f, -1.0f, 2.0f}, 0.75f, 1.25f,
                                 math::Vector4{0.6f, 0.7f, 0.8f, 1.0f});
    renderer::DebugDraw::EndCapture();
    EXPECT_EQ(capsuleCapture.overlayLines.size(), 200u);
}

TEST_F(ThreadedRhiTest, ResourceSummaryMatchesDetailedLedgerAfterRelease)
{
    const auto second = m_resources->CreateConstantBuffer(sizeof(int));
    std::vector<core::AllocationInfo> live;
    m_resources->CollectLiveDebugResources(live);
    EXPECT_EQ(m_resources->GetLiveDebugResourceCount(), live.size());
    size_t bytes = 0;
    for (const auto& allocation : live) bytes += allocation.size;
    EXPECT_EQ(m_resources->GetLiveDebugResourceBytes(), bytes);
    m_resources->Release(second);
    EXPECT_EQ(m_resources->GetLiveDebugResourceCount(), live.size() - 1);
    EXPECT_EQ(m_resources->GetLiveDebugResourceBytes(), bytes - sizeof(int));
}

TEST_F(ThreadedRhiTest, ReusesCompletedGuiBuffersWithoutChangingCapturedPixels)
{
    InitGui();
    for (uint64_t frame = 1; frame <= 6; ++frame) {
        m_bundle.renderer->BeginCpuFrame(frame);
        m_bundle.renderer->BeginFrame();
        GuiFrame(IM_COL32(static_cast<int>(frame), 0, 0, 255));
        m_bundle.renderer->EndFrame();
        m_bundle.renderer->WaitForCpuFrame();
    }
    ASSERT_EQ(m_result.gui.size(), 6u);
    for (size_t index = 0; index < m_result.gui.size(); ++index) {
        EXPECT_EQ(m_result.gui[index].buffer, m_result.gui.front().buffer);
        EXPECT_EQ(m_result.gui[index].color, IM_COL32(static_cast<int>(index + 1), 0, 0, 255));
        EXPECT_NE(m_result.gui[index].thread, std::this_thread::get_id());
    }
}

TEST_F(ThreadedRhiTest, GuiSnapshotsBorrowBackendDataWithoutOwningNativeViewport)
{
    InitGui();
    auto* viewport = ImGui::GetMainViewport();
    int platformToken = 1, rendererToken = 2;
    viewport->PlatformUserData = &platformToken;
    viewport->RendererUserData = &rendererToken;
    for (uint64_t frame = 1; frame <= 3; ++frame) {
        m_bundle.renderer->BeginCpuFrame(frame);
        m_bundle.renderer->BeginFrame();
        GuiFrame(IM_COL32(255, 0, 0, 255));
        m_bundle.renderer->EndFrame();
        m_bundle.renderer->WaitForCpuFrame();
    }
    EXPECT_EQ(m_result.gui.size(), 3u);
    for (const auto& capture : m_result.gui) {
        EXPECT_EQ(capture.platformUserData, &platformToken);
        EXPECT_EQ(capture.rendererUserData, &rendererToken);
    }
    m_bundle.imguiRenderer->ImGuiShutdown();
    EXPECT_EQ(viewport->PlatformUserData, &platformToken);
    EXPECT_EQ(viewport->RendererUserData, &rendererToken);
    /// @note This fake backend owns the original tokens and releases them before context destruction.
    viewport->PlatformUserData = nullptr;
    viewport->RendererUserData = nullptr;
}

TEST_F(ThreadedRhiTest, AdditionalViewportOwnsItsInputAndDoesNotWaitDuringStableFrames)
{
    InitGui();
    m_result.blockFirst = true;
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    GuiFrame(IM_COL32(255, 0, 0, 255));
    /// @note 初回 atlas の同期準備を済ませてから worker を止め、通常 viewport フレームの並行実行を検証する。
    Draw(31);
    auto* source = ImGui::GetDrawData();
    ImGuiViewport extra = *ImGui::GetMainViewport();
    extra.ID = 99;
    ImDrawData extraData = *source;
    extraData.OwnerViewport = &extra;
    extra.DrawData = &extraData;
    auto& platform = ImGui::GetPlatformIO();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    platform.Viewports.push_back(&extra);
    m_bundle.imguiRenderer->ImGuiRenderPlatformWindows();
    m_bundle.renderer->EndFrame();
    m_result.entered.acquire();
    m_bundle.renderer->BeginCpuFrame(2);
    m_bundle.imguiRenderer->ImGuiNewFrame();
    for (auto* list : source->CmdLists)
        for (auto& vertex : list->VtxBuffer) vertex.col = IM_COL32(0, 0, 255, 255);
    m_bundle.imguiRenderer->ImGuiRenderPlatformWindows();
    platform.Viewports.pop_back();
    m_resumed = true;
    m_result.resume.release();
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    ASSERT_EQ(m_result.gui.size(), 3u);
    EXPECT_EQ(m_result.gui[0].color, IM_COL32(255, 0, 0, 255));
    EXPECT_EQ(m_result.gui[1].viewport, 99u);
    EXPECT_EQ(m_result.gui[1].color, IM_COL32(255, 0, 0, 255));
    EXPECT_EQ(m_result.gui[2].viewport, 99u);
    EXPECT_EQ(m_result.gui[2].color, IM_COL32(0, 0, 255, 255));
    platform.Renderer_CreateWindow(&extra);
    platform.Renderer_SetWindowSize(&extra, {48, 48});
    platform.Renderer_DestroyWindow(&extra);
    ASSERT_EQ(m_result.viewportChanges.size(), 3u);
    for (const auto thread : m_result.viewportChanges) EXPECT_NE(thread, std::this_thread::get_id());
}

TEST_F(ThreadedRhiTest, PreservesUploadDrawOrderAndOwnsStackTransferData)
{
    const auto producer = std::this_thread::get_id();
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(17);
    Draw(23);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.draws, (std::vector<int>{17, 23}));
    ASSERT_EQ(m_result.threads.size(), 4u);
    for (const auto id : m_result.threads) EXPECT_NE(id, producer);
}

TEST_F(ThreadedRhiTest, CoalescesQueuedCommandsWithSyncAndPreservesReceiptOrder)
{
    m_result.tryDispatchResult = true;
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame(200, profiler::ProfileRecorder::NowMs());
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(17);
    EXPECT_TRUE(m_bundle.renderer->TryDispatch({}, *m_resources));
    Draw(23);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    profiler::Profiler::EndFrame();

    EXPECT_EQ(m_result.draws, (std::vector<int>{17, 23}));
    EXPECT_EQ(m_result.operations, (std::vector<std::string>{"draw:17", "sync:17", "draw:23"}));

    const auto& snapshot = profiler::Profiler::GetSnapshot();
    uint64_t syncKey = 0;
    uint64_t backpressureKey = 0;
    uint64_t packetKey = 0;
    uint64_t acceptanceKey = 0;
    uint64_t completionKey = 0;
    uint64_t callerKey = 0;
    for (const auto& descriptor : snapshot.descriptors) {
        if (descriptor.name == "ThreadedRenderer::Sync") syncKey = descriptor.sampleKey;
        if (descriptor.name == "ThreadedRenderer::SubmitCommandsBackpressure") backpressureKey = descriptor.sampleKey;
        if (descriptor.name == "ThreadedRenderer::SubmitPacket") packetKey = descriptor.sampleKey;
        if (descriptor.name == "ThreadedRenderer::PacketAcceptance") acceptanceKey = descriptor.sampleKey;
        if (descriptor.name == "ThreadedRenderer::PacketCompletionWait") completionKey = descriptor.sampleKey;
        if (descriptor.category == "Render synchronization"
            && descriptor.name.find("ThreadedRenderer::TryDispatch") != std::string::npos) callerKey = descriptor.sampleKey;
    }
    EXPECT_NE(syncKey, 0u);
    EXPECT_NE(backpressureKey, 0u);
    EXPECT_NE(packetKey, 0u);
    EXPECT_NE(acceptanceKey, 0u);
    EXPECT_NE(completionKey, 0u);
    EXPECT_NE(callerKey, 0u);
    bool foundSync = false;
    uint32_t syncBackpressureCount = 0;
    uint32_t syncPacketCount = 0;
    uint64_t syncSampleId = 0;
    uint64_t syncParentId = 0;
    for (const auto& sample : snapshot.samples) {
        if (sample.sampleKey == syncKey) {
            foundSync = true;
            syncSampleId = sample.sampleId;
            syncParentId = sample.parentSampleId;
        }
    }
    for (const auto& sample : snapshot.samples)
        if (sample.sampleKey == backpressureKey && sample.parentSampleId == syncSampleId) ++syncBackpressureCount;
    for (const auto& sample : snapshot.samples)
        if (sample.sampleKey == packetKey && sample.parentSampleId == syncSampleId) ++syncPacketCount;
    EXPECT_TRUE(foundSync);
    EXPECT_EQ(syncBackpressureCount, 1u);
    EXPECT_EQ(syncPacketCount, 1u);
    uint64_t packetSampleId = 0;
    bool callerFound = false;
    for (const auto& sample : snapshot.samples) {
        if (sample.sampleKey == packetKey && sample.parentSampleId == syncSampleId) packetSampleId = sample.sampleId;
        if (sample.sampleKey == callerKey && sample.sampleId == syncParentId) callerFound = true;
    }
    uint32_t acceptanceCount = 0;
    uint32_t completionCount = 0;
    for (const auto& sample : snapshot.samples) {
        if (sample.parentSampleId != packetSampleId) continue;
        if (sample.sampleKey == acceptanceKey) ++acceptanceCount;
        if (sample.sampleKey == completionKey) ++completionCount;
    }
    EXPECT_TRUE(callerFound);
    EXPECT_EQ(acceptanceCount, 1u);
    EXPECT_EQ(completionCount, 1u);
    profiler::Profiler::SetEnabled(false);
}

TEST_F(ThreadedRhiTest, WaitPumpCanReenterSyncWithoutDeadlockingOrReordering)
{
    m_result.blockFirst = true;
    m_result.tryDispatchResult = true;
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(31);
    m_bundle.renderer->EndFrame();
    m_result.entered.acquire();

    m_bundle.renderer->BeginCpuFrame(2);
    m_bundle.renderer->BeginFrame();
    Draw(47);
    ReentrantWaitPumpState state{
        .renderer = m_bundle.renderer.get(),
        .resources = m_resources.get(),
        .resume = &m_result.resume,
        .resumed = &m_resumed};
    s_reentrantWaitPumpState = &state;
    m_bundle.renderer->SetRenderWaitPump(&ReentrantSyncWaitPump);
    const bool outerResult = m_bundle.renderer->TryDispatch({}, *m_resources);
    m_bundle.renderer->SetRenderWaitPump(nullptr);
    s_reentrantWaitPumpState = nullptr;

    Draw(59);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_TRUE(state.invoked);
    EXPECT_TRUE(state.nestedResult);
    EXPECT_TRUE(outerResult);
    EXPECT_EQ(m_result.operations,
        (std::vector<std::string>{"draw:31", "draw:47", "sync:47", "sync:47", "draw:59"}));
}

TEST_F(ThreadedRhiTest, WaitForCpuFrameDrainsCommandsRecordedDuringPacketCompletionWait)
{
    m_result.blockFirst = true;
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(31);
    PostSubmitRecordWaitPumpState state{
        .renderer = m_bundle.renderer.get(),
        .resources = m_resources.get(),
        .constantBuffer = m_constants,
        .resume = &m_result.resume,
        .resumed = &m_resumed};
    s_postSubmitRecordWaitPumpState = &state;
    m_bundle.renderer->SetRenderWaitPump(&PostSubmitRecordWaitPump);
    m_bundle.renderer->WaitForCpuFrame();
    m_bundle.renderer->SetRenderWaitPump(nullptr);
    s_postSubmitRecordWaitPumpState = nullptr;

    EXPECT_TRUE(state.invoked);
    EXPECT_EQ(m_result.draws, (std::vector<int>{31, 47}));
    EXPECT_EQ(m_result.operations, (std::vector<std::string>{"draw:31", "draw:47"}));

    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
}

TEST_F(ThreadedRhiTest, AllowsNextGameUpdateWhilePreviousDrawIsProcessing)
{
    m_result.blockFirst = true;
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(31);
    m_bundle.renderer->EndFrame();
    m_result.entered.acquire();
    m_bundle.renderer->BeginCpuFrame(2);
    int nextGameValue = 47;
    m_resources->Update(m_constants, &nextGameValue, sizeof(nextGameValue));
    nextGameValue = 99;
    m_resumed = true;
    m_result.resume.release();
    m_bundle.renderer->BeginFrame();
    renderer::DrawCall call;
    call.constantBuffers[0] = m_constants;
    m_bundle.renderer->Submit(call, *m_resources);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.draws, (std::vector<int>{31, 47}));
}

TEST_F(ThreadedRhiTest, DoesNotReportQueuedUnsupportedDispatchAsRecorded)
{
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    EXPECT_FALSE(m_bundle.renderer->TryDispatch({}, *m_resources));
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
}

TEST_F(ThreadedRhiTest, ResourceReleaseDrainsRecordedReferencesBeforeDestroyingHandle)
{
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(53);
    m_resources->Release(m_constants);
    EXPECT_EQ(m_resources->Get(m_constants), nullptr);
    EXPECT_EQ(m_result.draws, (std::vector<int>{53}));
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
}

TEST_F(ThreadedRhiTest, GroupedBufferStatesObserveQueuedBytesAndRejectReleasedGenerations)
{
    const std::array<uint32_t, 4> original{3, 5, 7, 11};
    const auto released = m_resources->CreateVertexBuffer(original.data(), sizeof(original), sizeof(uint32_t));
    m_resources->Release(released);
    const auto current = m_resources->CreateVertexBuffer(original.data(), sizeof(original), sizeof(uint32_t));
    ASSERT_TRUE(current.IsValid());
    ASSERT_EQ(current.id, released.id);
    ASSERT_NE(current.gen, released.gen);
    const std::array<renderer::ResourceHandle<renderer::BufferTag>, 4> handles{current, {}, released, current};
    std::array<renderer::BufferResourceState, 4> states;

    m_bundle.renderer->BeginCpuFrame(1);
    const std::array<uint32_t, 4> updated{13, 17, 19, 23};
    m_resources->Update(current, updated.data(), sizeof(updated));
    m_resources->GetBufferStates(handles, states);
    EXPECT_TRUE(states[0].live);
    EXPECT_EQ(states[0].sizeBytes, sizeof(updated));
    EXPECT_EQ(states[0].stride, sizeof(uint32_t));
    EXPECT_EQ(states[0].contentVersion, 1u);
    EXPECT_FALSE(states[1].live);
    EXPECT_FALSE(states[2].live);
    EXPECT_TRUE(states[3].live);
    EXPECT_EQ(states[3].contentVersion, states[0].contentVersion);
    renderer::BufferResourceMetadata accepted;
    ASSERT_TRUE(m_resources->GetBufferMetadata(current, accepted));
    EXPECT_GT(accepted.contentVersion, states[0].contentVersion);
    std::array<uint32_t, 4> copied{};
    const auto* native = m_resources->Get(current);
    ASSERT_NE(native, nullptr);
    ASSERT_TRUE(native->CopyData(0, sizeof(copied), copied.data()));
    EXPECT_EQ(copied, updated);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
}

TEST_F(ThreadedRhiTest, GroupedBufferStatesResolveOnTheWorkerInsideAnExistingOperation)
{
    const std::array<uint32_t, 2> bytes{31, 47};
    const auto buffer = m_resources->CreateVertexBuffer(bytes.data(), sizeof(bytes), sizeof(uint32_t));
    const std::array<renderer::ResourceHandle<renderer::BufferTag>, 2> handles{buffer, buffer};
    std::array<renderer::BufferResourceState, 2> states;
    bool ranOnWorker = false;
    m_bundle.renderer->ExecuteResourceOperation([&] {
        ranOnWorker = m_bundle.renderer->IsRenderThread();
        m_resources->GetBufferStates(handles, states);
    });
    EXPECT_TRUE(ranOnWorker);
    EXPECT_TRUE(states[0].live);
    EXPECT_TRUE(states[1].live);
    EXPECT_EQ(states[0].sizeBytes, sizeof(bytes));
    EXPECT_EQ(states[1].stride, sizeof(uint32_t));
}

TEST_F(ThreadedRhiTest, RenderRecorderDoesNotModifyMainThreadSamples)
{
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame(100, profiler::ProfileRecorder::NowMs());
    profiler::Profiler::PushMarker(profiler::ProfilerMarker("GameUpdate", "Render Thread Test"));
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(67);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    profiler::Profiler::EndFrame();
    bool gameFound = false;
    bool nativeFound = false;
    for (const auto& descriptor : profiler::Profiler::GetSnapshot().descriptors) {
        gameFound |= descriptor.name == "GameUpdate";
        nativeFound |= descriptor.name == "NativeSubmit";
    }
    EXPECT_TRUE(gameFound);
    EXPECT_FALSE(nativeFound);
    EXPECT_EQ(profiler::Profiler::GetSnapshot().applicationFrameSerial, 100u);
    profiler::Profiler::SetEnabled(false);
}

/// @note Every fixture drains and destroys its worker before changing the diagnostic session.
class RenderCpuTraceRhiTest : public ThreadedRhiTest {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(renderer::cpu_trace::ConfigureForTesting());
        ThreadedRhiTest::SetUp();
    }
    void TearDown() override
    {
        profiler::Profiler::SetEnabled(false);
        ThreadedRhiTest::TearDown();
        EXPECT_TRUE(renderer::cpu_trace::DisableForTesting());
    }
};

TEST_F(RenderCpuTraceRhiTest, IdleBufferGetsCountWithoutDiagnosticClocksOrProfilerRecords)
{
    const std::array<uint32_t, 4> source{3, 5, 7, 11};
    const auto buffer = m_resources->CreateVertexBuffer(source.data(), sizeof(source), sizeof(uint32_t));
    ASSERT_TRUE(buffer.IsValid());
    m_bundle.renderer->WaitForCpuFrame();
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame(901, profiler::ProfileRecorder::NowMs());
    m_bundle.renderer->BeginCpuFrame(17);
    for (uint32_t i = 0; i < 1024; ++i) {
        const auto* native = m_resources->Get(buffer);
        ASSERT_NE(native, nullptr);
        EXPECT_EQ(native->GetStride(), sizeof(uint32_t));
    }
    const auto summary = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(summary.bufferProducer, 1024u);
    EXPECT_EQ(summary.waitCalls, 0u);
    EXPECT_EQ(summary.notReady, 0u);
    EXPECT_EQ(summary.waitInclusiveNs, 0u);
    EXPECT_EQ(summary.cpuQueries, 0u);
    EXPECT_EQ(summary.wallQueries, 0u);
    EXPECT_EQ(summary.cycleQueries, 0u);
    EXPECT_EQ(summary.packets, 0u);
    EXPECT_TRUE(summary.complete);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    profiler::Profiler::EndFrame();
    for (const auto& descriptor : profiler::Profiler::GetSnapshot().descriptors)
        EXPECT_EQ(descriptor.name.find("RenderCpuTrace"), std::string::npos);
}

TEST_F(RenderCpuTraceRhiTest, GroupedBufferStatesDrainOnceWithoutCreatingAnEmptyPacket)
{
    const uint32_t source = 7;
    const auto buffer = m_resources->CreateVertexBuffer(&source, sizeof(source), sizeof(source));
    ASSERT_TRUE(buffer.IsValid());
    const std::array<renderer::ResourceHandle<renderer::BufferTag>, 4> handles{buffer, {}, buffer, buffer};
    std::array<renderer::BufferResourceState, 4> states;
    m_bundle.renderer->BeginCpuFrame(17);
    m_resources->GetBufferStates(handles, states);
    const auto grouped = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(grouped.bufferProducer, 1u);
    EXPECT_EQ(grouped.waitCalls, 0u);
    EXPECT_EQ(grouped.packets, 0u);
    EXPECT_TRUE(states[0].live);
    EXPECT_FALSE(states[1].live);
    EXPECT_TRUE(states[2].live);
    EXPECT_TRUE(states[3].live);

    const std::array<renderer::ResourceHandle<renderer::BufferTag>, 2> invalid{};
    m_resources->GetBufferStates(invalid, std::span(states).first(invalid.size()));
    const auto afterInvalid = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(afterInvalid.bufferProducer, grouped.bufferProducer);
    EXPECT_EQ(afterInvalid.waitCalls, grouped.waitCalls);
    EXPECT_EQ(afterInvalid.packets, grouped.packets);
    EXPECT_FALSE(states[0].live);
    EXPECT_FALSE(states[1].live);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
}

TEST_F(RenderCpuTraceRhiTest, BufferReadScopeSkipsOnlyIdleDrainsAndObservesPreviouslyBorrowedNativeWrites)
{
    m_result.bufferTracksWrites = true;
    const uint32_t original = 7, updated = 19;
    const auto buffer = m_resources->CreateVertexBuffer(&original, sizeof(original), sizeof(original));
    auto* borrowed = m_resources->Get(buffer);
    ASSERT_NE(borrowed, nullptr);
    const std::array handles{buffer};
    std::array<renderer::BufferResourceState, 1> states;
    m_bundle.renderer->BeginCpuFrame(17);
    renderer::buffer_state_detail::BufferStateReadScope reads(*m_resources, *m_bundle.renderer);
    ASSERT_TRUE(reads.Read(handles, states));
    const auto firstVersion = states[0].contentVersion;
    for (size_t index = 0; index < 32; ++index) ASSERT_TRUE(reads.Read(handles, states));
    borrowed->Update(&updated, sizeof(updated));
    ASSERT_TRUE(reads.Read(handles, states));
    EXPECT_GT(states[0].contentVersion, firstVersion);
    const auto writtenVersion = states[0].contentVersion;
    borrowed->NotifyGpuWrite();
    ASSERT_TRUE(reads.Read(handles, states));
    EXPECT_GT(states[0].contentVersion, writtenVersion);
    EXPECT_TRUE(states[0].live);
    uint32_t copied = 0;
    ASSERT_TRUE(borrowed->CopyData(0, sizeof(copied), &copied));
    EXPECT_EQ(copied, updated);
    const auto summary = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(summary.waitCalls, 0u);
    EXPECT_EQ(summary.bufferProducer, 35u);
    EXPECT_EQ(summary.packets, 0u);
}

TEST_F(RenderCpuTraceRhiTest, BufferReadScopeInvalidatesAfterQueueSyncMutableGetFrameAndNestedRead)
{
    m_result.bufferTracksWrites = true;
    const uint32_t original = 7, updated = 29;
    const auto buffer = m_resources->CreateVertexBuffer(&original, sizeof(original), sizeof(original));
    const std::array handles{buffer};
    std::array<renderer::BufferResourceState, 1> states;
    m_bundle.renderer->BeginCpuFrame(17);
    renderer::buffer_state_detail::BufferStateReadScope reads(*m_resources, *m_bundle.renderer);
    ASSERT_TRUE(reads.Read(handles, states));
    const auto originalVersion = states[0].contentVersion;
    m_resources->Update(buffer, &updated, sizeof(updated));
    EXPECT_FALSE(reads.HasCurrentReadBoundary());
    ASSERT_TRUE(reads.Read(handles, states));
    EXPECT_GT(states[0].contentVersion, originalVersion);
    ASSERT_NE(m_resources->Get(buffer), nullptr);
    EXPECT_FALSE(reads.HasCurrentReadBoundary());
    ASSERT_TRUE(reads.Read(handles, states));
    m_bundle.renderer->TryDispatch({}, *m_resources);
    EXPECT_FALSE(reads.HasCurrentReadBoundary());
    ASSERT_TRUE(reads.Read(handles, states));
    m_bundle.renderer->CancelCpuFrame();
    EXPECT_FALSE(reads.HasCurrentReadBoundary());
    ASSERT_TRUE(reads.Read(handles, states));
    m_resources->AdvanceFrame();
    EXPECT_FALSE(reads.HasCurrentReadBoundary());
    ASSERT_TRUE(reads.Read(handles, states));
    {
        renderer::buffer_state_detail::BufferStateReadScope nested(*m_resources, *m_bundle.renderer);
        EXPECT_FALSE(nested.Read(handles, states));
        EXPECT_FALSE(reads.HasCurrentReadBoundary());
        EXPECT_TRUE(states[0].live);
    }
    ASSERT_TRUE(reads.Read(handles, states));
    EXPECT_TRUE(states[0].live);
}

TEST_F(ThreadedRhiTest, BufferReadScopeFallsBackOnceWhenANativeGetterQueuesAWrite)
{
    m_result.bufferTracksWrites = true;
    const uint32_t original = 7, updated = 41;
    const auto buffer = m_resources->CreateVertexBuffer(&original, sizeof(original), sizeof(original));
    const std::array handles{buffer};
    std::array<renderer::BufferResourceState, 1> states;
    renderer::buffer_state_detail::BufferStateReadScope reads(*m_resources, *m_bundle.renderer);
    ASSERT_TRUE(reads.Read(handles, states));
    const auto originalVersion = states[0].contentVersion;
    m_result.bufferSizeProbe = [&] { m_resources->Update(buffer, &updated, sizeof(updated)); };
    EXPECT_FALSE(reads.Read(handles, states));
    EXPECT_FALSE(reads.HasCurrentReadBoundary());
    EXPECT_TRUE(states[0].live);
    EXPECT_GT(states[0].contentVersion, originalVersion);
    ASSERT_TRUE(reads.Read(handles, states));
    uint32_t copied = 0;
    ASSERT_TRUE(m_resources->Get(buffer)->CopyData(0, sizeof(copied), &copied));
    EXPECT_EQ(copied, updated);
}

TEST_F(ThreadedRhiTest, BufferReadScopeRejectsAResetOrReplacedGenerationDuringNativeGetters)
{
    const uint32_t source = 7;
    const auto old = m_resources->CreateVertexBuffer(&source, sizeof(source), sizeof(source));
    std::array handles{old};
    std::array<renderer::BufferResourceState, 1> states;
    renderer::buffer_state_detail::BufferStateReadScope reads(*m_resources, *m_bundle.renderer);
    ASSERT_TRUE(reads.Read(handles, states));
    renderer::ResourceHandle<renderer::BufferTag> replacement;
    m_result.bufferSizeProbe = [&] {
        m_resources->Release(old);
        replacement = m_resources->CreateVertexBuffer(&source, sizeof(source), sizeof(source));
    };
    EXPECT_FALSE(reads.Read(handles, states));
    EXPECT_FALSE(states[0].live);
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.id, old.id);
    EXPECT_NE(replacement.gen, old.gen);
    handles[0] = replacement;
    ASSERT_TRUE(reads.Read(handles, states));
    EXPECT_TRUE(states[0].live);
    m_result.bufferSizeProbe = [&] { m_resources->Reset(); };
    EXPECT_FALSE(reads.Read(handles, states));
    EXPECT_FALSE(states[0].live);
    EXPECT_FALSE(reads.HasCurrentReadBoundary());
}

TEST_F(ThreadedRhiTest, BufferReadScopeRereadsSourceAndOutputAtTheirOriginalPositionsAfterOtherNativeGetters)
{
    m_result.bufferTracksWrites = true;
    const uint32_t originalBoneIndex = 0, invalidBoneIndex = 9;
    const auto source = m_resources->CreateVertexBuffer(&originalBoneIndex, sizeof(originalBoneIndex), sizeof(originalBoneIndex));
    const auto output = m_resources->CreateVertexBuffer(&originalBoneIndex, sizeof(originalBoneIndex), sizeof(originalBoneIndex));
    const auto structured = m_resources->CreateStructuredBuffer(&originalBoneIndex, 1, sizeof(originalBoneIndex));
    auto* borrowedSource = m_resources->Get(source);
    auto* borrowedOutput = m_resources->Get(output);
    ASSERT_NE(borrowedSource, nullptr);
    ASSERT_NE(borrowedOutput, nullptr);
    const std::array handles{source, output};
    std::array<renderer::BufferResourceState, 2> states;
    renderer::buffer_state_detail::BufferStateReadScope reads(*m_resources, *m_bundle.renderer);
    ASSERT_TRUE(reads.Read(handles, states));
    m_result.bufferSizeProbeSkip = 1;
    m_result.bufferSizeProbe = [&] { borrowedSource->Update(&invalidBoneIndex, sizeof(invalidBoneIndex)); };
    ASSERT_TRUE(reads.Read(handles, states));
    const auto* currentSource = reads.Get(source);
    ASSERT_NE(currentSource, nullptr);
    EXPECT_GT(currentSource->GetContentVersion(), states[0].contentVersion);
    uint32_t currentBoneIndex = 0;
    ASSERT_TRUE(currentSource->CopyData(0, sizeof(currentBoneIndex), &currentBoneIndex));
    EXPECT_EQ(currentBoneIndex, invalidBoneIndex);

    m_result.structuredSizeProbe = [&] { borrowedOutput->NotifyGpuWrite(); };
    const auto* nativeStructured = m_resources->Get(structured);
    ASSERT_NE(nativeStructured, nullptr);
    EXPECT_EQ(nativeStructured->GetSize(), sizeof(originalBoneIndex));
    const auto* currentOutput = reads.Get(output);
    ASSERT_NE(currentOutput, nullptr);
    const auto previousVersion = currentOutput->GetContentVersion();
    EXPECT_GT(previousVersion, states[1].contentVersion);
    EXPECT_FALSE(m_bundle.renderer->TryDispatch({}, *m_resources));
    EXPECT_EQ(reads.Get(output)->GetContentVersion(), previousVersion);
}

TEST_F(ThreadedRhiTest, BufferReadScopeRetainsFullReadsForWorkerAndUnregisteredFacades)
{
    const uint32_t source = 7;
    const auto buffer = m_resources->CreateVertexBuffer(&source, sizeof(source), sizeof(source));
    const std::array handles{buffer};
    std::array<renderer::BufferResourceState, 1> states;
    m_bundle.renderer->ExecuteResourceOperation([&] {
        renderer::buffer_state_detail::BufferStateReadScope worker(*m_resources, *m_bundle.renderer);
        EXPECT_FALSE(worker.Read(handles, states));
        EXPECT_TRUE(states[0].live);
    });
    TestRhi unregistered(m_result);
    renderer::ResourceManager other(unregistered);
    const auto otherBuffer = other.CreateVertexBuffer(&source, sizeof(source), sizeof(source));
    const std::array otherHandles{otherBuffer};
    renderer::buffer_state_detail::BufferStateReadScope unknown(other, unregistered);
    EXPECT_FALSE(unknown.Read(otherHandles, states));
    EXPECT_TRUE(states[0].live);
}

TEST_F(RenderCpuTraceRhiTest, DisabledTraceLeavesScopesAndIdleWaitsUnrecorded)
{
    ASSERT_TRUE(renderer::cpu_trace::DisableForTesting());
    m_bundle.renderer->BeginCpuFrame(19);
    {
        renderer::cpu_trace::ViewScope view(1, 2, 3, 4, 5, 6);
        view.SetPhase(renderer::cpu_trace::Phase::EXTRACT);
        const renderer::cpu_trace::SourceScope source(renderer::cpu_trace::Source::BATCH_STATIC, "disabled");
        for (uint32_t i = 0; i < 32; ++i) m_bundle.renderer->WaitForCpuFrame();
    }
    m_bundle.renderer->EndFrame();
    const auto summary = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(summary.selectedFrames, 0u);
    EXPECT_EQ(summary.phases, 0u);
    EXPECT_EQ(summary.waitCalls, 0u);
    EXPECT_EQ(summary.cpuQueries, 0u);
    EXPECT_EQ(summary.wallQueries, 0u);
    EXPECT_EQ(summary.cycleQueries, 0u);
}

TEST_F(RenderCpuTraceRhiTest, SelectedPacketKeepsItsViewAndFrameAfterProducerAdvances)
{
    m_result.blockFirst = true;
    m_bundle.renderer->BeginCpuFrame(23);
    {
        renderer::cpu_trace::ViewScope view(101, 202, 303, 404, 505, 606);
        view.SetViewId(2);
        view.SetPhase(renderer::cpu_trace::Phase::GRAPH);
        Draw(71);
        m_bundle.renderer->EndFrame();
        m_result.entered.acquire();
    }
    m_bundle.renderer->BeginCpuFrame(24);
    m_resumed = true;
    m_result.resume.release();
    m_bundle.renderer->WaitForCpuFrame();
    m_bundle.renderer->EndFrame();
    const auto packet = renderer::cpu_trace::SnapshotPacket(0);
    EXPECT_EQ(packet.applicationFrame, 23u);
    EXPECT_EQ(packet.resourceFrame, 303u);
    EXPECT_EQ(packet.token, 1u);
    EXPECT_EQ(packet.viewId, 2u);
    EXPECT_TRUE(packet.accepted);
    EXPECT_TRUE(packet.complete);
    const auto summary = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(summary.selectedFrames, 1u);
    EXPECT_EQ(summary.packets, 1u);
    EXPECT_EQ(summary.completedPackets, 1u);
    EXPECT_TRUE(summary.complete);
    EXPECT_GT(summary.cycleQueries, 0u);
    EXPECT_EQ(summary.cycleQueries, summary.cpuQueries);
    EXPECT_EQ(m_result.draws, (std::vector<int>{71}));
}

TEST_F(RenderCpuTraceRhiTest, PumpReentryPreservesOrderAndDoesNotDuplicateLeafWaits)
{
    ASSERT_TRUE(renderer::cpu_trace::ConfigureForTesting(1, 1, 1));
    m_result.blockFirst = true;
    m_result.tryDispatchResult = true;
    m_bundle.renderer->BeginCpuFrame(31);
    m_bundle.renderer->BeginFrame();
    Draw(31);
    m_bundle.renderer->EndFrame();
    m_result.entered.acquire();
    m_bundle.renderer->BeginCpuFrame(32);
    m_bundle.renderer->BeginFrame();
    Draw(47);
    ReentrantWaitPumpState state{.renderer = m_bundle.renderer.get(), .resources = m_resources.get(),
        .resume = &m_result.resume, .resumed = &m_resumed};
    s_reentrantWaitPumpState = &state;
    m_bundle.renderer->SetRenderWaitPump(&ReentrantSyncWaitPump);
    const bool result = m_bundle.renderer->TryDispatch({}, *m_resources);
    m_bundle.renderer->SetRenderWaitPump(nullptr);
    s_reentrantWaitPumpState = nullptr;
    Draw(59);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_TRUE(result);
    EXPECT_TRUE(state.invoked);
    EXPECT_TRUE(state.nestedResult);
    EXPECT_EQ(m_result.operations,
        (std::vector<std::string>{"draw:31", "draw:47", "sync:47", "sync:47", "draw:59"}));
    const auto summary = renderer::cpu_trace::SnapshotSummary();
    EXPECT_GT(summary.notReady, 0u);
    EXPECT_GT(summary.pumpCalls, 0u);
    EXPECT_LE(summary.waitLeafNs, summary.waitInclusiveNs);
    EXPECT_TRUE(summary.leafAvailable);
    EXPECT_TRUE(summary.complete);
    EXPECT_EQ(summary.packets, summary.completedPackets);
}

class RenderCpuTraceThreadTest : public RenderThreadTest {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(renderer::cpu_trace::ConfigureForTesting(0, 1, 2));
        RenderThreadTest::SetUp();
    }
    void TearDown() override
    {
        RenderThreadTest::TearDown();
        EXPECT_TRUE(renderer::cpu_trace::DisableForTesting());
    }
};

TEST_F(RenderCpuTraceThreadTest, ThreadStorageReuseIssuesANewLifetimeAndCompletionIncludesDestruction)
{
    std::optional<renderer::RenderThread> worker;
    worker.emplace();
    renderer::cpu_trace::BeginFrame(&*worker, 41);
    renderer::cpu_trace::BindPacket(&*worker, 1, 1);
    ASSERT_TRUE(worker->Submit(1, std::make_unique<OwnedRenderFrame>(std::vector<int>{13}, m_result)));
    renderer::cpu_trace::PacketAccepted(&*worker, 1, true);
    m_result->entered.acquire();
    Resume();
    worker->WaitIdle();
    EXPECT_TRUE(m_result->destroyed.load());
    worker.reset();
    const auto first = renderer::cpu_trace::SnapshotPacket(0);
    auto secondResult = std::make_shared<RenderFrameResult>();
    worker.emplace();
    renderer::cpu_trace::BeginFrame(&*worker, 42);
    renderer::cpu_trace::BindPacket(&*worker, 1, 1);
    ASSERT_TRUE(worker->Submit(1, std::make_unique<OwnedRenderFrame>(std::vector<int>{17}, secondResult)));
    renderer::cpu_trace::PacketAccepted(&*worker, 1, true);
    secondResult->entered.acquire();
    secondResult->resume.release();
    worker->Stop();
    const auto second = renderer::cpu_trace::SnapshotPacket(1);
    EXPECT_NE(first.threadLifetimeId, second.threadLifetimeId);
    EXPECT_EQ(first.serial, second.serial);
    EXPECT_TRUE(first.complete);
    EXPECT_TRUE(second.complete);
    EXPECT_TRUE(secondResult->destroyed.load());
    EXPECT_EQ(secondResult->values, (std::vector<int>{17}));
    worker.reset();
    EXPECT_TRUE(renderer::cpu_trace::SnapshotSummary().complete);
}

TEST_F(RenderCpuTraceRhiTest, PacketCapacityAndUnavailableCpuAreExplicit)
{
    ASSERT_TRUE(renderer::cpu_trace::ConfigureForTesting(0, 1, 1, 1, renderer::cpu_trace::TestCpuMode::UNAVAILABLE));
    m_bundle.renderer->BeginCpuFrame(51);
    {
        renderer::cpu_trace::ViewScope view(1, 2, 3, 4, 5, 6);
        Draw(83);
        EXPECT_FALSE(m_bundle.renderer->TryDispatch({}, *m_resources));
        Draw(89);
        EXPECT_FALSE(m_bundle.renderer->TryDispatch({}, *m_resources));
    }
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    const auto summary = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(summary.packets, 1u);
    EXPECT_EQ(summary.completedPackets, 1u);
    EXPECT_GT(summary.dropped, 0u);
    EXPECT_FALSE(summary.complete);
    EXPECT_FALSE(summary.cpuAvailable);
    EXPECT_FALSE(summary.cyclesAvailable);
    EXPECT_EQ(m_result.draws, (std::vector<int>{83, 89}));
}

TEST_F(RenderCpuTraceRhiTest, RegressingCpuCountersAndExcessWaitDepthStayUnavailable)
{
    ASSERT_TRUE(renderer::cpu_trace::ConfigureForTesting(0, 1, 1, renderer::cpu_trace::MAX_PACKETS,
        renderer::cpu_trace::TestCpuMode::REGRESSING));
    m_bundle.renderer->BeginCpuFrame(61);
    {
        renderer::cpu_trace::ViewScope view(1, 2, 3, 4, 5, 6);
        const auto nested = [](auto&& self, uint32_t depth) -> void {
            const renderer::cpu_trace::WaitScope wait(true);
            if (depth) self(self, depth - 1);
        };
        nested(nested, renderer::cpu_trace::MAX_REENTRY);
    }
    m_bundle.renderer->EndFrame();
    const auto summary = renderer::cpu_trace::SnapshotSummary();
    EXPECT_FALSE(summary.cpuAvailable);
    EXPECT_FALSE(summary.cyclesAvailable);
    EXPECT_FALSE(summary.leafAvailable);
    EXPECT_FALSE(summary.complete);
    EXPECT_GT(summary.dropped, 0u);
}

TEST_F(RenderCpuTraceThreadTest, CsvRequiresStopAndRetriesWriteFailureWithoutInventingRejectedExecution)
{
    testkit::TempDir temporary("render-cpu-csv");
    ASSERT_TRUE(temporary.IsValid());
    const auto blockedParent = temporary.File("blocked-parent");
    ASSERT_TRUE(util::FileSystem::WriteText(blockedParent.string(), "not a directory"));
    const auto output = (blockedParent / "trace.csv").string();
    ASSERT_TRUE(renderer::cpu_trace::ConfigureForTesting(0, 1, 1, renderer::cpu_trace::MAX_PACKETS,
        renderer::cpu_trace::TestCpuMode::UNAVAILABLE, output.c_str()));
    renderer::cpu_trace::BeginFrame(m_thread.get(), 71);
    renderer::cpu_trace::BindPacket(m_thread.get(), 1, 1);
    ASSERT_TRUE(m_thread->Submit(1, std::make_unique<OwnedRenderFrame>(std::vector<int>{97}, m_result)));
    renderer::cpu_trace::PacketAccepted(m_thread.get(), 1, true);
    m_result->entered.acquire();
    EXPECT_FALSE(renderer::cpu_trace::WriteCsvAfterStop());
    EXPECT_FALSE(util::FileSystem::Exists(output));
    Resume();
    m_thread->Stop();
    EXPECT_TRUE(m_result->destroyed.load());
    renderer::cpu_trace::BindPacket(m_thread.get(), 2, 1);
    const bool accepted = m_thread->Submit(2, std::make_unique<OwnedRenderFrame>(std::vector<int>{}, m_result));
    EXPECT_FALSE(accepted);
    renderer::cpu_trace::PacketAccepted(m_thread.get(), 2, accepted);
    EXPECT_FALSE(renderer::cpu_trace::WriteCsvAfterStop());
    std::error_code removeError;
    ASSERT_TRUE(std::filesystem::remove(blockedParent, removeError));
    ASSERT_FALSE(removeError);
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(blockedParent));
    ASSERT_TRUE(renderer::cpu_trace::WriteCsvAfterStop());
    EXPECT_FALSE(renderer::cpu_trace::WriteCsvAfterStop());
    std::string csv;
    ASSERT_TRUE(util::FileSystem::ReadText(output, csv));
    EXPECT_NE(csv.find("parentViewOrdinal"), std::string::npos);
    EXPECT_NE(csv.find("packet-execute"), std::string::npos);
    EXPECT_NE(csv.find("packet-destroy"), std::string::npos);
    bool rejectedExecute = false, unavailableCpu = false, unavailableCycles = false, rejectedCycles = false;
    std::istringstream input(csv);
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("packet-execute,") && line.find(",wall-ns,") != std::string::npos
            && line.ends_with(",0,0,0,2,1,0,1,0,0")) rejectedExecute = true;
        if (line.starts_with("packet-execute,") && line.find(",kernel-100ns,") != std::string::npos
            && line.ends_with(",0,0,0,1,1,1,1,0,0")) unavailableCpu = true;
        if (line.starts_with("packet-execute,") && line.find(",cpu-cycles,") != std::string::npos
            && line.ends_with(",0,0,0,1,1,1,1,0,0")) unavailableCycles = true;
        if (line.starts_with("packet-execute,") && line.find(",cpu-cycles,") != std::string::npos
            && line.ends_with(",0,0,0,2,1,0,1,0,0")) rejectedCycles = true;
    }
    EXPECT_TRUE(rejectedExecute);
    EXPECT_TRUE(unavailableCpu);
    EXPECT_TRUE(unavailableCycles);
    EXPECT_TRUE(rejectedCycles);
    EXPECT_NE(csv.find("cycle-queries"), std::string::npos);
    const auto summary = renderer::cpu_trace::SnapshotSummary();
    EXPECT_TRUE(summary.complete);
    EXPECT_EQ(summary.packets, 2u);
    EXPECT_EQ(summary.completedPackets, 1u);
    EXPECT_FALSE(summary.cpuAvailable);
    EXPECT_FALSE(summary.cyclesAvailable);
}

/// @pre The executor is causally idle; only its mutex handoff or permitted spurious try-lock failure may reject.
/// @note A bounded scheduler yield retries unchanged state without sleeps or elapsed-time expectations.
renderer::RenderThread::SubmissionReservation ReserveIdleSubmission(renderer::RenderThread& thread, uint64_t serial)
{
    for (uint32_t attempt = 0; attempt < 65536; ++attempt) {
        auto reservation = thread.TryReserveSubmission(serial);
        if (reservation.IsValid()) return reservation;
        std::this_thread::yield();
    }
    return {};
}

/// @pre Pending transfers are eligible and the previous packet was causally drained or none was submitted.
/// @note Failed tries do not publish or advance serial; negative contract tests use a single call instead.
bool SubmitIdleResourceUpdates(renderer::IRenderer& renderer)
{
    for (uint32_t attempt = 0; attempt < 65536; ++attempt) {
        if (renderer::TrySubmitPendingResourceUpdates(renderer)) return true;
        std::this_thread::yield();
    }
    return false;
}

TEST_F(RenderThreadTest, NonblockingReservationRejectsBusyAndPreservesUnpublishedSerial)
{
    ASSERT_TRUE(m_thread->Submit(1, std::make_unique<OwnedRenderFrame>(std::vector<int>{11}, m_result)));
    m_result->entered.acquire();
    EXPECT_FALSE(m_thread->TryReserveSubmission(2).IsValid());
    Resume();
    ASSERT_TRUE(m_thread->WaitUntilCompleted(1));
    {
        auto original = ReserveIdleSubmission(*m_thread, 2);
        ASSERT_TRUE(original.IsValid());
        auto moved = std::move(original);
        EXPECT_FALSE(original.IsValid());
        EXPECT_TRUE(moved.IsValid());
        EXPECT_FALSE(moved.Publish({}));
        EXPECT_TRUE(moved.IsValid());
    }
    EXPECT_FALSE(m_thread->WaitUntilCompleted(2));
    auto nextResult = std::make_shared<RenderFrameResult>();
    nextResult->resume.release();
    auto reserved = ReserveIdleSubmission(*m_thread, 2);
    ASSERT_TRUE(reserved.IsValid());
    EXPECT_TRUE(reserved.Publish(std::make_unique<OwnedRenderFrame>(std::vector<int>{17}, nextResult)));
    EXPECT_FALSE(reserved.IsValid());
    ASSERT_TRUE(m_thread->WaitUntilCompleted(2));
    EXPECT_EQ(nextResult->values, (std::vector<int>{17}));
    EXPECT_TRUE(nextResult->destroyed.load());
    EXPECT_FALSE(m_thread->TryReserveSubmission(2).IsValid());
    m_thread->Stop();
    EXPECT_FALSE(m_thread->TryReserveSubmission(3).IsValid());
}

TEST_F(ThreadedRhiTest, EarlyTransfersOwnInputsPreserveAllLateWritersAndKeepFrameOpen)
{
    const std::array<int, 4> initial{1, 2, 3, 4};
    const auto buffer = m_resources->CreateVertexBuffer(initial.data(), sizeof(initial), sizeof(int));
    ASSERT_TRUE(buffer.IsValid());
    m_bundle.renderer->BeginCpuFrame(1);
    for (int value : {11, 23, 37, 53}) {
        m_resources->Update(m_constants, &value, sizeof(value));
        value = 99;
    }
    auto batch = std::make_shared<renderer::ConstantBufferUpdateBatch>();
    const int lastWriter = 61;
    batch->bytes.resize(sizeof(lastWriter));
    std::memcpy(batch->bytes.data(), &lastWriter, sizeof(lastWriter));
    batch->entries.push_back({m_constants, 0, sizeof(lastWriter)});
    m_resources->UpdateConstantBuffers(batch);
    batch.reset();
    std::array<int, 4> vertices{7, 11, 13, 17};
    m_resources->Update(buffer, vertices.data(), sizeof(vertices));
    vertices.fill(99);
    ASSERT_TRUE(SubmitIdleResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.transfers, (std::vector<int>{11, 23, 37, 53, 61}));
    std::array<int, 4> copied{};
    const auto* native = m_resources->Get(buffer);
    ASSERT_NE(native, nullptr);
    ASSERT_TRUE(native->CopyData(0, sizeof(copied), copied.data()));
    EXPECT_EQ(copied, (std::array<int, 4>{7, 11, 13, 17}));
    m_bundle.renderer->BeginFrame();
    renderer::DrawCall call;
    call.constantBuffers[0] = m_constants;
    m_bundle.renderer->Submit(call, *m_resources);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.draws, (std::vector<int>{61}));
}

TEST_F(ThreadedRhiTest, StructuredOpaqueAndBeginFrameQueuesStayInOriginalFallbackOrder)
{
    const auto structured = m_resources->CreateStructuredBuffer(nullptr, 1, sizeof(int));
    ASSERT_TRUE(structured.IsValid());
    TestRhi unknown(m_result);
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(unknown));
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->BeginCpuFrame(1);
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    int value = 19;
    m_resources->Update(m_constants, &value, sizeof(value));
    value = 29;
    m_resources->Update(structured, &value, sizeof(value));
    value = 39;
    m_resources->Update(m_constants, &value, sizeof(value));
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.structuredValue, 29);
    EXPECT_EQ(m_result.transfers, (std::vector<int>{19, 39}));
    value = 43;
    m_resources->Update(m_constants, &value, sizeof(value));
    ASSERT_TRUE(m_bundle.renderer->RecordResourceUpdate([this] {
        const int value = 47;
        m_resources->Update(m_constants, &value, sizeof(value));
    }));
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.currentValue, 47);
    value = 59;
    m_resources->Update(m_constants, &value, sizeof(value));
    m_bundle.renderer->BeginFrame();
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    renderer::DrawCall call;
    call.constantBuffers[0] = m_constants;
    m_bundle.renderer->Submit(call, *m_resources);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.draws, (std::vector<int>{59}));
}

TEST_F(ThreadedRhiTest, ResourceClassificationCannotLeakToAnotherFacadeOrNativeReentry)
{
    RhiResult probeResult;
    TransferProbeRhi probe(probeResult);
    renderer::ResourceManager probeResources(probe);
    const auto probeCb = probeResources.CreateConstantBuffer(sizeof(int));
    ASSERT_TRUE(probeCb.IsValid());
    m_bundle.renderer->BeginCpuFrame(1);
    bool publicAccepted = false;
    probe.onRecord = [this, &publicAccepted] {
        const int known = 7;
        m_resources->Update(m_constants, &known, sizeof(known));
        std::function<void()> opaque = [this] {
            const int value = 11;
            m_resources->Update(m_constants, &value, sizeof(value));
        };
        publicAccepted = m_bundle.renderer->RecordResourceUpdate(std::move(opaque));
    };
    const int probeValue = 13;
    probeResources.Update(probeCb, &probeValue, sizeof(probeValue));
    EXPECT_TRUE(publicAccepted);
    EXPECT_EQ(probeResult.currentValue, 13);
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(probe));
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.transfers, (std::vector<int>{7, 11}));

    RhiResult secondResult;
    renderer::RendererBundle native{std::make_unique<TestRhi>(secondResult), std::make_unique<TestGui>(secondResult)};
    auto second = renderer::EnableRenderThread(std::move(native));
    renderer::ResourceManager secondResources(*second.renderer);
    const auto secondCb = secondResources.CreateConstantBuffer(sizeof(int));
    second.renderer->BeginCpuFrame(1);
    bool workerAccepted = true, opaqueRan = false;
    m_result.transferProbe = [this, &secondResources, secondCb, &workerAccepted, &opaqueRan] {
        workerAccepted = m_bundle.renderer->RecordResourceUpdate([&opaqueRan] { opaqueRan = true; });
        const int nestedValue = 23;
        m_resources->Update(m_constants, &nestedValue, sizeof(nestedValue));
        const int otherValue = 29;
        secondResources.Update(secondCb, &otherValue, sizeof(otherValue));
    };
    const int firstValue = 19;
    m_resources->Update(m_constants, &firstValue, sizeof(firstValue));
    ASSERT_TRUE(SubmitIdleResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_FALSE(workerAccepted);
    EXPECT_FALSE(opaqueRan);
    ASSERT_TRUE(SubmitIdleResourceUpdates(*second.renderer));
    second.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.transfers, (std::vector<int>{7, 11, 19, 23}));
    EXPECT_EQ(secondResult.transfers, (std::vector<int>{29}));
    second.renderer->CancelCpuFrame();
    m_bundle.renderer->CancelCpuFrame();
}

struct EarlySubmitPumpState {
    renderer::IRenderer* renderer = nullptr;
    renderer::ResourceManager* resources = nullptr;
    renderer::ResourceHandle<renderer::ConstantBufferTag> constants;
    std::binary_semaphore* resume = nullptr;
    bool* resumed = nullptr;
    bool invoked = false, attemptResult = true, append = false;
};
thread_local EarlySubmitPumpState* s_earlySubmitPump = nullptr;

void EarlySubmitWaitPump()
{
    auto* state = s_earlySubmitPump;
    if (!state || state->invoked) return;
    state->invoked = true;
    *state->resumed = true;
    state->resume->release();
    if (!state->append) return;
    state->renderer->WaitForCpuFrame();
    const int value = 59;
    state->resources->Update(state->constants, &value, sizeof(value));
    state->attemptResult = renderer::TrySubmitPendingResourceUpdates(*state->renderer);
}

TEST_F(ThreadedRhiTest, EarlySubmitRejectsOuterSyncReentryAfterNestedDrainMakesWorkerIdle)
{
    m_result.blockFirst = true;
    m_result.tryDispatchResult = true;
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(31);
    m_bundle.renderer->EndFrame();
    m_result.entered.acquire();
    m_bundle.renderer->BeginCpuFrame(2);
    const int value = 47;
    m_resources->Update(m_constants, &value, sizeof(value));
    EarlySubmitPumpState state{.renderer = m_bundle.renderer.get(), .resources = m_resources.get(),
        .constants = m_constants, .resume = &m_result.resume, .resumed = &m_resumed, .append = true};
    s_earlySubmitPump = &state;
    m_bundle.renderer->SetRenderWaitPump(&EarlySubmitWaitPump);
    const bool result = m_bundle.renderer->TryDispatch({}, *m_resources);
    m_bundle.renderer->SetRenderWaitPump(nullptr);
    s_earlySubmitPump = nullptr;
    EXPECT_TRUE(result);
    EXPECT_TRUE(state.invoked);
    EXPECT_FALSE(state.attemptResult);
    EXPECT_EQ(m_result.transfers, (std::vector<int>{31, 47, 59}));
    EXPECT_EQ(m_result.operations, (std::vector<std::string>{"draw:31", "sync:59"}));
    m_bundle.renderer->CancelCpuFrame();
}

TEST_F(ThreadedRhiTest, CancelDiscardsPendingUpdatesAndNextFrameReusesCommittedValue)
{
    m_result.blockTransferNumber = 1;
    m_bundle.renderer->BeginCpuFrame(1);
    int value = 40;
    m_resources->Update(m_constants, &value, sizeof(value));
    ASSERT_TRUE(SubmitIdleResourceUpdates(*m_bundle.renderer));
    m_result.entered.acquire();
    value = 99;
    m_resources->Update(m_constants, &value, sizeof(value));
    m_bundle.renderer->CancelCpuFrame();
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_resumed = true;
    m_result.resume.release();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.transfers, (std::vector<int>{40}));
    m_bundle.renderer->BeginCpuFrame(2);
    m_bundle.renderer->BeginFrame();
    renderer::DrawCall call;
    call.constantBuffers[0] = m_constants;
    m_bundle.renderer->Submit(call, *m_resources);
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.draws, (std::vector<int>{40}));
}

TEST_F(ThreadedRhiTest, ReleaseResetAndShutdownDrainCommittedTransfersBeforeResourceDestruction)
{
    for (int iteration = 0; iteration < 3; ++iteration) {
        if (iteration != 0) m_constants = m_resources->CreateConstantBuffer(sizeof(int));
        m_result.blockTransferNumber = static_cast<size_t>(iteration) + 1;
        m_resumed = false;
        m_bundle.renderer->BeginCpuFrame(static_cast<uint64_t>(iteration) + 1);
        const int value = 63 + iteration * 10;
        m_resources->Update(m_constants, &value, sizeof(value));
        ASSERT_TRUE(SubmitIdleResourceUpdates(*m_bundle.renderer));
        m_result.entered.acquire();
        EarlySubmitPumpState state{.renderer = m_bundle.renderer.get(), .resume = &m_result.resume, .resumed = &m_resumed};
        s_earlySubmitPump = &state;
        m_bundle.renderer->SetRenderWaitPump(&EarlySubmitWaitPump);
        if (iteration == 0) m_resources->Release(m_constants);
        else if (iteration == 1) m_resources->Reset();
        else m_resources->PrepareRendererShutdown();
        m_bundle.renderer->SetRenderWaitPump(nullptr);
        s_earlySubmitPump = nullptr;
        EXPECT_TRUE(state.invoked);
        ASSERT_EQ(m_result.releasedValues.size(), static_cast<size_t>(iteration) + 1);
        EXPECT_EQ(m_result.releasedValues.back(), value);
        EXPECT_EQ(m_resources->Get(m_constants), nullptr);
        m_bundle.renderer->CancelCpuFrame();
    }
    m_bundle.renderer->Shutdown();
    EXPECT_EQ(m_result.transfers, (std::vector<int>{63, 73, 83}));
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
}

TEST_F(RenderCpuTraceRhiTest, BusyEarlyAttemptKeepsPendingTransferAndCreatesNoTracePacket)
{
    ASSERT_TRUE(renderer::cpu_trace::ConfigureForTesting(0, 1, 2));
    m_result.blockFirst = true;
    m_bundle.renderer->BeginCpuFrame(51);
    m_bundle.renderer->BeginFrame();
    Draw(31);
    m_bundle.renderer->EndFrame();
    m_result.entered.acquire();
    m_bundle.renderer->BeginCpuFrame(52);
    const int value = 47;
    m_resources->Update(m_constants, &value, sizeof(value));
    const auto before = renderer::cpu_trace::SnapshotSummary();
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    const auto rejected = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(rejected.packets, before.packets);
    EXPECT_EQ(rejected.dropped, before.dropped);
    EXPECT_EQ(m_result.currentValue, 31);
    m_resumed = true;
    m_result.resume.release();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.transfers, (std::vector<int>{31, 47}));
    const int finalValue = 61;
    m_resources->Update(m_constants, &finalValue, sizeof(finalValue));
    ASSERT_TRUE(SubmitIdleResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    const auto packet = renderer::cpu_trace::SnapshotPacket(2);
    EXPECT_EQ(packet.applicationFrame, 52u);
    EXPECT_EQ(packet.serial, renderer::cpu_trace::SnapshotPacket(1).serial + 1);
    EXPECT_TRUE(packet.accepted);
    EXPECT_TRUE(packet.complete);
    EXPECT_TRUE(renderer::cpu_trace::SnapshotSummary().complete);
    EXPECT_EQ(m_result.transfers, (std::vector<int>{31, 47, 61}));
    m_bundle.renderer->CancelCpuFrame();
}

TEST_F(ThreadedRhiTest, RegistryCapacityFallsBackAndReleasedSlotCanBeRegisteredAgain)
{
    std::array<RhiResult, 16> results;
    std::vector<renderer::RendererBundle> bundles;
    std::vector<std::unique_ptr<renderer::ResourceManager>> resources;
    for (size_t i = 0; i < results.size(); ++i) {
        renderer::RendererBundle native{std::make_unique<TestRhi>(results[i]), std::make_unique<TestGui>(results[i])};
        bundles.push_back(renderer::EnableRenderThread(std::move(native)));
        resources.push_back(std::make_unique<renderer::ResourceManager>(*bundles.back().renderer));
        const auto cb = resources.back()->CreateConstantBuffer(sizeof(int));
        bundles.back().renderer->BeginCpuFrame(1);
        const int value = static_cast<int>(i) + 1;
        resources.back()->Update(cb, &value, sizeof(value));
        const bool early = i < 15 ? SubmitIdleResourceUpdates(*bundles.back().renderer)
            : renderer::TrySubmitPendingResourceUpdates(*bundles.back().renderer);
        EXPECT_EQ(early, i < 15);
        bundles.back().renderer->WaitForCpuFrame();
        EXPECT_EQ(results[i].currentValue, value);
        bundles.back().renderer->CancelCpuFrame();
    }
    resources.front().reset();
    bundles.front().imguiRenderer.reset();
    bundles.front().renderer.reset();
    RhiResult replacementResult;
    renderer::RendererBundle native{std::make_unique<TestRhi>(replacementResult), std::make_unique<TestGui>(replacementResult)};
    auto replacement = renderer::EnableRenderThread(std::move(native));
    renderer::ResourceManager replacementResources(*replacement.renderer);
    const auto cb = replacementResources.CreateConstantBuffer(sizeof(int));
    replacement.renderer->BeginCpuFrame(1);
    const int value = 73;
    replacementResources.Update(cb, &value, sizeof(value));
    EXPECT_TRUE(SubmitIdleResourceUpdates(*replacement.renderer));
    replacement.renderer->WaitForCpuFrame();
    EXPECT_EQ(replacementResult.currentValue, value);
    replacement.renderer->CancelCpuFrame();
}

TEST_F(RenderCpuTraceRhiTest, OpaqueOriginCsvIdentifiesEditorPreludeAndTransferPrefixWithoutAllowingEarlySubmission)
{
    InitGui();
    testkit::TempDir temporary("opaque-queue-origin");
    ASSERT_TRUE(temporary.IsValid());
    const auto output = temporary.File("trace.csv").string();
    ASSERT_TRUE(renderer::cpu_trace::ConfigureForTesting(0, 1, 2,
        renderer::cpu_trace::MAX_PACKETS, renderer::cpu_trace::TestCpuMode::NATIVE, output.c_str()));
    m_bundle.renderer->BeginCpuFrame(101);
    m_bundle.imguiRenderer->ImGuiNewFrame();
    int value = 17;
    m_resources->Update(m_constants, &value, sizeof(value));
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    m_bundle.renderer->BeginFrame();
    m_bundle.renderer->WaitForCpuFrame();
    value = 19;
    m_resources->Update(m_constants, &value, sizeof(value));
    m_bundle.renderer->GpuProfBeginFrame();
    m_bundle.renderer->GpuProfCollect();
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    Draw(23);
    m_bundle.renderer->GpuProfEndFrame();
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();

    m_bundle.renderer->BeginCpuFrame(102);
    m_bundle.renderer->BeginFrame();
    m_bundle.renderer->WaitForCpuFrame();
    value = 29;
    m_resources->Update(m_constants, &value, sizeof(value));
    ASSERT_TRUE(m_bundle.renderer->RecordResourceUpdate([this] {
        const int value = 31;
        m_resources->Update(m_constants, &value, sizeof(value));
    }));
    ASSERT_TRUE(m_bundle.renderer->RecordResourceUpdate([this] {
        const int value = 37;
        m_resources->Update(m_constants, &value, sizeof(value));
    }));
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->WaitForCpuFrame();
    m_bundle.renderer->ClearDepth();
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.transfers, (std::vector<int>{17, 19, 23, 29, 31, 37}));
    EXPECT_EQ(m_result.draws, (std::vector<int>{23}));
    EXPECT_TRUE(renderer::cpu_trace::SnapshotSummary().complete);
    m_bundle.imguiRenderer->ImGuiShutdown();
    m_resources->PrepareRendererShutdown();
    m_bundle.renderer->Shutdown();

    std::string csv;
    ASSERT_TRUE(util::FileSystem::ReadText(output, csv));
    const std::array<std::pair<const char*, size_t>, 5> expected{{
        {"OpaqueQueue::ImGuiGpuPrepare::AtFront", 1}, {"OpaqueQueue::GpuProfiler::AfterPending", 1},
        {"OpaqueQueue::BeginFrame::AtFront", 2}, {"OpaqueQueue::ResourceUpdate::AfterPending", 1},
        {"OpaqueQueue::Other::AtFront", 1}}};
    for (const auto& [api, expectedCount] : expected) {
        std::istringstream rows(csv);
        std::string row;
        size_t matches = 0;
        while (std::getline(rows, row))
            if (row.starts_with("counter,") && row.find(api) != std::string::npos
                && row.find(",source-operations,1,") != std::string::npos) ++matches;
        EXPECT_EQ(matches, expectedCount) << api;
    }
}

TEST_F(RenderCpuTraceRhiTest, OpaqueOriginCountersUseNoClocksAndStayDisabledWithoutTrace)
{
    InitGui();
    m_bundle.renderer->BeginCpuFrame(103);
    const auto before = renderer::cpu_trace::SnapshotSummary();
    m_bundle.imguiRenderer->ImGuiNewFrame();
    const auto counted = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(counted.wallQueries, before.wallQueries);
    EXPECT_EQ(counted.cpuQueries, before.cpuQueries);
    EXPECT_EQ(counted.cycleQueries, before.cycleQueries);
    EXPECT_EQ(counted.packets, before.packets);
    m_bundle.renderer->CancelCpuFrame();
    ASSERT_TRUE(renderer::cpu_trace::DisableForTesting());
    m_bundle.renderer->BeginCpuFrame(104);
    m_bundle.imguiRenderer->ImGuiNewFrame();
    m_bundle.renderer->BeginFrame();
    Draw(43);
    EXPECT_FALSE(renderer::TrySubmitPendingResourceUpdates(*m_bundle.renderer));
    m_bundle.renderer->EndFrame();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_EQ(m_result.draws, (std::vector<int>{43}));
    const auto disabled = renderer::cpu_trace::SnapshotSummary();
    EXPECT_EQ(disabled.selectedFrames, 0u);
    EXPECT_EQ(disabled.packets, 0u);
    EXPECT_EQ(disabled.wallQueries, 0u);
    EXPECT_EQ(disabled.cpuQueries, 0u);
    EXPECT_EQ(disabled.cycleQueries, 0u);
}


struct MaterialTransferObservation {
    uint32_t owner = 0;
    const void* source = nullptr;
    std::vector<uint8_t> bytes;
};

struct MaterialTransferResult {
    uint32_t nextOwner = 1;
    std::atomic<uint32_t> textureIndex{17};
    std::vector<MaterialTransferObservation> updates, draws;
    std::binary_semaphore entered{0}, resume{0};
};

struct MaterialTransferBufferState {
    uint32_t owner = 0;
    std::vector<uint8_t> bytes;
};

class MaterialTransferTestBuffer final : public renderer::IConstantBuffer {
public:
    MaterialTransferTestBuffer(MaterialTransferResult& result, std::shared_ptr<MaterialTransferBufferState> state)
        : m_result(result), m_state(std::move(state)) {}
    void Update(const void* data, size_t size) override
    {
        if (!data || size > m_state->bytes.size()) return;
        std::memcpy(m_state->bytes.data(), data, size);
        std::fill(m_state->bytes.begin() + size, m_state->bytes.end(), 0u);
        m_result.updates.push_back({m_state->owner, data, m_state->bytes});
    }
    size_t GetSize() const override { return m_state->bytes.size(); }
private:
    MaterialTransferResult& m_result;
    std::shared_ptr<MaterialTransferBufferState> m_state;
};

class MaterialTransferTestTexture final : public renderer::ITexture {
public:
    explicit MaterialTransferTestTexture(MaterialTransferResult& result) : m_result(result) {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    uint32_t GetBindlessIndex() const override { return m_result.textureIndex.load(); }
private:
    MaterialTransferResult& m_result;
};

class MaterialTransferTestRhi : public TestRhi {
public:
    MaterialTransferTestRhi(RhiResult& legacy, MaterialTransferResult& result) : TestRhi(legacy), m_result(result) {}
    void Submit(const renderer::DrawCall& call, renderer::ResourceManager& resources) override
    {
        auto* buffer = resources.Get(call.constantBuffers[0]);
        const auto found = m_buffers.find(buffer);
        if (found != m_buffers.end())
            m_result.draws.push_back({found->second->owner, nullptr, found->second->bytes});
    }
private:
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override
    {
        auto state = std::make_shared<MaterialTransferBufferState>();
        state->owner = m_result.nextOwner++;
        state->bytes.assign(size, 0u);
        auto buffer = std::make_unique<MaterialTransferTestBuffer>(m_result, state);
        m_buffers.insert_or_assign(buffer.get(), std::move(state));
        return buffer;
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override
    { return std::make_unique<MaterialTransferTestTexture>(m_result); }
    MaterialTransferResult& m_result;
    std::unordered_map<const renderer::IConstantBuffer*, std::shared_ptr<MaterialTransferBufferState>> m_buffers;
};

class RejectingMaterialTransferRhi final : public MaterialTransferTestRhi {
public:
    RejectingMaterialTransferRhi(RhiResult& legacy, MaterialTransferResult& result)
        : MaterialTransferTestRhi(legacy, result) {}
    bool IsRenderThread() const override { return m_executing; }
    void ExecuteResourceOperation(const std::function<void()>& operation) override
    {
        const ExecutionScope execution(m_executing);
        operation();
    }
    bool RecordResourceUpdate(std::function<void()>) override
    {
        ++recordCalls;
        if (onRecord) onRecord();
        return false;
    }
    size_t recordCalls = 0;
    std::function<void()> onRecord;
private:
    /// @note ResourceManager recursively dispatches release/shutdown until this adapter reports execution ownership.
    class ExecutionScope final {
    public:
        explicit ExecutionScope(bool& executing) : m_executing(executing), m_previous(std::exchange(executing, true)) {}
        ~ExecutionScope() { m_executing = m_previous; }
    private:
        bool& m_executing;
        bool m_previous;
    };
    bool m_executing = false;
};

thread_local std::function<void()>* s_materialTransferWaitPump = nullptr;

void MaterialTransferWaitPump()
{
    if (auto* callback = std::exchange(s_materialTransferWaitPump, nullptr)) (*callback)();
}

class MaterialTransferPayloadTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        renderer::RendererBundle native{std::make_unique<MaterialTransferTestRhi>(m_legacy, m_result),
            std::make_unique<TestGui>(m_legacy)};
        m_bundle = renderer::EnableRenderThread(std::move(native));
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        m_descriptor.cbufferSize = 32;
    }
    void TearDown() override
    {
        s_materialTransferWaitPump = nullptr;
        m_bundle.renderer->SetRenderWaitPump(nullptr);
        m_bundle.renderer->CancelCpuFrame();
        m_bundle.renderer->WaitForCpuFrame();
        m_resources->Reset();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        m_bundle.renderer.reset();
        testkit::Fixture::TearDown();
    }
    void Initialize(renderer::Material& material, uint8_t value = 37)
    {
        material.Init(*m_resources, m_descriptor.cbufferSize);
        material.paramData.assign(m_descriptor.cbufferSize, value);
    }
    void Draw(const renderer::Material& material, renderer::ResourceManager& resources)
    {
        renderer::DrawCall call;
        call.constantBuffers[0] = material.paramsBuffer;
        m_bundle.renderer->Submit(call, resources);
    }
    void Complete()
    {
        m_bundle.renderer->EndFrame();
        m_bundle.renderer->WaitForCpuFrame();
    }
    RhiResult m_legacy;
    MaterialTransferResult m_result;
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ShaderDescriptor m_descriptor;
};

TEST_F(MaterialTransferPayloadTest, SharesImmutableFinalPayloadWhileKeepingOwnersAndOrderedDraws)
{
    renderer::Material first, second;
    Initialize(first);
    Initialize(second);
    ASSERT_NE(first.paramsBuffer, second.paramsBuffer);
    const auto expected = first.paramData;
    m_bundle.renderer->BeginCpuFrame(1);
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope scope(*m_resources);
        first.Upload(*m_resources, m_descriptor);
        Draw(first, *m_resources);
        second.Upload(*m_resources, m_descriptor);
        Draw(second, *m_resources);
    }
    first.paramData.assign(2048, 99);
    first.paramData.clear();
    first.paramData.shrink_to_fit();
    second.paramData.assign(2048, 51);
    second.paramData.clear();
    second.paramData.shrink_to_fit();
    Complete();
    ASSERT_EQ(m_result.updates.size(), 2u);
    ASSERT_EQ(m_result.draws.size(), 2u);
    EXPECT_NE(m_result.updates[0].owner, m_result.updates[1].owner);
    /// @note Both queued closures still own their input when execution observes these addresses.
    EXPECT_EQ(m_result.updates[0].source, m_result.updates[1].source);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(m_result.updates[i].bytes, expected);
        EXPECT_EQ(m_result.draws[i].owner, m_result.updates[i].owner);
        EXPECT_EQ(m_result.draws[i].bytes, expected);
    }
}

TEST_F(MaterialTransferPayloadTest, RebuildsExactPublicBitsTextureMaskAndCurrentBindlessValues)
{
    renderer::Material first, second;
    Initialize(first, 0);
    Initialize(second, 0);
    const uint8_t pixel[4]{255, 255, 255, 255};
    const auto texture = m_resources->CreateTexture(pixel, 1, 1);
    ASSERT_TRUE(texture.IsValid());
    first.textures = {texture};
    second.textures = {texture};
    m_descriptor.textureMaskOffset = 16;
    m_descriptor.textures.push_back({"texAlbedoIndex", 0, 20});
    const std::array<uint32_t, 2> firstBits{0x00000000u, 0x7fc00001u};
    const std::array<uint32_t, 2> secondBits{0x80000000u, 0x7fc00002u};
    std::memcpy(first.paramData.data(), firstBits.data(), sizeof(firstBits));
    std::memcpy(second.paramData.data(), secondBits.data(), sizeof(secondBits));
    std::vector<std::vector<uint8_t>> expected;
    m_bundle.renderer->BeginCpuFrame(1);
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope scope(*m_resources);
        first.Upload(*m_resources, m_descriptor);
        expected.push_back(first.paramData);
        Draw(first, *m_resources);
        second.Upload(*m_resources, m_descriptor);
        expected.push_back(second.paramData);
        Draw(second, *m_resources);
        m_result.textureIndex.store(23);
        std::memcpy(first.paramData.data(), secondBits.data(), sizeof(secondBits));
        first.Upload(*m_resources, m_descriptor);
        expected.push_back(first.paramData);
        Draw(first, *m_resources);
        first.textures.clear();
        first.Upload(*m_resources, m_descriptor);
        expected.push_back(first.paramData);
        Draw(first, *m_resources);
    }
    Complete();
    ASSERT_EQ(m_result.updates.size(), 4u);
    ASSERT_EQ(m_result.draws.size(), 4u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(m_result.updates[i].bytes, expected[i]);
        EXPECT_EQ(m_result.draws[i].bytes, expected[i]);
    }
    EXPECT_NE(m_result.updates[0].source, m_result.updates[1].source);
    EXPECT_NE(m_result.updates[1].source, m_result.updates[2].source);
    EXPECT_NE(m_result.updates[2].source, m_result.updates[3].source);
    uint32_t firstMask = 0, lastMask = 1, firstIndex = 0, changedIndex = 0, lastIndex = 0;
    std::memcpy(&firstMask, expected[0].data() + 16, 4);
    std::memcpy(&lastMask, expected[3].data() + 16, 4);
    std::memcpy(&firstIndex, expected[0].data() + 20, 4);
    std::memcpy(&changedIndex, expected[2].data() + 20, 4);
    std::memcpy(&lastIndex, expected[3].data() + 20, 4);
    EXPECT_EQ(firstMask, 1u);
    EXPECT_EQ(lastMask, 0u);
    EXPECT_EQ(firstIndex, 17u);
    EXPECT_EQ(changedIndex, 23u);
    EXPECT_EQ(lastIndex, renderer::INVALID_BINDLESS_INDEX);
    m_resources->Release(texture);
}

TEST_F(MaterialTransferPayloadTest, CancellationResetAndStaleHandleKeepDestinationGenerationChecks)
{
    renderer::Material material;
    Initialize(material);
    const auto oldHandle = material.paramsBuffer;
    const auto expected = material.paramData;
    m_bundle.renderer->BeginCpuFrame(1);
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope scope(*m_resources);
        material.Upload(*m_resources, m_descriptor);
        Draw(material, *m_resources);
    }
    m_bundle.renderer->CancelCpuFrame();
    EXPECT_TRUE(m_result.updates.empty());
    EXPECT_TRUE(m_result.draws.empty());
    m_resources->Reset();
    Initialize(material);
    ASSERT_NE(material.paramsBuffer, oldHandle);
    m_bundle.renderer->BeginCpuFrame(2);
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope scope(*m_resources);
        m_resources->Update(oldHandle, expected.data(), expected.size());
        material.Upload(*m_resources, m_descriptor);
        Draw(material, *m_resources);
    }
    Complete();
    ASSERT_EQ(m_result.updates.size(), 1u);
    ASSERT_EQ(m_result.draws.size(), 1u);
    EXPECT_EQ(m_result.updates[0].bytes, expected);
    EXPECT_EQ(m_result.draws[0].bytes, expected);
}

TEST_F(MaterialTransferPayloadTest, NestedWaitPumpKeepsOuterSnapshotAndWorkerUsesRawSource)
{
    renderer::Material outer, inner, worker;
    Initialize(outer);
    Initialize(inner);
    Initialize(worker);
    bool reentered = false;
    m_bundle.renderer->BeginCpuFrame(1);
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope outerScope(*m_resources);
        outer.Upload(*m_resources, m_descriptor);
        std::function<void()> callback = [&] {
            renderer::material_transfer_detail::MaterialTransferPayloadScope innerScope(*m_resources);
            inner.Upload(*m_resources, m_descriptor);
            reentered = true;
            m_result.resume.release();
        };
        s_materialTransferWaitPump = &callback;
        m_bundle.renderer->SetRenderWaitPump(&MaterialTransferWaitPump);
        m_bundle.renderer->ExecuteResourceOperation([&] {
            renderer::material_transfer_detail::MaterialTransferPayloadScope workerScope(*m_resources);
            worker.Upload(*m_resources, m_descriptor);
            m_result.entered.release();
            m_result.resume.acquire();
        });
        m_bundle.renderer->SetRenderWaitPump(nullptr);
        ASSERT_TRUE(reentered);
        outer.Upload(*m_resources, m_descriptor);
    }
    Complete();
    ASSERT_EQ(m_result.updates.size(), 4u);
    EXPECT_EQ(m_result.updates[1].source, worker.paramData.data());
    EXPECT_EQ(m_result.updates[0].source, m_result.updates[3].source);
    EXPECT_NE(m_result.updates[0].source, m_result.updates[2].source);
    EXPECT_EQ(m_result.updates[0].owner, m_result.updates[3].owner);
    EXPECT_NE(m_result.updates[0].owner, m_result.updates[2].owner);
    for (const auto& update : m_result.updates) EXPECT_EQ(update.bytes, outer.paramData);
}

TEST_F(MaterialTransferPayloadTest, BoundedAndDeepNestedFallbacksKeepEveryCompleteUpdate)
{
    renderer::Material material, oversized;
    Initialize(material, 0);
    renderer::ShaderDescriptor largeDescriptor;
    largeDescriptor.cbufferSize = 5000;
    oversized.Init(*m_resources, largeDescriptor.cbufferSize);
    oversized.paramData.assign(largeDescriptor.cbufferSize, 119);
    m_bundle.renderer->BeginCpuFrame(1);
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope scope(*m_resources);
        for (uint32_t i = 0; i < 66; ++i) {
            material.paramData.assign(32, static_cast<uint8_t>(i));
            material.Upload(*m_resources, m_descriptor);
        }
        material.paramData.assign(32, 0);
        material.Upload(*m_resources, m_descriptor);
        oversized.Upload(*m_resources, largeDescriptor);
        renderer::material_transfer_detail::MaterialTransferPayloadScope second(*m_resources), third(*m_resources),
            fourth(*m_resources), fifth(*m_resources);
        material.Upload(*m_resources, m_descriptor);
        material.Upload(*m_resources, m_descriptor);
    }
    Complete();
    ASSERT_EQ(m_result.updates.size(), 70u);
    for (size_t i = 0; i < 66; ++i)
        EXPECT_EQ(m_result.updates[i].bytes, (std::vector<uint8_t>(32, static_cast<uint8_t>(i))));
    EXPECT_EQ(m_result.updates[66].bytes, (std::vector<uint8_t>(32, 0)));
    EXPECT_EQ(m_result.updates[67].bytes, oversized.paramData);
    EXPECT_EQ(m_result.updates[68].bytes, (std::vector<uint8_t>(32, 0)));
    EXPECT_EQ(m_result.updates[69].bytes, (std::vector<uint8_t>(32, 0)));
    EXPECT_EQ(m_result.updates[0].source, m_result.updates[66].source);
    EXPECT_NE(m_result.updates[68].source, m_result.updates[69].source);
}

TEST_F(MaterialTransferPayloadTest, ScopeByteBudgetDifferentLayoutsAndFreshScopesKeepFullOutputs)
{
    renderer::Material large, small;
    renderer::ShaderDescriptor largeDescriptor;
    largeDescriptor.cbufferSize = 4096;
    large.Init(*m_resources, largeDescriptor.cbufferSize);
    Initialize(small, 0);
    m_bundle.renderer->BeginCpuFrame(1);
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope scope(*m_resources);
        for (uint32_t i = 0; i < 17; ++i) {
            large.paramData.assign(4096, static_cast<uint8_t>(i));
            large.Upload(*m_resources, largeDescriptor);
        }
        small.Upload(*m_resources, m_descriptor);
        Draw(small, *m_resources);
        large.paramData.assign(4096, 0);
        large.Upload(*m_resources, largeDescriptor);
    }
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope freshScope(*m_resources);
        large.Upload(*m_resources, largeDescriptor);
        Draw(large, *m_resources);
    }
    Complete();
    ASSERT_EQ(m_result.updates.size(), 20u);
    ASSERT_EQ(m_result.draws.size(), 2u);
    for (size_t i = 0; i < 17; ++i)
        EXPECT_EQ(m_result.updates[i].bytes, (std::vector<uint8_t>(4096, static_cast<uint8_t>(i))));
    EXPECT_EQ(m_result.updates[17].bytes, small.paramData);
    EXPECT_EQ(m_result.updates[18].bytes, large.paramData);
    EXPECT_EQ(m_result.updates[19].bytes, large.paramData);
    EXPECT_EQ(m_result.draws[0].bytes, small.paramData);
    EXPECT_EQ(m_result.draws[1].bytes, large.paramData);
    EXPECT_EQ(m_result.updates[0].source, m_result.updates[18].source);
    EXPECT_NE(m_result.updates[0].source, m_result.updates[19].source);
}

TEST_F(MaterialTransferPayloadTest, ScopeNeverLendsEligibilityToOtherManagerOrRejectingCustomFacade)
{
    renderer::Material mainMaterial;
    Initialize(mainMaterial);
    renderer::ResourceManager foreign(*m_bundle.renderer);
    renderer::Material foreignMaterial;
    foreignMaterial.Init(foreign, 32);
    foreignMaterial.paramData = mainMaterial.paramData;
    RhiResult customLegacy;
    MaterialTransferResult customResult;
    RejectingMaterialTransferRhi custom(customLegacy, customResult);
    EXPECT_FALSE(custom.IsRenderThread());
    custom.ExecuteResourceOperation([&] {
        EXPECT_TRUE(custom.IsRenderThread());
        custom.ExecuteResourceOperation([&] { EXPECT_TRUE(custom.IsRenderThread()); });
        EXPECT_TRUE(custom.IsRenderThread());
    });
    EXPECT_FALSE(custom.IsRenderThread());
    renderer::ResourceManager customResources(custom);
    renderer::Material customMaterial;
    customMaterial.Init(customResources, 32);
    customMaterial.paramData = mainMaterial.paramData;
    custom.onRecord = [&] { customMaterial.paramData[0] = 99; };
    m_bundle.renderer->BeginCpuFrame(1);
    {
        renderer::material_transfer_detail::MaterialTransferPayloadScope scope(*m_resources);
        mainMaterial.Upload(*m_resources, m_descriptor);
        foreignMaterial.Upload(foreign, m_descriptor);
        foreignMaterial.Upload(foreign, m_descriptor);
        customMaterial.Upload(customResources, m_descriptor);
        {
            renderer::material_transfer_detail::MaterialTransferPayloadScope customScope(customResources);
            customMaterial.paramData[0] = 37;
            customMaterial.Upload(customResources, m_descriptor);
        }
        mainMaterial.Upload(*m_resources, m_descriptor);
    }
    Complete();
    ASSERT_EQ(m_result.updates.size(), 4u);
    EXPECT_EQ(m_result.updates[0].source, m_result.updates[3].source);
    EXPECT_NE(m_result.updates[1].source, m_result.updates[2].source);
    EXPECT_EQ(custom.recordCalls, 2u);
    ASSERT_EQ(customResult.updates.size(), 2u);
    EXPECT_EQ(customResult.updates[0].bytes[0], 99u);
    EXPECT_EQ(customResult.updates[1].bytes[0], 99u);
    EXPECT_EQ(customResult.updates[0].source, customMaterial.paramData.data());
    EXPECT_EQ(customResult.updates[1].source, customMaterial.paramData.data());
    /// @note These three managers share Active() only as a legacy destructor fallback; release through each real owner.
    m_resources->Release(mainMaterial.paramsBuffer);
    foreign.Release(foreignMaterial.paramsBuffer);
    customResources.Release(customMaterial.paramsBuffer);
    EXPECT_FALSE(custom.IsRenderThread());
    mainMaterial.paramsBuffer = {};
    foreignMaterial.paramsBuffer = {};
    customMaterial.paramsBuffer = {};
}


/// @pre The executor was stopped after accepted input completed; only permitted spurious try-lock failure can reject.
/// @note Negative readiness checks use one probe; this bounded yield retry makes no elapsed-time assertion.
bool ProbeStoppedIdleThread(renderer::RenderThread& thread)
{
    for (uint32_t attempt = 0; attempt < 65536; ++attempt) {
        if (thread.TryIsIdle()) return true;
        std::this_thread::yield();
    }
    return false;
}

/// @note Both execution and destruction remain blocked until the producer explicitly releases their semaphore.
struct IdleProbeLifetimeState {
    std::binary_semaphore entered{0}, resume{0}, destructionEntered{0}, destructionResume{0};
    std::atomic<bool> workerIdle{true}, destroyed{false};
};

class IdleProbeLifetimeFrame final : public renderer::IRenderFrame {
public:
    IdleProbeLifetimeFrame(renderer::RenderThread& thread, std::shared_ptr<IdleProbeLifetimeState> state)
        : m_thread(thread), m_state(std::move(state)) {}
    ~IdleProbeLifetimeFrame() override
    {
        if (!m_executed) return;
        m_state->destructionEntered.release();
        m_state->destructionResume.acquire();
        m_state->destroyed.store(true);
    }
    void Execute() override
    {
        m_executed = true;
        m_state->workerIdle.store(m_thread.TryIsIdle());
        m_state->entered.release();
        m_state->resume.acquire();
    }
private:
    renderer::RenderThread& m_thread;
    std::shared_ptr<IdleProbeLifetimeState> m_state;
    bool m_executed = false;
};

TEST_F(RenderThreadTest, IdleProbeRejectsWorkerExecutionAndLiveInputDestruction)
{
    auto state = std::make_shared<IdleProbeLifetimeState>();
    ASSERT_TRUE(m_thread->Submit(1, std::make_unique<IdleProbeLifetimeFrame>(*m_thread, state)));
    state->entered.acquire();
    EXPECT_FALSE(state->workerIdle.load());
    EXPECT_FALSE(m_thread->TryIsIdle());
    state->resume.release();
    state->destructionEntered.acquire();
    EXPECT_FALSE(state->destroyed.load());
    EXPECT_FALSE(m_thread->TryIsIdle());
    state->destructionResume.release();
    ASSERT_TRUE(m_thread->WaitUntilCompleted(1));
    EXPECT_TRUE(state->destroyed.load());
    m_thread->Stop();
    EXPECT_TRUE(ProbeStoppedIdleThread(*m_thread));
}

TEST_F(RenderThreadTest, IdleProbeRejectsAnotherProducersReservationWithoutCommittingSerial)
{
    std::binary_semaphore reserved{0}, release{0};
    bool valid = false;
    std::jthread producer([&] {
        auto reservation = ReserveIdleSubmission(*m_thread, 1);
        valid = reservation.IsValid();
        reserved.release();
        release.acquire();
    });
    reserved.acquire();
    EXPECT_TRUE(valid);
    if (valid) EXPECT_FALSE(m_thread->TryIsIdle());
    release.release();
    producer.join();
    EXPECT_FALSE(m_thread->WaitUntilCompleted(1));
    Resume();
    ASSERT_TRUE(m_thread->Submit(1, std::make_unique<OwnedRenderFrame>(std::vector<int>{23}, m_result)));
    ASSERT_TRUE(m_thread->WaitUntilCompleted(1));
    EXPECT_TRUE(m_result->destroyed.load());
    EXPECT_EQ(m_result->values, (std::vector<int>{23}));
    m_thread->Stop();
    EXPECT_TRUE(ProbeStoppedIdleThread(*m_thread));
}

TEST_F(ThreadedRhiTest, EmptyQueueBusyWorkerKeepsNestedPumpGuardAndDrainsNewRecords)
{
    std::binary_semaphore packetReached{0};
    m_result.blockFirst = true;
    m_bundle.renderer->BeginCpuFrame(1);
    m_bundle.renderer->BeginFrame();
    Draw(31);
    EXPECT_TRUE(m_bundle.renderer->RecordResourceUpdate([&packetReached] { packetReached.release(); }));
    m_bundle.renderer->EndFrame();
    m_result.entered.acquire();
    m_bundle.renderer->BeginCpuFrame(2);
    EarlySubmitPumpState state{.renderer = m_bundle.renderer.get(), .resources = m_resources.get(),
        .constants = m_constants, .resume = &m_result.resume, .resumed = &m_resumed, .append = true};
    s_earlySubmitPump = &state;
    m_bundle.renderer->SetRenderWaitPump(&EarlySubmitWaitPump);
    m_bundle.renderer->WaitForCpuFrame();
    m_bundle.renderer->SetRenderWaitPump(nullptr);
    s_earlySubmitPump = nullptr;
    EXPECT_TRUE(state.invoked);
    if (!state.invoked) { m_resumed = true; m_result.resume.release(); }
    packetReached.acquire();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_FALSE(state.attemptResult);
    EXPECT_EQ(m_result.transfers, (std::vector<int>{31, 59}));
    EXPECT_EQ(m_result.draws, (std::vector<int>{31}));
    EXPECT_EQ(m_result.operations, (std::vector<std::string>{"draw:31"}));
    m_bundle.renderer->CancelCpuFrame();
}

struct IdleWaitCommandLifetimeState {
    std::binary_semaphore destructionEntered{0}, resume{0}, finished{0};
    std::atomic<bool> armed{false}, destroyed{false};
    std::thread::id executionThread;
};

class IdleWaitCommandOwner final {
public:
    explicit IdleWaitCommandOwner(std::shared_ptr<IdleWaitCommandLifetimeState> state) : m_state(std::move(state)) {}
    ~IdleWaitCommandOwner()
    {
        if (!m_state->armed.load()) return;
        m_state->destructionEntered.release();
        m_state->resume.acquire();
        m_state->destroyed.store(true);
        m_state->finished.release();
    }
private:
    std::shared_ptr<IdleWaitCommandLifetimeState> m_state;
};

struct IdleWaitDestructionPumpState {
    std::binary_semaphore* resume = nullptr;
    bool invoked = false;
};
thread_local IdleWaitDestructionPumpState* s_idleWaitDestructionPump = nullptr;

void IdleWaitDestructionPump()
{
    auto* state = s_idleWaitDestructionPump;
    if (!state || state->invoked) return;
    state->invoked = true;
    state->resume->release();
}

TEST_F(ThreadedRhiTest, EmptyQueueWaitIncludesSubmittedCommandOwnerDestruction)
{
    auto lifetime = std::make_shared<IdleWaitCommandLifetimeState>();
    auto owner = std::make_shared<IdleWaitCommandOwner>(lifetime);
    m_bundle.renderer->BeginCpuFrame(1);
    const bool accepted = m_bundle.renderer->RecordResourceUpdate([lifetime, owner = std::move(owner)] {
        lifetime->executionThread = std::this_thread::get_id();
        lifetime->armed.store(true);
    });
    EXPECT_TRUE(accepted);
    if (!accepted) { m_bundle.renderer->CancelCpuFrame(); return; }
    m_bundle.renderer->EndFrame();
    lifetime->destructionEntered.acquire();
    IdleWaitDestructionPumpState pump{.resume = &lifetime->resume};
    s_idleWaitDestructionPump = &pump;
    m_bundle.renderer->SetRenderWaitPump(&IdleWaitDestructionPump);
    m_bundle.renderer->WaitForCpuFrame();
    m_bundle.renderer->SetRenderWaitPump(nullptr);
    s_idleWaitDestructionPump = nullptr;
    EXPECT_TRUE(pump.invoked);
    EXPECT_TRUE(lifetime->destroyed.load());
    if (!pump.invoked) lifetime->resume.release();
    lifetime->finished.acquire();
    m_bundle.renderer->WaitForCpuFrame();
    EXPECT_NE(lifetime->executionThread, std::this_thread::get_id());
}

TEST_F(ThreadedRhiTest, IdleWaitLeavesFrameOpenForAnotherProducersLaterOwnedRecord)
{
    std::binary_semaphore recordNow{0}, recorded{0};
    std::atomic<int> result{0};
    std::thread::id executionThread;
    bool accepted = false;
    auto source = std::make_shared<int>(47);
    std::weak_ptr<int> lifetime = source;
    m_bundle.renderer->BeginCpuFrame(1);
    std::jthread producer([&, input = std::move(source)]() mutable {
        recordNow.acquire();
        accepted = m_bundle.renderer->RecordResourceUpdate([&, input = std::move(input)] {
            executionThread = std::this_thread::get_id();
            result.store(*input);
        });
        recorded.release();
    });
    m_bundle.renderer->WaitForCpuFrame();
    recordNow.release();
    recorded.acquire();
    EXPECT_TRUE(accepted);
    EXPECT_EQ(result.load(), 0);
    EXPECT_FALSE(lifetime.expired());
    m_bundle.renderer->WaitForCpuFrame();
    producer.join();
    EXPECT_EQ(result.load(), 47);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_NE(executionThread, std::this_thread::get_id());
    m_bundle.renderer->CancelCpuFrame();
}

/// @note Capture の最終破棄と worker 実行順を、公開された同期完了だけから観測する。
struct RecycledCommandLifetime {
    std::vector<int> executed, destroyed;
    std::vector<std::thread::id> destructionThreads;
};

class RecycledCommandOwner final {
public:
    RecycledCommandOwner(std::shared_ptr<RecycledCommandLifetime> lifetime, int value)
        : m_lifetime(std::move(lifetime)), m_value(value) {}
    ~RecycledCommandOwner()
    {
        m_lifetime->destroyed.push_back(m_value);
        m_lifetime->destructionThreads.push_back(std::this_thread::get_id());
    }
    int Value() const { return m_value; }
private:
    std::shared_ptr<RecycledCommandLifetime> m_lifetime;
    int m_value;
};

TEST_F(ThreadedRhiTest, RecycledCommandStoragePreservesOrderAndReleasesOwnersBeforeSyncAndFrameCompletion)
{
    const auto producer = std::this_thread::get_id();
    auto lifetime = std::make_shared<RecycledCommandLifetime>();
    std::vector<std::weak_ptr<RecycledCommandOwner>> owners;
    std::vector<int> expected;
    m_result.tryDispatchResult = true;
    uint64_t serial = 1;
    int nextValue = 0;
    for (const uint32_t count : std::array{32u, 5u, 64u, 3u}) {
        m_bundle.renderer->BeginCpuFrame(serial++);
        m_bundle.renderer->BeginFrame();
        for (uint32_t index = 0; index < count; ++index) {
            auto owner = std::make_shared<RecycledCommandOwner>(lifetime, ++nextValue);
            owners.push_back(owner);
            ASSERT_TRUE(m_bundle.renderer->RecordResourceUpdate([lifetime, owner] {
                lifetime->executed.push_back(owner->Value());
            }));
            owner.reset();
            expected.push_back(nextValue);
            EXPECT_FALSE(owners.back().expired());
            if (index == count / 2) {
                renderer::ComputeCall call;
                ASSERT_TRUE(m_bundle.renderer->TryDispatch(call, *m_resources));
                EXPECT_EQ(lifetime->executed, expected);
                auto destroyed = lifetime->destroyed;
                std::sort(destroyed.begin(), destroyed.end());
                EXPECT_EQ(destroyed, expected);
                for (const auto& pending : owners) EXPECT_TRUE(pending.expired());
            }
        }
        m_bundle.renderer->EndFrame();
        m_bundle.renderer->WaitForCpuFrame();
        EXPECT_EQ(lifetime->executed, expected);
        auto destroyed = lifetime->destroyed;
        std::sort(destroyed.begin(), destroyed.end());
        EXPECT_EQ(destroyed, expected);
        for (const auto& pending : owners) EXPECT_TRUE(pending.expired());
    }
    ASSERT_EQ(lifetime->destructionThreads.size(), expected.size());
    for (const auto thread : lifetime->destructionThreads) EXPECT_NE(thread, producer);
}

} /// @note namespace
} /// @note namespace fbzz::tests
