/// @file    ThreadedRenderer.cpp
/// @brief   所有済み RHI 要求を Render Thread へ渡し、同期 API の完了契約を維持する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Renderer/RendererFactory.hpp>
#include "MaterialTransferPayload.hpp"
#include "BufferStateReadScope.hpp"
#include "PreparedSkinningBackend.hpp"
#include "GpuCullDrawQueue.hpp"
#include <Graphics/Renderer/RenderThread.hpp>
#include <Graphics/Renderer/RenderCpuTrace.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <Core/Profiler/FrameTimeline.hpp>
#include <cassert>
#include <algorithm>
#include <atomic>
#include <array>
#include <limits>
#include <cstring>
#include <functional>
#include <mutex>
#include <source_location>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <imgui.h>

namespace fbzz::renderer {
class ThreadedRenderer;
namespace {

/// @note Classification is private to this Graphics DLL; a consumed token cannot classify reentrant closures.
thread_local IRenderer* s_cpuShadowTransfer = nullptr;
thread_local bool s_resourceSubmitActive = false;

bool ConsumeCpuShadowTransfer(IRenderer& renderer)
{
    return std::exchange(s_cpuShadowTransfer, nullptr) == &renderer;
}

enum class ResourceSubmitOutcome : uint8_t { UNKNOWN, SUCCESS, MIXED, REENTRY, NO_QUEUED, WORKER, SERIAL_EXHAUSTED, BUSY };
constexpr std::array<const char*, 8> RESOURCE_SUBMIT_LABELS{
    "AsyncResourceSubmit::Unknown", "AsyncResourceSubmit::Success", "AsyncResourceSubmit::Mixed",
    "AsyncResourceSubmit::Reentry", "AsyncResourceSubmit::NoQueued", "AsyncResourceSubmit::Worker",
    "AsyncResourceSubmit::SerialExhausted", "AsyncResourceSubmit::Busy"};

enum class OpaqueQueueOrigin : uint8_t { OTHER, RESOURCE_UPDATE, IMGUI_GPU_PREPARE, BEGIN_FRAME, GPU_PROFILER };
struct OpaqueQueueLabels { const char* atFront; const char* afterPending; };
constexpr std::array<OpaqueQueueLabels, 5> OPAQUE_QUEUE_LABELS{{
    {"OpaqueQueue::Other::AtFront", "OpaqueQueue::Other::AfterPending"},
    {"OpaqueQueue::ResourceUpdate::AtFront", "OpaqueQueue::ResourceUpdate::AfterPending"},
    {"OpaqueQueue::ImGuiGpuPrepare::AtFront", "OpaqueQueue::ImGuiGpuPrepare::AfterPending"},
    {"OpaqueQueue::BeginFrame::AtFront", "OpaqueQueue::BeginFrame::AfterPending"},
    {"OpaqueQueue::GpuProfiler::AtFront", "OpaqueQueue::GpuProfiler::AfterPending"}}};

struct RendererRegistryEntry { IRenderer* facade = nullptr; ThreadedRenderer* owner = nullptr; };
struct RendererRegistry { std::mutex mutex; std::array<RendererRegistryEntry, 16> entries{}; };
RendererRegistry& Registry() { static RendererRegistry registry; return registry; }

void RegisterRenderer(IRenderer& facade, ThreadedRenderer& owner)
{
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    for (auto& entry : registry.entries) if (!entry.facade) { entry = {&facade, &owner}; return; }
}

void UnregisterRenderer(IRenderer& facade)
{
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    for (auto& entry : registry.entries) if (entry.facade == &facade) { entry = {}; return; }
}

/// @note Queue acceptance does not borrow native buffer pointers or drain the producer frame.
bool CullDrawRangeCurrent(const DrawCall& call, ResourceManager& resources)
{
    const auto* indices = resources.Get(call.indexBuffer);
    return indices && static_cast<uint64_t>(call.startIndex) + call.indexCount <= indices->GetSize()/sizeof(uint32_t);
}

class SynchronousScope final {
public:
    explicit SynchronousScope(bool& active) : m_active(active), m_previous(std::exchange(active, true)) {}
    ~SynchronousScope() { m_active = m_previous; }
private:
    bool& m_active;
    bool m_previous;
};

using CommandList = std::vector<std::function<void()>>;

/// @note Producer は idle 待ちまたは submission reservation 後だけ取得する。worker は完了通知前に返却し、既存 mutex で同期する。
class CommandStoragePool final {
public:
    CommandList Acquire()
    {
        size_t selected = 0;
        for (size_t index = 1; index < m_recycled.size(); ++index)
            if (m_recycled[index].capacity() > m_recycled[selected].capacity()) selected = index;
        return std::move(m_recycled[selected]);
    }
    /// @pre Capture の破棄が完了した空配列だけを渡す。
    void Recycle(CommandList&& commands)
    {
        assert(commands.empty());
        if (commands.capacity() == 0 || commands.capacity() > MAX_COMMANDS) return;
        size_t selected = 0;
        for (size_t index = 1; index < m_recycled.size(); ++index)
            if (m_recycled[index].capacity() < m_recycled[selected].capacity()) selected = index;
        if (commands.capacity() > m_recycled[selected].capacity()) m_recycled[selected] = std::move(commands);
    }
private:
    static constexpr size_t MAX_COMMANDS = 16384;
    std::array<CommandList, 3> m_recycled;
};

class RenderCommands final : public IRenderFrame {
public:
    RenderCommands(CommandList commands, CommandStoragePool& storage) : m_commands(std::move(commands)), m_storage(storage) {}
    ~RenderCommands() override
    {
        /// @note 資源 owner と任意 capture は worker で破棄し、空の capacity だけを再貸出しする。
        m_commands.clear();
        m_storage.Recycle(std::move(m_commands));
    }
    void Execute() override { for (auto& command : m_commands) command(); }
private:
    CommandList m_commands;
    CommandStoragePool& m_storage;
};

class ImGuiContextScope {
public:
    explicit ImGuiContextScope(ImGuiContext* context) : m_previous(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(context); }
    ~ImGuiContextScope() { ImGui::SetCurrentContext(m_previous); }
private:
    ImGuiContext* m_previous;
};

struct ImGuiContextDeleter {
    void operator()(ImGuiContext* context) const { if (context) ImGui::DestroyContext(context); }
};

struct ImGuiDrawListDeleter {
    void operator()(ImDrawList* list) const
    {
        if (!list) return;
        list->~ImDrawList();
        ImGui::MemFree(list);
    }
};

/// @note 動的 texture 更新を同期済みにしてから数値 ID を固定し、次の UI フレームの atlas を参照しない。
/// @see https://github.com/ocornut/imgui/blob/v1.92.8/imgui.h ImDrawData::Textures and ImDrawList::CloneOutput.
struct OwnedImGuiDrawData {
    /// @note Backend userdata is borrowed from the native viewport; only the backend destroys it after queued rendering completes.
    /// @see https://github.com/ocornut/imgui/blob/v1.92.8/imgui.h ImGuiViewport destructor requires both backend userdata pointers to be null.
    ~OwnedImGuiDrawData()
    {
        viewport.PlatformUserData = nullptr;
        viewport.RendererUserData = nullptr;
    }
    void Copy(const ImDrawData& source)
    {
        /// @note clone の一時確保を UI context の未解放数に加算しない。破棄も別スレッドの context 外で行う。
        const ImGuiContextScope scope(nullptr);
        data.Valid = source.Valid;
        data.TotalIdxCount = source.TotalIdxCount;
        data.TotalVtxCount = source.TotalVtxCount;
        data.DisplayPos = source.DisplayPos;
        data.DisplaySize = source.DisplaySize;
        data.FramebufferScale = source.FramebufferScale;
        /// @note A conditional value-copy creates an owning temporary whose destructor asserts on borrowed backend userdata.
        if (source.OwnerViewport) viewport = *source.OwnerViewport;
        else viewport = ImGuiViewport{};
        viewport.DrawData = &data;
        data.OwnerViewport = &viewport;
        data.Textures = nullptr;
        data.CmdLists.resize(source.CmdLists.Size);
        for (int index = 0; index < source.CmdLists.Size; ++index) {
            const auto* sourceList = source.CmdLists[index];
            if (static_cast<size_t>(index) == lists.size()) lists.emplace_back(sourceList->CloneOutput());
            auto& list = *lists[index];
            CopyBuffer(list.CmdBuffer, sourceList->CmdBuffer);
            CopyBuffer(list.IdxBuffer, sourceList->IdxBuffer);
            CopyBuffer(list.VtxBuffer, sourceList->VtxBuffer);
            list.Flags = sourceList->Flags;
            for (auto& command : list.CmdBuffer) command.TexRef = ImTextureRef(command.GetTexID());
            data.CmdLists[index] = &list;
        }
        data.CmdListsCount = data.CmdLists.Size;
    }
    ImDrawData data;
    ImGuiViewport viewport;
    std::vector<std::unique_ptr<ImDrawList, ImGuiDrawListDeleter>> lists;
    std::atomic<bool> completed{false};
private:
    template<typename T>
    static void CopyBuffer(ImVector<T>& target, const ImVector<T>& source)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        /// @note ImVector の代入は既存容量を破棄するため、resize とバイトコピーで完了済みバッファを再利用する。
        target.resize(source.Size);
        if (source.Size > 0) std::memcpy(target.Data, source.Data, static_cast<size_t>(source.Size) * sizeof(T));
    }
};

struct OwnedImGuiPlatformFrame {
    std::vector<std::unique_ptr<OwnedImGuiDrawData>> viewports;
    size_t count = 0;
    std::atomic<bool> completed{false};
};

/// @note pool の参照だけになった入力は worker が処理・破棄を完了しているため、producer が安全に再利用できる。
template<typename T>
std::shared_ptr<T> AcquireGuiSnapshot(std::vector<std::shared_ptr<T>>& pool)
{
    for (const auto& snapshot : pool) if (snapshot.use_count() == 1) {
        /// @note 参照数は寿命だけを表す。worker の最終アクセスとは release/acquire で同期する。
        static_cast<void>(snapshot->completed.exchange(false, std::memory_order_acq_rel));
        return snapshot;
    }
    auto snapshot = std::make_shared<T>();
    pool.push_back(snapshot);
    return snapshot;
}

} /// @note namespace

class ThreadedRenderer final : public IRenderer {
public:
    explicit ThreadedRenderer(std::unique_ptr<IRenderer> native)
        : m_native(std::move(native)), m_capabilities(m_native->GetCapabilities()),
          m_supportsIndirectDraw(m_native->SupportsIndirectDraw()),
          m_supportsMeshShaders(m_native->SupportsMeshShaders()),
          m_width(m_native->GetWidth()), m_height(m_native->GetHeight()), m_vsync(m_native->GetVSync())
    {
        cpu_trace::InitializeFromEnvironment();
        cpu_trace::RegisterThread(&m_thread);
        m_native->SetCpuExecutor(this);
        RegisterRenderer(*this, *this);
    }
    ~ThreadedRenderer() override { UnregisterRenderer(*this); Shutdown(); }

    template<typename Function>
    auto Sync(Function&& function, std::source_location caller = std::source_location::current()) -> decltype(function())
    {
        m_bufferReadRevision.fetch_add(1, std::memory_order_relaxed);
        if (IsRenderThread()) return function();
        const cpu_trace::SourceScope traceSource(cpu_trace::Source::UNKNOWN, caller.function_name(), true);
        cpu_trace::Count(cpu_trace::Counter::SYNC);
        /// @note 同期 API の要求元を別 scope に残し、同名の packet 完了待ちから原因の操作を辿れるようにする。
        const profiler::ProfileScope callerScope(profiler::ProfilerMarker(caller.function_name(), "Render synchronization"));
        FBZZ_PROFILE_SCOPE("ThreadedRenderer::Sync");
        cpu_trace::AcquireScope traceAcquire(cpu_trace::AcquireKind::EXECUTION_MUTEX);
        const auto timelineMutex = profiler::frame_timeline::Begin(profiler::frame_timeline::Event::EXECUTION_MUTEX);
        std::lock_guard executionLock(m_executionMutex);
        profiler::frame_timeline::End(timelineMutex);
        traceAcquire.Acquired();
        const SynchronousScope synchronousScope(m_synchronousActive);
        using Result = decltype(function());
        if constexpr (std::is_void_v<Result>) {
            ExecuteAndWait([&function] { function(); });
        } else {
            Result result{};
            ExecuteAndWait([&result, &function] { result = function(); });
            return result;
        }
    }

    void Record(std::function<void()> command, bool cpuShadowTransfer = false,
        OpaqueQueueOrigin opaqueOrigin = OpaqueQueueOrigin::OTHER)
    {
        m_bufferReadRevision.fetch_add(1, std::memory_order_relaxed);
        if (IsRenderThread()) { command(); return; }
        {
            std::lock_guard lock(m_commandsMutex);
            if (m_cpuFrameOpen) {
                /// @note Count only the first opaque entry in this pending vector; labels classify diagnostics, never eligibility.
                if (!cpuShadowTransfer && m_resourceUpdatesOnly && cpu_trace::HasSelectedFrame()) {
                    const auto& labels = OPAQUE_QUEUE_LABELS[static_cast<size_t>(opaqueOrigin)];
                    const cpu_trace::SourceScope traceOpaque(cpu_trace::Source::UNKNOWN,
                        m_commands.empty() ? labels.atFront : labels.afterPending);
                }
                m_resourceUpdatesOnly &= cpuShadowTransfer;
                m_commands.push_back(std::move(command));
                return;
            }
        }
        Sync(std::move(command));
    }

    const char* GetBackendName() const override { return m_native->GetBackendName(); }
    bool SupportsIndirectDraw() const override { return m_supportsIndirectDraw; }
    bool SupportsMeshShaders() const override { return m_supportsMeshShaders; }
    void BeginCpuFrame(uint64_t serial) override
    {
        m_bufferReadRevision.fetch_add(1, std::memory_order_relaxed);
        cpu_trace::BeginFrame(&m_thread, serial);
        std::lock_guard lock(m_commandsMutex);
        assert(!m_cpuFrameOpen && m_commands.empty());
        m_resourceUpdatesOnly = true;
        m_applicationSerial = serial;
        m_cpuFrameOpen = true;
    }
    void WaitForCpuFrame() override
    {
        m_bufferReadRevision.fetch_add(1, std::memory_order_relaxed);
        if (IsRenderThread()) return;
        FBZZ_PROFILE_SCOPE("ThreadedRenderer::WaitForCpuFrame");
        cpu_trace::Count(cpu_trace::Counter::CPU_FRAME_WAIT);
        cpu_trace::AcquireScope traceAcquire(cpu_trace::AcquireKind::EXECUTION_MUTEX);
        const auto timelineMutex = profiler::frame_timeline::Begin(profiler::frame_timeline::Event::EXECUTION_MUTEX);
        std::lock_guard executionLock(m_executionMutex);
        profiler::frame_timeline::End(timelineMutex);
        traceAcquire.Acquired();
        const SynchronousScope synchronousScope(m_synchronousActive);
        {
            /// @note Keep the queue locked through the idle witness; in-flight work, input destruction and reserved submissions retain the full pump/drain path.
            std::lock_guard commandsLock(m_commandsMutex);
            if (m_commands.empty() && m_thread.TryIsIdle()) return;
        }
        Flush();
    }
    void CancelCpuFrame() override { m_bufferReadRevision.fetch_add(1, std::memory_order_relaxed); std::lock_guard lock(m_commandsMutex); m_commands.clear(); m_resourceUpdatesOnly = true; m_cpuFrameOpen = false; }
    void SetRenderWaitPump(void (*pump)()) override { m_thread.SetWaitPump(pump); }
    bool IsRenderThread() const override { return m_thread.IsCurrentThread(); }
    bool SupportsPreparedSkinning() const noexcept { return prepared_skinning_detail::HasRegisteredBackend(*m_native); }
    void ExecuteResourceOperation(const std::function<void()>& operation) override { Sync(operation); }
    std::unique_ptr<prepared_skinning_detail::NativeWork> PrepareSkinning(ResourceManager& resources,
        ResourceHandle<ShaderTag> shader, std::span<const CapturedSkinningRequest> requests,
        std::span<const math::Matrix4> matrices)
    {
        return Sync([&] { return prepared_skinning_detail::PrepareExactNative(*m_native, resources, shader, requests, matrices); });
    }
    bool TryObserveIdleReadBoundary(uint64_t& submission, uint64_t& revision)
    {
        if (std::this_thread::get_id() != m_producerThread || IsRenderThread()) return false;
        std::unique_lock executionLock(m_executionMutex, std::try_to_lock);
        if (!executionLock.owns_lock() || m_synchronousActive || m_shutdown) return false;
        std::unique_lock commandsLock(m_commandsMutex, std::try_to_lock);
        if (!commandsLock.owns_lock() || !m_commands.empty() || !m_thread.TryIsIdle()) return false;
        submission = m_submissionSerial;
        revision = m_bufferReadRevision.load(std::memory_order_relaxed);
        return true;
    }
    bool RecordResourceUpdate(std::function<void()> operation) override
    {
        const bool cpuShadowTransfer = ConsumeCpuShadowTransfer(*this);
        if (IsRenderThread()) return false;
        Record(std::move(operation), cpuShadowTransfer, OpaqueQueueOrigin::RESOURCE_UPDATE);
        return true;
    }
    /// @note Registry lifetime is borrowed by the caller; all existing synchronization uses the same execution mutex.
    ResourceSubmitOutcome TrySubmitResourceUpdates()
    {
        if (IsRenderThread()) return ResourceSubmitOutcome::WORKER;
        std::unique_lock executionLock(m_executionMutex, std::try_to_lock);
        if (!executionLock.owns_lock()) return ResourceSubmitOutcome::BUSY;
        if (m_synchronousActive) return ResourceSubmitOutcome::REENTRY;
        std::unique_lock commandsLock(m_commandsMutex, std::try_to_lock);
        if (!commandsLock.owns_lock()) return ResourceSubmitOutcome::BUSY;
        if (m_shutdown || !m_cpuFrameOpen || m_commands.empty()) return ResourceSubmitOutcome::NO_QUEUED;
        if (!m_resourceUpdatesOnly) return ResourceSubmitOutcome::MIXED;
        if (m_submissionSerial == std::numeric_limits<uint64_t>::max()) return ResourceSubmitOutcome::SERIAL_EXHAUSTED;
        const uint64_t serial = m_submissionSerial + 1;
        auto reservation = m_thread.TryReserveSubmission(serial);
        if (!reservation.IsValid()) return ResourceSubmitOutcome::BUSY;
        const auto commandCount = m_commands.size();
        auto timelineContext = profiler::frame_timeline::CurrentContext();
        timelineContext.submissionSerial = serial;
        profiler::frame_timeline::Mark(profiler::frame_timeline::Event::PACKET_SUBMIT, timelineContext, commandCount);
        auto frame = std::make_unique<RenderCommands>(std::move(m_commands), m_commandStorage);
        m_commands = m_commandStorage.Acquire();
        cpu_trace::BindPacket(&m_thread, serial, commandCount);
        /// @note Commit before publication because the worker may finish immediately after notification.
        m_submissionSerial = serial;
        m_resourceUpdatesOnly = true;
        const bool accepted = reservation.Publish(std::move(frame));
        assert(accepted);
        cpu_trace::PacketAccepted(&m_thread, serial, accepted);
        return ResourceSubmitOutcome::SUCCESS;
    }
    void Shutdown() override
    {
        if (m_shutdown) return;
        WaitForCpuFrame();
        Sync([this] { CloseRecordedGpuViews(); m_native->SetCpuExecutor(nullptr); m_native->Shutdown(); });
        m_thread.Stop();
        (void)cpu_trace::WriteCsvAfterStop();
        m_shutdown = true;
    }
    void BeginFrame() override
    {
        if (m_resizePending) {
            const auto width = m_width, height = m_height;
            Record([this, width, height] { m_native->Resize(width, height); });
            m_resizePending = false;
        }
        const auto serial = m_applicationSerial;
        const bool recording = profiler::Profiler::IsEnabled();
        Record([this, serial, recording] {
            m_workerFrameTimeline = profiler::frame_timeline::Begin(profiler::frame_timeline::Event::WORKER_FRAME, {}, 0, true);
            profiler::Profiler::SetEnabled(recording);
            profiler::Profiler::BeginFrame(serial, profiler::ProfileRecorder::NowMs());
            CloseRecordedGpuViews();
            m_native->BeginFrame();
        }, false, OpaqueQueueOrigin::BEGIN_FRAME);
    }
    void EndFrame() override
    {
        Record([this] {
            CloseRecordedGpuViews();
            m_native->EndFrame();
            profiler::frame_timeline::End(m_workerFrameTimeline);
            m_workerFrameTimeline = 0;
            profiler::Profiler::EndFrame();
            std::lock_guard lock(m_snapshotMutex);
            m_completedGpuSnapshot = m_native->GpuProfGetSnapshot();
            m_completedGpuResults = m_native->GpuProfGetResults();
        });
        bool submit = false;
        { std::lock_guard lock(m_commandsMutex); submit = std::exchange(m_cpuFrameOpen, false); }
        if (submit) SubmitCommands();
    }
    void Clear(const math::Vector4& color) override { Record([this, color] { m_native->Clear(color); }); }
    void Submit(const DrawCall& call, ResourceManager& resources) override
    {
        Record([this, call, &resources] { m_native->Submit(call, resources); });
    }
    void CullDraw(const ComputeCall& compute, const DrawCall& indirect, const DrawCall& fallback, ResourceManager& resources)
    {
        Record([this, compute, indirect, fallback, &resources] {
            if (!CullDrawRangeCurrent(indirect, resources)) return;
            if (m_native->TryDispatch(compute, resources)) {
                const cpu_trace::SourceScope gpuDrawTrace(cpu_trace::Source::UNKNOWN, "GpuIndirect::NativeDraw");
                FBZZ_PROFILE_SCOPE("Render::GpuIndirect::NativeDraw"); m_native->Submit(indirect, resources);
            } else {
                const cpu_trace::SourceScope gpuFallbackTrace(cpu_trace::Source::UNKNOWN, "GpuIndirect::NativeFallback");
                FBZZ_PROFILE_SCOPE("Render::GpuIndirect::NativeFallback"); m_native->Submit(fallback, resources);
            }
        });
    }
    void Dispatch(const ComputeCall& call, ResourceManager& resources) override
    {
        Record([this, call, &resources] { m_native->Dispatch(call, resources); });
    }
    bool TryDispatch(const ComputeCall& call, ResourceManager& resources) override
    {
        return Sync([&] { return m_native->TryDispatch(call, resources); });
    }
    bool RenderDebugPreview(const DrawCall& call, ResourceHandle<RenderTargetTag> target, ResourceManager& resources) override
    {
        return Sync([&] { return m_native->RenderDebugPreview(call, target, resources); });
    }
    void BeginComputeBatch() override { Record([this] { m_native->BeginComputeBatch(); }); }
    void EndComputeBatch() override { Record([this] { m_native->EndComputeBatch(); }); }
    bool BeginAsyncCompute(ResourceManager& resources) override
    {
        return Sync([&] { return m_native->BeginAsyncCompute(resources); });
    }
    void EndAsyncCompute() override { Record([this] { m_native->EndAsyncCompute(); }); }
    void SetRenderWhenOccluded(bool enabled) override { Record([this, enabled] { m_native->SetRenderWhenOccluded(enabled); }); }
    void Resize(uint32_t width, uint32_t height) override
    {
        /// @note WM_SIZE から Present 待ちへ入らない。次の物理フレーム開始前に Render Thread で適用する。
        m_width = width;
        m_height = height;
        m_resizePending = true;
    }
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    void SetVSync(bool enabled) override { m_vsync = enabled; Record([this, enabled] { m_native->SetVSync(enabled); }); }
    bool GetVSync() const override { return m_vsync; }
    void SetRenderTarget(ResourceHandle<RenderTargetTag> target, ResourceManager& resources) override
    {
        Record([this, target, &resources] { m_native->SetRenderTarget(target, resources); });
    }
    bool SetRenderTargetColors(std::span<const RenderTargetColorAttachment> colors, ResourceManager& resources) override
    {
        if (colors.empty() || colors.size() > 8u) return false;
        std::array<RenderTargetColorAttachment, 8> owned{};
        std::copy(colors.begin(), colors.end(), owned.begin());
        return Sync([this, owned, count = colors.size(), &resources] {
            return m_native->SetRenderTargetColors(std::span{owned.data(), count}, resources);
        });
    }
    void ClearDepth() override { Record([this] { m_native->ClearDepth(); }); }
    void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override
    {
        Record([=, this] { m_native->SetViewport(x, y, width, height); });
    }
    void SetRenderTargetFace(ResourceHandle<RenderTargetTag> target, uint32_t face, uint32_t mip, ResourceManager& resources) override
    {
        Record([this, target, face, mip, &resources] { m_native->SetRenderTargetFace(target, face, mip, resources); });
    }
    GraphicsCapabilities GetCapabilities() const override { return m_capabilities; }
    bool BuildAccelerationStructure(ResourceHandle<AccelerationStructureTag> handle, ResourceManager& resources) override
    {
        return Sync([&] { return m_native->BuildAccelerationStructure(handle, resources); });
    }
    void GpuProfBeginFrame() override { Record([this] { m_native->GpuProfBeginFrame(); }, false, OpaqueQueueOrigin::GPU_PROFILER); }
    void GpuProfEndFrame() override { Record([this] { m_native->GpuProfEndFrame(); }, false, OpaqueQueueOrigin::GPU_PROFILER); }
    void GpuProfBeginPass(const char* name) override
    {
        Record([this, name = std::string(name ? name : "Unnamed")] {
            if (m_recordedGpuViews.empty() || m_recordedGpuViews.back()) m_native->GpuProfBeginPass(name.c_str());
        }, false, OpaqueQueueOrigin::GPU_PROFILER);
    }
    void GpuProfEndPass(const char* name) override
    {
        Record([this, name = std::string(name ? name : "Unnamed")] {
            if (m_recordedGpuViews.empty() || m_recordedGpuViews.back()) m_native->GpuProfEndPass(name.c_str());
        }, false, OpaqueQueueOrigin::GPU_PROFILER);
    }
    bool GpuProfBeginView(const GpuProfilerViewMetadata& metadata) override
    {
        return Sync([&] { return m_native->GpuProfBeginView(metadata); });
    }
    void GpuProfEndView() override { Record([this] { m_native->GpuProfEndView(); }, false, OpaqueQueueOrigin::GPU_PROFILER); }
    bool GpuProfRecordView(const GpuProfilerViewMetadata& metadata) override
    {
        /// @note Only the worker owns recording success; metadata is copied before producer inputs can change.
        /// @see https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class-renderingserver-method-call-on-render-thread Queued render-thread calls.
        Record([this, metadata] {
            const bool parentEnabled = m_recordedGpuViews.empty() || m_recordedGpuViews.back();
            const bool opened = parentEnabled && m_native->GpuProfBeginView(metadata);
            m_recordedGpuViews.push_back(opened);
        }, false, OpaqueQueueOrigin::GPU_PROFILER);
        return true;
    }
    void GpuProfEndRecordedView() override
    {
        Record([this] {
            assert(!m_recordedGpuViews.empty());
            if (m_recordedGpuViews.empty()) return;
            const bool opened = m_recordedGpuViews.back();
            m_recordedGpuViews.pop_back();
            if (opened) m_native->GpuProfEndView();
        }, false, OpaqueQueueOrigin::GPU_PROFILER);
    }
    void GpuProfCollect() override { Record([this] { m_native->GpuProfCollect(); }, false, OpaqueQueueOrigin::GPU_PROFILER); }
    const std::vector<GpuPassProfile>& GpuProfGetResults() const override
    {
        std::lock_guard lock(m_snapshotMutex);
        m_gpuResults = m_completedGpuResults;
        return m_gpuResults;
    }
    const GpuProfilerSnapshot& GpuProfGetSnapshot() const override
    {
        std::lock_guard lock(m_snapshotMutex);
        m_gpuSnapshot = m_completedGpuSnapshot;
        return m_gpuSnapshot;
    }
    bool BakeSkyLight(ResourceHandle<RenderTargetTag> target, ResourceManager& resources,
        uint32_t size, uint32_t irradianceSize, uint32_t prefilterSize, uint32_t mips,
        std::unique_ptr<ITexture>& irradiance, std::unique_ptr<ITexture>& prefilter) override
    {
        return Sync([&] { return m_native->BakeSkyLight(target, resources, size, irradianceSize, prefilterSize, mips, irradiance, prefilter); });
    }
    std::unique_ptr<IIblBaker> CreateIblBaker() override;
    bool CaptureRenderTargetToPng(ResourceHandle<RenderTargetTag> target, ResourceManager& resources,
        std::vector<uint8_t>& data, uint32_t& width, uint32_t& height) override
    {
        return Sync([&] { return m_native->CaptureRenderTargetToPng(target, resources, data, width, height); });
    }
    bool CaptureRenderTargetToLinearRGBA(ResourceHandle<RenderTargetTag> target, ResourceManager& resources,
        std::vector<float>& data, uint32_t& width, uint32_t& height) override
    {
        return Sync([&] { return m_native->CaptureRenderTargetToLinearRGBA(target, resources, data, width, height); });
    }

private:
    void CloseRecordedGpuViews()
    {
        /// @note A synchronous operation may have executed a begin before CancelCpuFrame discarded its pending end.
        while (!m_recordedGpuViews.empty()) {
            const bool opened = m_recordedGpuViews.back();
            m_recordedGpuViews.pop_back();
            if (opened) m_native->GpuProfEndView();
        }
    }
    void ExecuteAndWait(std::function<void()> operation)
    {
        WaitForSubmissionSlot();
        std::vector<std::function<void()>> commands;
        {
            std::lock_guard lock(m_commandsMutex);
            commands = std::move(m_commands);
            m_commands = m_commandStorage.Acquire();
            m_resourceUpdatesOnly = true;
        }
        commands.push_back(std::move(operation));
        const auto serial = ++m_submissionSerial;
        auto timelineContext = profiler::frame_timeline::CurrentContext();
        timelineContext.submissionSerial = serial;
        timelineContext.targetSerial = serial;
        const profiler::frame_timeline::ContextScope timelineOrigin(timelineContext);
        profiler::frame_timeline::Mark(profiler::frame_timeline::Event::PACKET_SUBMIT, {}, commands.size());
        cpu_trace::BindPacket(&m_thread, serial, commands.size());
        FBZZ_PROFILE_SCOPE("ThreadedRenderer::SubmitPacket");
        bool accepted = false;
        {
            FBZZ_PROFILE_SCOPE("ThreadedRenderer::PacketAcceptance");
            const cpu_trace::WaitKindScope traceWait(cpu_trace::WaitKind::ACCEPTANCE);
            const profiler::frame_timeline::Scope timelineWait(profiler::frame_timeline::Event::WAIT_ACCEPTANCE);
            accepted = m_thread.Submit(serial, std::make_unique<RenderCommands>(std::move(commands), m_commandStorage));
        }
        cpu_trace::PacketAccepted(&m_thread, serial, accepted);
        assert(accepted);
        if (accepted) {
            FBZZ_PROFILE_SCOPE("ThreadedRenderer::PacketCompletionWait");
            const cpu_trace::WaitKindScope traceWait(cpu_trace::WaitKind::COMPLETION);
            const profiler::frame_timeline::Scope timelineWait(profiler::frame_timeline::Event::WAIT_COMPLETION);
            const bool completed = m_thread.WaitUntilCompleted(serial);
            assert(completed);
            (void)completed;
        }
    }
    bool SubmitCommands()
    {
        cpu_trace::AcquireScope traceAcquire(cpu_trace::AcquireKind::EXECUTION_MUTEX);
        const auto timelineMutex = profiler::frame_timeline::Begin(profiler::frame_timeline::Event::EXECUTION_MUTEX);
        std::lock_guard executionLock(m_executionMutex);
        profiler::frame_timeline::End(timelineMutex);
        traceAcquire.Acquired();
        WaitForSubmissionSlot();
        std::vector<std::function<void()>> commands;
        {
            std::lock_guard lock(m_commandsMutex);
            if (m_commands.empty()) { cpu_trace::Count(cpu_trace::Counter::EMPTY_SUBMIT); return false; }
            commands = std::move(m_commands);
            m_commands = m_commandStorage.Acquire();
            m_resourceUpdatesOnly = true;
        }
        const auto serial = ++m_submissionSerial;
        auto timelineContext = profiler::frame_timeline::CurrentContext();
        timelineContext.submissionSerial = serial;
        timelineContext.targetSerial = serial;
        const profiler::frame_timeline::ContextScope timelineOrigin(timelineContext);
        profiler::frame_timeline::Mark(profiler::frame_timeline::Event::PACKET_SUBMIT, {}, commands.size());
        cpu_trace::BindPacket(&m_thread, serial, commands.size());
        FBZZ_PROFILE_SCOPE("ThreadedRenderer::SubmitPacket");
        FBZZ_PROFILE_SCOPE("ThreadedRenderer::PacketAcceptance");
        const cpu_trace::WaitKindScope traceWait(cpu_trace::WaitKind::ACCEPTANCE);
        const profiler::frame_timeline::Scope timelineWait(profiler::frame_timeline::Event::WAIT_ACCEPTANCE);
        const bool accepted = m_thread.Submit(serial, std::make_unique<RenderCommands>(std::move(commands), m_commandStorage));
        cpu_trace::PacketAccepted(&m_thread, serial, accepted);
        assert(accepted);
        return accepted;
    }
    void WaitForSubmissionSlot()
    {
        FBZZ_PROFILE_SCOPE("ThreadedRenderer::SubmitCommandsBackpressure");
        const cpu_trace::WaitKindScope traceWait(cpu_trace::WaitKind::SLOT);
        auto context = profiler::frame_timeline::CurrentContext();
        context.targetSerial = m_submissionSerial;
        const profiler::frame_timeline::Scope timelineWait(profiler::frame_timeline::Event::WAIT_SLOT, context);
        m_thread.WaitIdle();
    }
    void Flush()
    {
        FBZZ_PROFILE_SCOPE("ThreadedRenderer::Flush");
        while (SubmitCommands()) {
            const cpu_trace::WaitKindScope traceWait(cpu_trace::WaitKind::DRAIN);
            auto context = profiler::frame_timeline::CurrentContext();
            context.targetSerial = m_submissionSerial;
            const profiler::frame_timeline::Scope timelineWait(profiler::frame_timeline::Event::WAIT_DRAIN, context);
            m_thread.WaitIdle();
        }
    }

    std::unique_ptr<IBuffer> CreateNativeVertexBuffer(const void* data, size_t bytes, uint32_t stride) override
    { return Sync([&] { return m_native->CreateNativeVertexBuffer(data, bytes, stride); }); }
    std::unique_ptr<IBuffer> CreateNativeGpuWritableVertexBuffer(size_t bytes, uint32_t stride) override
    { return Sync([&] { return m_native->CreateNativeGpuWritableVertexBuffer(bytes, stride); }); }
    std::unique_ptr<IBuffer> CreateNativeIndexBuffer(const void* data, uint32_t count) override
    { return Sync([&] { return m_native->CreateNativeIndexBuffer(data, count); }); }
    std::unique_ptr<IConstantBuffer> CreateNativeConstantBuffer(size_t bytes) override
    { return Sync([&] { return m_native->CreateNativeConstantBuffer(bytes); }); }
    std::unique_ptr<IShader> CreateNativeShader(const std::string& path) override
    { return Sync([&] { return m_native->CreateNativeShader(path); }); }
    std::unique_ptr<ITexture> CreateNativeTexture(const std::string& path) override
    { return Sync([&] { return m_native->CreateNativeTexture(path); }); }
    std::unique_ptr<ITexture> CreateNativeTextureFromData(const uint8_t* data, uint32_t width, uint32_t height) override
    { return Sync([&] { return m_native->CreateNativeTextureFromData(data, width, height); }); }
    std::unique_ptr<ITexture> CreateNativeTextureFromDataMips(const TextureMipData* data, uint32_t count) override
    { return Sync([&] { return m_native->CreateNativeTextureFromDataMips(data, count); }); }
    std::unique_ptr<ITexture> CreateNativeTextureFromDataMipsAsync(const TextureMipData* data, uint32_t count, uint64_t& token) override
    { return Sync([&] { return m_native->CreateNativeTextureFromDataMipsAsync(data, count, token); }); }
    bool IsUploadComplete(uint64_t token) const override
    { return const_cast<ThreadedRenderer*>(this)->Sync([&] { return m_native->IsUploadComplete(token); }); }
    std::unique_ptr<ITexture> CreateNativeTexture3DFromData(const uint8_t* data, uint32_t width, uint32_t height, uint32_t depth) override
    { return Sync([&] { return m_native->CreateNativeTexture3DFromData(data, width, height, depth); }); }
    std::unique_ptr<ITexture> CreateNativeTextureFromRenderTarget(IRenderTarget& target, uint32_t index, RenderTargetTextureKind kind) override
    { return Sync([&] { return m_native->CreateNativeTextureFromRenderTarget(target, index, kind); }); }
    std::unique_ptr<IPipelineState> CreateNativePipelineState(const PipelineStateDesc& desc) override
    { return Sync([&] { return m_native->CreateNativePipelineState(desc); }); }
    std::unique_ptr<IRenderTarget> CreateNativeRenderTarget(uint32_t width, uint32_t height, const RenderTargetDesc& desc) override
    { return Sync([&] { return m_native->CreateNativeRenderTarget(width, height, desc); }); }
    std::unique_ptr<IRenderTarget> CreateNativeCubemapRenderTarget(uint32_t size, uint32_t mips) override
    { return Sync([&] { return m_native->CreateNativeCubemapRenderTarget(size, mips); }); }
    std::unique_ptr<ITexture> CreateNativeCubeTextureFromRenderTarget(IRenderTarget& target) override
    { return Sync([&] { return m_native->CreateNativeCubeTextureFromRenderTarget(target); }); }
    std::unique_ptr<ITexture> CreateNativeComputeTexture(uint32_t width, uint32_t height) override
    { return Sync([&] { return m_native->CreateNativeComputeTexture(width, height); }); }
    std::unique_ptr<ITexture> CreateNativeComputeTexture3D(uint32_t width, uint32_t height, uint32_t depth) override
    { return Sync([&] { return m_native->CreateNativeComputeTexture3D(width, height, depth); }); }
    std::unique_ptr<ITexture> CreateNativeDynamicTexture(uint32_t width, uint32_t height, DynamicTextureFormat format) override
    { return Sync([&] { return m_native->CreateNativeDynamicTexture(width, height, format); }); }
    std::unique_ptr<IStructuredBuffer> CreateNativeStructuredBuffer(const void* data, uint32_t count, uint32_t stride) override
    { return Sync([&] { return m_native->CreateNativeStructuredBuffer(data, count, stride); }); }
    std::unique_ptr<IStructuredBuffer> CreateNativeGpuLocalStructuredBuffer(const void* data, uint32_t count, uint32_t stride) override
    { return Sync([&] { return m_native->CreateNativeGpuLocalStructuredBuffer(data, count, stride); }); }
    std::unique_ptr<IStructuredBuffer> CreateNativeRWStructuredBuffer(const void* data, uint32_t count, uint32_t stride) override
    { return Sync([&] { return m_native->CreateNativeRWStructuredBuffer(data, count, stride); }); }
    bool PrepareShaderReload() override { return Sync([this] { return m_native->PrepareShaderReload(); }); }
    std::unique_ptr<IAccelerationStructure> CreateNativeAccelerationStructure(const AccelerationStructureDesc& desc, ResourceManager& resources) override
    { return Sync([&] { return m_native->CreateNativeAccelerationStructure(desc, resources); }); }

    std::unique_ptr<IRenderer> m_native;
    /// @note 宣言順により worker の Stop/join が storage 破棄より先になる。
    CommandStoragePool m_commandStorage;
    RenderThread m_thread;
    GraphicsCapabilities m_capabilities;
    bool m_supportsIndirectDraw = false;
    bool m_supportsMeshShaders = false;
    std::vector<std::function<void()>> m_commands;
    uint64_t m_applicationSerial = 0;
    uint64_t m_workerFrameTimeline = 0;
    /// @note Worker-only state spans synchronous packet splits; failed nested views suppress only their own markers.
    std::vector<bool> m_recordedGpuViews;
    uint64_t m_submissionSerial = 0;
    std::atomic<uint64_t> m_bufferReadRevision{0};
    const std::thread::id m_producerThread = std::this_thread::get_id();
    uint32_t m_width = 0, m_height = 0;
    bool m_vsync = false;
    bool m_cpuFrameOpen = false;
    bool m_resourceUpdatesOnly = true;
    bool m_synchronousActive = false;
    bool m_resizePending = false;
    bool m_shutdown = false;
    std::mutex m_commandsMutex;
    std::recursive_mutex m_executionMutex;
    mutable std::mutex m_snapshotMutex;
    GpuProfilerSnapshot m_completedGpuSnapshot;
    std::vector<GpuPassProfile> m_completedGpuResults;
    mutable GpuProfilerSnapshot m_gpuSnapshot;
    mutable std::vector<GpuPassProfile> m_gpuResults;
};

namespace {

class ThreadedIblBaker final : public IIblBaker {
public:
    ThreadedIblBaker(ThreadedRenderer& owner, std::unique_ptr<IIblBaker> native)
        : m_owner(owner), m_native(std::move(native)) {}
    ~ThreadedIblBaker() override { m_owner.Sync([this] { m_native.reset(); }); }
    bool Bake(const IblBakeInput& input, const std::string& directory, const std::string& name, IblBakeOutput& output) override
    { return m_owner.Sync([&] { return m_native->Bake(input, directory, name, output); }); }
private:
    ThreadedRenderer& m_owner;
    std::unique_ptr<IIblBaker> m_native;
};

class ThreadedImGuiRenderer final : public IImGuiRenderer {
public:
    ThreadedImGuiRenderer(ThreadedRenderer& owner, std::unique_ptr<IImGuiRenderer> native)
        : m_owner(owner), m_native(std::move(native)) {}
    ~ThreadedImGuiRenderer() override { ImGuiShutdown(); }
    void ImGuiInit(void* window) override
    {
        if (m_initialized) return;
        m_frontendContext = ImGui::GetCurrentContext();
        m_native->ImGuiInit(window);
        void* backend = ImGui::GetIO().BackendRendererUserData;
        const auto flags = ImGui::GetIO().BackendFlags;
        const auto config = ImGui::GetIO().ConfigFlags;
        const auto reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
        auto& platform = ImGui::GetPlatformIO();
        m_createWindow = platform.Renderer_CreateWindow;
        m_destroyWindow = platform.Renderer_DestroyWindow;
        m_resizeWindow = platform.Renderer_SetWindowSize;
        m_renderWindow = platform.Renderer_RenderWindow;
        m_swapBuffers = platform.Renderer_SwapBuffers;
        m_owner.Sync([&] {
            const ImGuiContextScope scope(nullptr);
            m_renderContext.reset(ImGui::CreateContext());
            ImGui::SetCurrentContext(m_renderContext.get());
            ImGui::GetIO().BackendRendererUserData = backend;
            ImGui::GetIO().BackendFlags = flags;
            ImGui::GetIO().ConfigFlags = config;
            ImGui::GetPlatformIO().DrawCallback_ResetRenderState = reset;
        });
        PlatformOwners().emplace(m_frontendContext, this);
        if (m_createWindow) platform.Renderer_CreateWindow = &CreatePlatformWindow;
        if (m_destroyWindow) platform.Renderer_DestroyWindow = &DestroyPlatformWindow;
        if (m_resizeWindow) platform.Renderer_SetWindowSize = &ResizePlatformWindow;
        m_initialized = true;
    }
    void ImGuiShutdown() override
    {
        if (!m_initialized) return;
        m_owner.WaitForCpuFrame();
        auto& platform = ImGui::GetPlatformIO();
        platform.Renderer_CreateWindow = m_createWindow;
        platform.Renderer_DestroyWindow = m_destroyWindow;
        platform.Renderer_SetWindowSize = m_resizeWindow;
        PlatformOwners().erase(m_frontendContext);
        m_native->ImGuiShutdown();
        m_owner.Sync([this] {
            if (!m_renderContext) return;
            ImGui::SetCurrentContext(m_renderContext.get());
            ImGui::GetIO().BackendRendererUserData = nullptr;
            m_renderContext.reset();
            ImGui::SetCurrentContext(nullptr);
        });
        {
            const ImGuiContextScope scope(nullptr);
            m_drawDataPool.clear();
            m_platformDataPool.clear();
        }
        m_textureIds.clear();
        m_initialized = false;
    }
    void ImGuiNewFrame() override
    {
        if (!m_initialized) return;
        m_owner.Record([this] {
            const ImGuiContextScope scope(m_renderContext.get());
            m_native->ImGuiPrepareGpuFrame();
        }, false, OpaqueQueueOrigin::IMGUI_GPU_PREPARE);
        m_native->ImGuiPreparePlatformFrame();
    }
    void ImGuiRenderDrawData() override
    {
        ImDrawData* source = ImGui::GetDrawData();
        if (!m_initialized || !source) return;
        if (source->Textures) {
            bool dirty = false;
            for (const auto* texture : *source->Textures)
                dirty |= texture && texture->Status != ImTextureStatus_OK;
            if (dirty) m_owner.Sync([this, source] {
                const ImGuiContextScope scope(m_renderContext.get());
                m_native->ImGuiPrepareTextures(*source);
            });
        }
        /// @note 任意の callback の Scene 参照や引数寿命は一般化できないため、既存 callback は producer で同期実行する。
        if (HasUserCallbacks(*source)) {
            m_owner.WaitForCpuFrame();
            m_native->ImGuiRenderDrawData();
            return;
        }
        auto data = AcquireGuiSnapshot(m_drawDataPool);
        data->Copy(*source);
        m_owner.Record([this, data = std::move(data)] {
            const ImGuiContextScope scope(m_renderContext.get());
            m_native->ImGuiRenderDrawData(data->data);
            data->completed.store(true, std::memory_order_release);
        });
    }
    void ImGuiRenderPlatformWindows() override
    {
        if (!m_initialized || (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0) return;
        /// @note 最後の追加 Window が消えたフレームも UpdatePlatformWindows を呼び、native 資源の破棄を確定する。
        m_native->ImGuiPreparePlatformWindows();
        const auto& platform = ImGui::GetPlatformIO();
        if (platform.Viewports.Size <= 1) return;
        bool producerCallbacks = platform.Platform_RenderWindow || platform.Platform_SwapBuffers;
        for (int index = 1; index < platform.Viewports.Size; ++index) {
            const auto* viewport = platform.Viewports[index];
            if (viewport->DrawData) producerCallbacks |= HasUserCallbacks(*viewport->DrawData);
        }
        if (producerCallbacks) {
            m_owner.WaitForCpuFrame();
            m_native->ImGuiRenderPreparedPlatformWindows();
            return;
        }
        auto frame = AcquireGuiSnapshot(m_platformDataPool);
        frame->count = 0;
        for (int index = 1; index < platform.Viewports.Size; ++index) {
            const auto* viewport = platform.Viewports[index];
            if (!viewport->DrawData || (viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0) continue;
            if (frame->count == frame->viewports.size()) frame->viewports.push_back(std::make_unique<OwnedImGuiDrawData>());
            frame->viewports[frame->count++]->Copy(*viewport->DrawData);
        }
        m_owner.Record([this, frame = std::move(frame)] {
            const ImGuiContextScope scope(m_renderContext.get());
            for (size_t index = 0; index < frame->count; ++index)
                if (m_renderWindow) m_renderWindow(&frame->viewports[index]->viewport, nullptr);
            for (size_t index = 0; index < frame->count; ++index)
                if (m_swapBuffers) m_swapBuffers(&frame->viewports[index]->viewport, nullptr);
            frame->completed.store(true, std::memory_order_release);
        });
    }
    void* GetImTextureID(ResourceHandle<RenderTargetTag> handle, ResourceManager& resources, int slot) override
    {
        const TextureKey key{&resources, handle.id, handle.gen, slot, true};
        if (!resources.Get(handle)) { m_textureIds.erase(key); return nullptr; }
        if (const auto found = m_textureIds.find(key); found != m_textureIds.end()) {
            m_owner.Record([this, handle, &resources, slot] { (void)m_native->GetImTextureID(handle, resources, slot); });
            return found->second;
        }
        void* id = m_owner.Sync([&] { return m_native->GetImTextureID(handle, resources, slot); });
        if (id) m_textureIds.emplace(key, id);
        return id;
    }
    void* GetImTextureID(ResourceHandle<TextureTag> handle, ResourceManager& resources) override
    {
        const TextureKey key{&resources, handle.id, handle.gen, 0, false};
        if (!resources.Get(handle)) { m_textureIds.erase(key); return nullptr; }
        if (const auto found = m_textureIds.find(key); found != m_textureIds.end()) {
            m_owner.Record([this, handle, &resources] { (void)m_native->GetImTextureID(handle, resources); });
            return found->second;
        }
        void* id = m_owner.Sync([&] { return m_native->GetImTextureID(handle, resources); });
        if (id) m_textureIds.emplace(key, id);
        return id;
    }
private:
    static bool HasUserCallbacks(const ImDrawData& data)
    {
        for (const auto* list : data.CmdLists)
            for (const auto& command : list->CmdBuffer)
                if (command.UserCallback && command.UserCallback != ImDrawCallback_ResetRenderState) return true;
        return false;
    }
    static std::unordered_map<ImGuiContext*, ThreadedImGuiRenderer*>& PlatformOwners()
    {
        static thread_local std::unordered_map<ImGuiContext*, ThreadedImGuiRenderer*> owners;
        return owners;
    }
    static ThreadedImGuiRenderer& PlatformOwner()
    {
        const auto found = PlatformOwners().find(ImGui::GetCurrentContext());
        assert(found != PlatformOwners().end());
        return *found->second;
    }
    /// @note native viewport の RendererUserData は snapshot が参照する。作成・破棄・Resize だけ worker 完了を待つ。
    /// @see https://github.com/ocornut/imgui/blob/v1.92.8/imgui.h ImGuiPlatformIO renderer lifecycle callbacks.
    static void CreatePlatformWindow(ImGuiViewport* viewport)
    {
        auto& owner = PlatformOwner();
        owner.m_owner.Sync([&] { const ImGuiContextScope scope(owner.m_renderContext.get()); owner.m_createWindow(viewport); });
    }
    static void DestroyPlatformWindow(ImGuiViewport* viewport)
    {
        auto& owner = PlatformOwner();
        owner.m_owner.Sync([&] { const ImGuiContextScope scope(owner.m_renderContext.get()); owner.m_destroyWindow(viewport); });
    }
    static void ResizePlatformWindow(ImGuiViewport* viewport, ImVec2 size)
    {
        auto& owner = PlatformOwner();
        owner.m_owner.Sync([&] { const ImGuiContextScope scope(owner.m_renderContext.get()); owner.m_resizeWindow(viewport, size); });
    }
    struct TextureKey {
        const ResourceManager* resources;
        uint32_t id, generation;
        int slot;
        bool target;
        bool operator==(const TextureKey&) const = default;
    };
    struct TextureKeyHash {
        size_t operator()(const TextureKey& key) const
        {
            return std::hash<const void*>{}(key.resources) ^ static_cast<size_t>(key.id)
                ^ (static_cast<size_t>(key.generation) << 1) ^ (static_cast<size_t>(key.slot) << 17)
                ^ (static_cast<size_t>(key.target) << 25);
        }
    };
    ThreadedRenderer& m_owner;
    std::unique_ptr<IImGuiRenderer> m_native;
    std::unordered_map<TextureKey, void*, TextureKeyHash> m_textureIds;
    std::vector<std::shared_ptr<OwnedImGuiDrawData>> m_drawDataPool;
    std::vector<std::shared_ptr<OwnedImGuiPlatformFrame>> m_platformDataPool;
    void (*m_createWindow)(ImGuiViewport*) = nullptr;
    void (*m_destroyWindow)(ImGuiViewport*) = nullptr;
    void (*m_resizeWindow)(ImGuiViewport*, ImVec2) = nullptr;
    void (*m_renderWindow)(ImGuiViewport*, void*) = nullptr;
    void (*m_swapBuffers)(ImGuiViewport*, void*) = nullptr;
    ImGuiContext* m_frontendContext = nullptr;
    std::unique_ptr<ImGuiContext, ImGuiContextDeleter> m_renderContext;
    bool m_initialized = false;
};

} /// @note namespace

std::unique_ptr<IIblBaker> ThreadedRenderer::CreateIblBaker()
{
    auto native = Sync([this] { return m_native->CreateIblBaker(); });
    return native ? std::make_unique<ThreadedIblBaker>(*this, std::move(native)) : nullptr;
}

void gpu_cull_detail::SubmitCullDraw(IRenderer& renderer, ResourceManager& resources,
    const ComputeCall& compute, const DrawCall& indirect, const DrawCall& fallback)
{
    ThreadedRenderer* owner = nullptr;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        for (const auto& entry : registry.entries) if (entry.facade == &renderer) { owner = entry.owner; break; }
    }
    if (owner) owner->CullDraw(compute, indirect, fallback, resources);
    else if (!CullDrawRangeCurrent(indirect, resources)) return;
    else if (renderer.TryDispatch(compute, resources)) {
        const cpu_trace::SourceScope gpuDrawTrace(cpu_trace::Source::UNKNOWN, "GpuIndirect::NativeDraw");
                FBZZ_PROFILE_SCOPE("Render::GpuIndirect::NativeDraw"); renderer.Submit(indirect, resources);
    } else {
        const cpu_trace::SourceScope gpuFallbackTrace(cpu_trace::Source::UNKNOWN, "GpuIndirect::NativeFallback");
                FBZZ_PROFILE_SCOPE("Render::GpuIndirect::NativeFallback"); renderer.Submit(fallback, resources);
    }
}

bool material_transfer_detail::IsRegisteredOwnedTransferProducer(IRenderer& renderer)
{
    /// @note Optional submit holds this same registry through allocations; nested queries must fail before self try_lock.
    if (s_resourceSubmitActive) return false;
    const SynchronousScope queryScope(s_resourceSubmitActive);
    auto& registry = Registry();
    std::unique_lock lock(registry.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    for (const auto& entry : registry.entries)
        if (entry.facade == &renderer) return !entry.owner->IsRenderThread();
    return false;
}

bool buffer_state_detail::TryObserveIdleReadBoundary(IRenderer& renderer, uint64_t& submission, uint64_t& revision)
{
    if (s_resourceSubmitActive) return false;
    const SynchronousScope queryScope(s_resourceSubmitActive);
    auto& registry = Registry();
    std::unique_lock lock(registry.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    for (const auto& entry : registry.entries)
        if (entry.facade == &renderer) return entry.owner->TryObserveIdleReadBoundary(submission, revision);
    return false;
}

std::unique_ptr<prepared_skinning_detail::NativeWork> prepared_skinning_detail::PrepareViaFacade(
    IRenderer& facade, ResourceManager& resources, ResourceHandle<ShaderTag> shader,
    std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices)
{
    ThreadedRenderer* owner = nullptr;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        for (const auto& entry : registry.entries) if (entry.facade == &facade) { owner = entry.owner; break; }
    }
    return owner ? owner->PrepareSkinning(resources, shader, requests, matrices)
                 : PrepareExactNative(facade, resources, shader, requests, matrices);
}

bool prepared_skinning_detail::SupportsViaFacade(const IRenderer& facade) noexcept
{
    ThreadedRenderer* owner = nullptr;
    {
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        for (const auto& entry : registry.entries) if (entry.facade == &facade) { owner = entry.owner; break; }
    }
    return owner ? owner->SupportsPreparedSkinning() : HasRegisteredBackend(facade);
}

CpuShadowTransferScope::CpuShadowTransferScope(IRenderer& renderer) noexcept
    : m_previous(std::exchange(s_cpuShadowTransfer, &renderer)) {}
CpuShadowTransferScope::~CpuShadowTransferScope() { s_cpuShadowTransfer = m_previous; }

bool TrySubmitPendingResourceUpdates(IRenderer& renderer)
{
    const cpu_trace::SourceScope traceAttempt(cpu_trace::Source::UNKNOWN, "AsyncResourceSubmit::Attempt");
    if (s_resourceSubmitActive) {
        const cpu_trace::SourceScope traceOutcome(cpu_trace::Source::UNKNOWN, "AsyncResourceSubmit::Reentry");
        return false;
    }
    const SynchronousScope attemptScope(s_resourceSubmitActive);
    auto& registry = Registry();
    std::unique_lock registryLock(registry.mutex, std::try_to_lock);
    auto outcome = ResourceSubmitOutcome::UNKNOWN;
    if (!registryLock.owns_lock()) outcome = ResourceSubmitOutcome::BUSY;
    else for (const auto& entry : registry.entries) if (entry.facade == &renderer) {
        outcome = entry.owner->TrySubmitResourceUpdates();
        break;
    }
    const cpu_trace::SourceScope traceOutcome(cpu_trace::Source::UNKNOWN,
        RESOURCE_SUBMIT_LABELS[static_cast<size_t>(outcome)]);
    return outcome == ResourceSubmitOutcome::SUCCESS;
}

RendererBundle EnableRenderThread(RendererBundle native)
{
    if (!native.renderer || !native.imguiRenderer) return {};
    auto renderer = std::make_unique<ThreadedRenderer>(std::move(native.renderer));
    auto imgui = std::make_unique<ThreadedImGuiRenderer>(*renderer, std::move(native.imguiRenderer));
    return {std::move(renderer), std::move(imgui)};
}

} /// @note namespace fbzz::renderer
