/// @file    DX12PreparedSkinning.cpp
/// @brief   元入力を閉じた DIRECT 列へ記録し、需要が発生した時だけ投入する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#include "DX12PreparedSkinning.hpp"
#include "DX12Buffer.hpp"
#include "DX12Context.hpp"
#include "DX12PsoCache.hpp"
#include "DX12Shader.hpp"
#include "DX12StateTracker.hpp"
#include <Graphics/Renderer/BindlessIndices.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/ExternalCallbackBoundary.hpp>
#include <Core/Profiler/FrameTimeline.hpp>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <utility>

namespace fbzz::renderer {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t MAX_SLOTS = 16;
constexpr size_t MAX_BYTES = 256u * 1024u * 1024u;
constexpr size_t MAX_TICKET_BYTES = 32u * 1024u * 1024u;
size_t Align256(size_t bytes) { return (bytes + 255u) & ~size_t{255}; }

/// @note 分割 list の途中 fence ではなく、physical frame の最後の提出だけが値を確定する。
struct FrameRetirement { std::atomic<uint64_t> actualFence{0}; };

struct OutputLease {
    ResourceHandle<BufferTag> handle;
    ComPtr<ID3D12Resource> resource;
    std::shared_ptr<prepared_skinning_detail::BufferWriteVersion> version;
    std::shared_ptr<prepared_skinning_detail::ResourceOwner> owner;
    uint32_t bytes = 0, srv = INVALID_BINDLESS_INDEX;
};

/// @note Native 入力の完全一致世代だけを所有する。出力や GPU 成功状態は保持しない。
struct SourceSnapshot {
    ResourceHandle<BufferTag> source;
    uint64_t resourceInstance = 0, resourceEpoch = 0, contentVersion = 0;
    uint32_t vertexCount = 0, largestBone = 0;
    std::vector<uint8_t> bytes;
};

struct PreparedSlot {
    std::atomic<bool> leased{false};
    std::atomic<uint32_t> submitted{0};
    uint64_t fenceValue = 0, nextFenceValue = 1;
    std::shared_ptr<prepared_skinning_detail::ResourceOwner> owner;
    std::shared_ptr<FrameRetirement> frameRetirement;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12RootSignature> root;
    size_t uploadCapacity = 0;
    std::array<SourceSnapshot, 4> sourceSnapshots;
    size_t nextSourceSnapshot = 0;
    uint32_t descriptorCapacity = 0;
    std::vector<OutputLease> outputs;
    std::vector<PreparedSkinningWrite> writes;
};

bool CreateUpload(ID3D12Device* device, size_t bytes, ComPtr<ID3D12Resource>& resource)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)));
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
} /// @note namespace

/// @note Slot は ticket と描画 fence の両方が退役した後だけ再利用する。Prepare と停止は描画 executor 上で直列。
/// @see https://microsoft.github.io/DirectX-Specs/d3d/CPUEfficiency.html Command lists do not retain resource references.
class DX12PreparedSkinningPool final {
public:
    explicit DX12PreparedSkinningPool(DX12Context& context) : device(context.GetDevice()), queue(context.GetCommandQueue()),
        computeQueue(context.GetComputeCommandQueue()), frameFence(context.GetFrameRetirementFence()), heap(context.GetResourceSrvHeap())
    { for (auto& marker : frameMarkers) marker = std::make_shared<FrameRetirement>(); }
    std::atomic<bool> alive{true};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandQueue> computeQueue;
    ComPtr<ID3D12Fence> frameFence;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::array<PreparedSlot, MAX_SLOTS> slots;
    std::array<std::shared_ptr<FrameRetirement>, MAX_SLOTS + 1> frameMarkers;
    std::atomic<std::shared_ptr<FrameRetirement>> currentFrame;
    std::atomic<bool> frameOpen{false};
};

namespace {
class DX12PreparedWork final : public prepared_skinning_detail::NativeWork {
public:
    DX12PreparedWork(std::shared_ptr<DX12PreparedSkinningPool> pool, size_t index)
        : m_pool(std::move(pool)), m_index(index) {}
    ~DX12PreparedWork() override { m_pool->slots[m_index].leased.store(false, std::memory_order_release); }
    bool Submit() noexcept override
    {
        auto& slot = m_pool->slots[m_index];
        if (!m_pool->alive.load(std::memory_order_acquire) || !slot.owner
            || !slot.owner->alive.load(std::memory_order_acquire)) return false;
        uint32_t expected = 0;
        if (!slot.submitted.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            if (expected != 2) return false;
            for (size_t i = 0; i < writes.size(); ++i)
                if (!writes[i].recorded || slot.outputs[i].version->value.load(std::memory_order_acquire) != writes[i].resultingVersion)
                    return false;
            auto currentFrame = m_pool->currentFrame.load(std::memory_order_acquire);
            if (currentFrame) slot.frameRetirement = std::move(currentFrame);
            else if (m_pool->frameOpen.load(std::memory_order_acquire)) return false;
            return true;
        }
        for (size_t i = 0; i < writes.size(); ++i) {
            if (slot.outputs[i].version->value.load(std::memory_order_acquire) != writes[i].previousVersion) {
                slot.submitted.store(3, std::memory_order_release);
                return false;
            }
        }
        slot.frameRetirement = m_pool->currentFrame.load(std::memory_order_acquire);
        if (!slot.frameRetirement && m_pool->frameOpen.load(std::memory_order_acquire)) {
            slot.submitted.store(3, std::memory_order_release);
            return false;
        }
        /// @note 専用 fence は Context の未提出 main list の退役値を完了させない。
        /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization Fence timelines.
        ID3D12CommandList* lists[] = {slot.commands.Get()};
        const profiler::frame_timeline::Scope timelineSubmit(profiler::frame_timeline::Event::QUEUE_SUBMIT, {}, writes.size());
        m_pool->queue->ExecuteCommandLists(1, lists);
        slot.fenceValue = slot.nextFenceValue++;
        if (FAILED(m_pool->queue->Signal(slot.fence.Get(), slot.fenceValue))) {
            /// @note 投入済みの失敗は再利用しない。Logger/待機を呼べない境界なので呼出し元が full fallback を選ぶ。
            slot.fenceValue = (std::numeric_limits<uint64_t>::max)();
            slot.submitted.store(3, std::memory_order_release);
            return false;
        }
        /// @note private 列は CPU input だけを読み、開いた main/compute list に依存しない。後続 compute 消費だけをこの private fence へ繋ぐ。
        if (m_pool->computeQueue && FAILED(m_pool->computeQueue->Wait(slot.fence.Get(), slot.fenceValue))) {
            slot.submitted.store(3, std::memory_order_release);
            return false;
        }
        for (size_t i = 0; i < writes.size(); ++i) {
            writes[i].resultingVersion = writes[i].previousVersion + 1u;
            slot.outputs[i].version->value.store(writes[i].resultingVersion, std::memory_order_release);
            writes[i].recorded = true;
        }
        slot.submitted.store(2, std::memory_order_release);
        return true;
    }
    const void* OutputResource(size_t row) const noexcept override
    { return row < writes.size() && m_pool->alive.load(std::memory_order_acquire)
        && m_pool->slots[m_index].owner && m_pool->slots[m_index].owner->alive.load(std::memory_order_acquire)
        && m_pool->slots[m_index].submitted.load(std::memory_order_acquire) == 2
        && writes[row].recorded && m_pool->slots[m_index].outputs[row].version->value.load(std::memory_order_acquire) == writes[row].resultingVersion
        ? m_pool->slots[m_index].outputs[row].resource.Get() : nullptr; }
    uint64_t OutputAddress(size_t row) const noexcept override
    { const auto* resource = static_cast<const ID3D12Resource*>(OutputResource(row)); return resource ? const_cast<ID3D12Resource*>(resource)->GetGPUVirtualAddress() : 0; }
    uint32_t OutputBytes(size_t row) const noexcept override
    { return row < writes.size() && writes[row].recorded ? m_pool->slots[m_index].outputs[row].bytes : 0; }
    uint32_t OutputSrv(size_t row) const noexcept override
    { return row < writes.size() && writes[row].recorded ? m_pool->slots[m_index].outputs[row].srv : INVALID_BINDLESS_INDEX; }
    const void* DeviceIdentity() const noexcept override { return m_pool->device.Get(); }
private:
    std::shared_ptr<DX12PreparedSkinningPool> m_pool;
    size_t m_index;
};
} /// @note namespace

std::shared_ptr<DX12PreparedSkinningPool> CreatePreparedSkinningPool(DX12Context& context)
{
    auto pool = std::make_shared<DX12PreparedSkinningPool>(context);
    if (context.IsFrameOpen()) BeginPreparedSkinningFrame(pool);
    return pool;
}

void StopPreparedSkinningPool(const std::shared_ptr<DX12PreparedSkinningPool>& pool)
{
    if (pool) pool->alive.store(false, std::memory_order_release);
}

void BeginPreparedSkinningFrame(const std::shared_ptr<DX12PreparedSkinningPool>& pool)
{
    if (!pool) return;
    pool->frameOpen.store(true, std::memory_order_release);
    pool->currentFrame.store({}, std::memory_order_release);
    for (const auto& marker : pool->frameMarkers) {
        /// @note 参照数は marker の CPU 値の所有だけを判定する。native slot の再利用は別途両 fence の完了を必須とする。
        if (marker.use_count() == 1) {
            marker->actualFence.store(0, std::memory_order_release);
            pool->currentFrame.store(marker, std::memory_order_release);
            return;
        }
    }
}

void EndPreparedSkinningFrame(const std::shared_ptr<DX12PreparedSkinningPool>& pool, uint64_t actualFence)
{
    if (!pool) return;
    if (auto marker = pool->currentFrame.exchange({}, std::memory_order_acq_rel))
        marker->actualFence.store(actualFence ? actualFence : UINT64_MAX, std::memory_order_release);
    pool->frameOpen.store(false, std::memory_order_release);
}

std::unique_ptr<prepared_skinning_detail::NativeWork> PrepareDX12Skinning(
    const std::shared_ptr<DX12PreparedSkinningPool>& pool, DX12Context& context,
    DX12PsoCache& pipelines, DX12StateTracker& tracker, ResourceManager& resources,
    ResourceHandle<ShaderTag> shaderHandle, std::span<const CapturedSkinningRequest> requests,
    std::span<const math::Matrix4> matrices)
{
    if (!pool || !pool->alive.load(std::memory_order_acquire) || !context.SupportsBindless()
        || !context.IsFrameOpen() || context.IsComputeListOpen() || requests.empty() || requests.size() > 256) return {};
    bool callbackObserved = false;
    const core::ExternalCallbackScope callbackScope([](void* state) {
        *static_cast<bool*>(state) = true;
        return false;
    }, &callbackObserved);
    auto owner = prepared_skinning_detail::AcquireResourceOwner(resources);
    if (!owner) return {};
    auto* shader = static_cast<DX12Shader*>(resources.Get(shaderHandle));
    if (callbackObserved || !shader || !shader->IsCompute() || !shader->GetPath().ends_with("SkinningCompute.cs.hlsl")) return {};
    auto* pso = pipelines.GetOrCreateCompute(*shader);
    if (callbackObserved || !pso || !owner->alive.load(std::memory_order_acquire)) return {};
    size_t index = MAX_SLOTS;
    size_t totalBytes = 0;
    for (size_t i = 0; i < pool->slots.size(); ++i) {
        auto& candidate = pool->slots[i];
        totalBytes += candidate.uploadCapacity;
        for (const auto& snapshot : candidate.sourceSnapshots) totalBytes += snapshot.bytes.capacity();
        for (const auto& output : candidate.outputs) totalBytes += output.bytes;
        if (index == MAX_SLOTS && !candidate.leased.load(std::memory_order_acquire)
            && (!candidate.frameRetirement || (candidate.frameRetirement->actualFence.load(std::memory_order_acquire) != 0
                && candidate.frameRetirement->actualFence.load(std::memory_order_acquire) != UINT64_MAX
                && pool->frameFence->GetCompletedValue() >= candidate.frameRetirement->actualFence.load(std::memory_order_acquire)))
            && (candidate.fenceValue == 0 || (candidate.fenceValue != (std::numeric_limits<uint64_t>::max)()
                && candidate.fence && candidate.fence->GetCompletedValue() >= candidate.fenceValue))) index = i;
    }
    if (index == MAX_SLOTS) return {};
    auto& slot = pool->slots[index];
    struct SourceLayout { ResourceHandle<BufferTag> handle; uint64_t version = 0; uint32_t count = 0, largestBone = 0; size_t offset = 0; bool copied = false; };
    std::array<SourceLayout, 256> sources{};
    std::array<size_t, 256> requestSources{};
    size_t sourceCount = 0;
    size_t inputCursor = Align256(matrices.size_bytes());
    size_t outputBytes = 0;
    for (size_t row = 0; row < requests.size(); ++row) {
        const auto& request = requests[row];
        if (request.outputVertices.IsValid() || !request.vertexCount || request.vertexCount > 65535u * 64u
            || !request.boneCount || request.boneOffset > matrices.size()
            || request.boneCount > matrices.size() - request.boneOffset) return {};
        size_t sourceIndex = 0;
        while (sourceIndex < sourceCount && !(sources[sourceIndex].handle == request.sourceVertices
            && sources[sourceIndex].version == request.sourceVersion && sources[sourceIndex].count == request.vertexCount)) ++sourceIndex;
        if (sourceIndex == sourceCount) {
            inputCursor = (inputCursor + sizeof(SkinnedVertex) - 1u) / sizeof(SkinnedVertex) * sizeof(SkinnedVertex);
            sources[sourceCount++] = {request.sourceVertices, request.sourceVersion, request.vertexCount, 0, inputCursor, false};
            inputCursor += static_cast<size_t>(request.vertexCount) * sizeof(SkinnedVertex);
        }
        requestSources[row] = sourceIndex;
        outputBytes += static_cast<size_t>(request.vertexCount) * sizeof(Vertex);
    }
    const size_t constantsStart = Align256(inputCursor);
    const size_t uploadBytes = constantsStart + requests.size() * 768u;
    if (uploadBytes > MAX_TICKET_BYTES || outputBytes > MAX_TICKET_BYTES
        || totalBytes + uploadBytes + outputBytes > MAX_BYTES) return {};
    size_t accountedBytes = totalBytes + uploadBytes + outputBytes;
    size_t slotSourceBytes = 0;
    for (const auto& snapshot : slot.sourceSnapshots) slotSourceBytes += snapshot.bytes.capacity();
    slot.leased.store(true, std::memory_order_release);
    auto work = std::make_unique<DX12PreparedWork>(pool, index);
    slot.submitted.store(0, std::memory_order_release);
    slot.frameRetirement.reset();
    if (!pool->currentFrame.load(std::memory_order_acquire)) BeginPreparedSkinningFrame(pool);
    slot.owner = owner;
    slot.pipeline = pso;
    slot.root = pipelines.GetComputeRootSignature();
    auto* device = context.GetDevice();
    if (!slot.allocator && FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))) return {};
    if (!slot.fence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)))) return {};
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset GPU completion precedes allocator reuse.
    if (FAILED(slot.allocator->Reset())) return {};
    if (slot.commands) {
        if (FAILED(slot.commands->Reset(slot.allocator.Get(), slot.pipeline.Get()))) return {};
    } else if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
        slot.pipeline.Get(), IID_PPV_ARGS(&slot.commands)))) return {};
    struct RecordingGuard {
        ID3D12GraphicsCommandList* commands;
        bool closed = false;
        ~RecordingGuard() { if (!closed) commands->Close(); }
    } recording{slot.commands.Get()};
    if (slot.uploadCapacity < uploadBytes) {
        slot.upload.Reset();
        if (!CreateUpload(device, uploadBytes, slot.upload)) return {};
        slot.uploadCapacity = uploadBytes;
    }
    if (slot.descriptorCapacity < requests.size() * 3u) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = static_cast<UINT>(requests.size() * 3u);
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        slot.descriptors.Reset();
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&slot.descriptors)))) return {};
        slot.descriptorCapacity = desc.NumDescriptors;
    }
    void* memory = nullptr;
    const D3D12_RANGE readRange{0, 0};
    if (FAILED(slot.upload->Map(0, &readRange, &memory))) return {};
    auto* mapped = static_cast<uint8_t*>(memory);
    std::memcpy(mapped, matrices.data(), matrices.size_bytes());
    const uint32_t increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu = [&](size_t entry) { auto handle = slot.descriptors->GetCPUDescriptorHandleForHeapStart(); handle.ptr += entry * increment; return handle; };
    slot.outputs.resize((std::max)(slot.outputs.size(), requests.size()));
    slot.writes.clear();
    slot.writes.reserve(requests.size());
    ID3D12DescriptorHeap* heaps[] = {slot.descriptors.Get()};
    slot.commands->SetDescriptorHeaps(1, heaps);
    slot.commands->SetComputeRootSignature(slot.root.Get());
    bool valid = true;
    for (size_t i = 0; i < requests.size() && valid; ++i) {
        const auto& request = requests[i];
        auto& capturedSource = sources[requestSources[i]];
        const size_t sourceOffset = capturedSource.offset;
        const size_t sourceBytes = static_cast<size_t>(request.vertexCount) * sizeof(SkinnedVertex);
        if (!capturedSource.copied) {
            auto* source = static_cast<DX12Buffer*>(resources.Get(request.sourceVertices));
            if (callbackObserved || !owner->alive.load(std::memory_order_acquire)
                || !source || source->IsGpuWritable() || source->GetStride() != sizeof(SkinnedVertex)
                || source->GetDataSize() < sourceBytes || source->GetContentVersion() != request.sourceVersion) { valid = false; break; }
            SourceSnapshot* captured = nullptr;
            for (auto& snapshot : slot.sourceSnapshots)
                if (snapshot.source == request.sourceVertices && snapshot.resourceInstance == resources.GetInstanceId()
                    && snapshot.resourceEpoch == resources.GetResetVersion() && snapshot.contentVersion == request.sourceVersion
                    && snapshot.vertexCount == request.vertexCount) { captured = &snapshot; break; }
            if (!captured) {
                captured = &slot.sourceSnapshots[slot.nextSourceSnapshot];
                slot.nextSourceSnapshot = (slot.nextSourceSnapshot + 1u) % slot.sourceSnapshots.size();
                captured->source = {};
                if (captured->bytes.capacity() < sourceBytes) {
                    const size_t oldCapacity = captured->bytes.capacity();
                    const size_t retainedSlotBytes = slotSourceBytes - oldCapacity;
                    const size_t retainedPoolBytes = accountedBytes - oldCapacity;
                    if (sourceBytes > MAX_TICKET_BYTES - retainedSlotBytes || sourceBytes > MAX_BYTES - retainedPoolBytes) { valid = false; break; }
                    /// @note 旧容量を解放してから確保し、vector の実 capacity を公開前に両上限へ照合する。
                    std::vector<uint8_t>().swap(captured->bytes);
                    slotSourceBytes = retainedSlotBytes;
                    accountedBytes = retainedPoolBytes;
                    std::vector<uint8_t> bytes(sourceBytes);
                    if (bytes.capacity() > MAX_TICKET_BYTES - slotSourceBytes || bytes.capacity() > MAX_BYTES - accountedBytes) { valid = false; break; }
                    slotSourceBytes += bytes.capacity();
                    accountedBytes += bytes.capacity();
                    captured->bytes.swap(bytes);
                } else captured->bytes.resize(sourceBytes);
                if (!source->CopyData(0, sourceBytes, captured->bytes.data())
                    || source->GetContentVersion() != request.sourceVersion || !owner->alive.load(std::memory_order_acquire)) { valid = false; break; }
                const auto* vertices = static_cast<const SkinnedVertex*>(static_cast<const void*>(captured->bytes.data()));
                uint32_t largestBone = 0;
                for (uint32_t vertex = 0; vertex < request.vertexCount; ++vertex)
                    for (uint32_t bone : vertices[vertex].boneIndices) largestBone = (std::max)(largestBone, bone);
                captured->resourceInstance = resources.GetInstanceId();
                captured->resourceEpoch = resources.GetResetVersion();
                captured->contentVersion = request.sourceVersion;
                captured->vertexCount = request.vertexCount;
                captured->largestBone = largestBone;
                captured->source = request.sourceVertices;
            }
            /// @note 完全一致 hit も native 版と owner を再検証し、UPLOAD は CPU 書込み先に限定する。
            /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map Simple Usage Models: avoid CPU reads from UPLOAD heaps.
            std::memcpy(mapped + sourceOffset, captured->bytes.data(), sourceBytes);
            if (callbackObserved || !owner->alive.load(std::memory_order_acquire)
                || source->GetContentVersion() != request.sourceVersion) { valid = false; break; }
            capturedSource.largestBone = captured->largestBone;
            capturedSource.copied = true;
        }
        if (capturedSource.largestBone >= request.boneCount) valid = false;
        if (!valid) break;
        auto& output = slot.outputs[i];
        auto* buffer = output.owner == owner ? static_cast<DX12Buffer*>(resources.Get(output.handle)) : nullptr;
        if (callbackObserved || !owner->alive.load(std::memory_order_acquire)) { valid = false; break; }
        const auto required = static_cast<uint32_t>(request.vertexCount * sizeof(Vertex));
        if (!buffer || !output.resource || buffer->GetResource() != output.resource.Get()
            || !output.version || output.bytes < required) {
            if (output.handle.IsValid() && buffer && buffer->GetResource() == output.resource.Get()) resources.Release(output.handle);
            if (callbackObserved || !owner->alive.load(std::memory_order_acquire)) { valid = false; break; }
            if (output.srv != INVALID_BINDLESS_INDEX) context.FreeBindlessSlot(output.srv);
            tracker.Remove(output.resource.Get());
            output = {};
            output.srv = INVALID_BINDLESS_INDEX;
            output.handle = resources.CreateGpuWritableVertexBuffer(required, sizeof(Vertex));
            if (callbackObserved || !owner->alive.load(std::memory_order_acquire)) { valid = false; break; }
            buffer = static_cast<DX12Buffer*>(resources.Get(output.handle));
            if (!buffer || !buffer->IsGpuWritable()) { valid = false; break; }
            output.resource = buffer->GetResource();
            output.owner = owner;
            output.version = buffer->RetainPreparedWriteVersion();
            output.bytes = required;
            output.srv = context.AllocateBindlessSlot();
            if (output.srv == INVALID_BINDLESS_INDEX) { valid = false; break; }
            D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
            raw.Format = DXGI_FORMAT_R32_TYPELESS;
            raw.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            raw.Buffer.NumElements = required / sizeof(uint32_t);
            raw.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            device->CreateShaderResourceView(output.resource.Get(), &raw, context.GetBindlessCpu(output.srv));
        }
        const uint64_t previous = output.version->value.load(std::memory_order_acquire);
        if (previous == (std::numeric_limits<uint64_t>::max)()) { valid = false; break; }
        /// @note 再利用した buffer は final-mainframe fence 完了済みで、ExecuteCommandLists 境界の buffer decay 後にある。
        /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12 State decay to COMMON.
        tracker.Register(output.resource.Get(), D3D12_RESOURCE_STATE_COMMON);
        D3D12_SHADER_RESOURCE_VIEW_DESC sourceDesc{};
        sourceDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sourceDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sourceDesc.Buffer.NumElements = request.vertexCount;
        sourceDesc.Buffer.StructureByteStride = sizeof(SkinnedVertex);
        /// @note 構造化 SRV の FirstElement は byte offset ではなく stride の倍数。
        sourceDesc.Buffer.FirstElement = sourceOffset / sizeof(SkinnedVertex);
        device->CreateShaderResourceView(slot.upload.Get(), &sourceDesc, cpu(i * 3));
        D3D12_SHADER_RESOURCE_VIEW_DESC bones{};
        bones.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        bones.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        bones.Buffer.NumElements = static_cast<UINT>(matrices.size());
        bones.Buffer.StructureByteStride = sizeof(math::Matrix4);
        device->CreateShaderResourceView(slot.upload.Get(), &bones, cpu(i * 3 + 1));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = request.vertexCount;
        uav.Buffer.StructureByteStride = sizeof(Vertex);
        device->CreateUnorderedAccessView(output.resource.Get(), nullptr, &uav, cpu(i * 3 + 2));
        const auto constantOffset = constantsStart + i * 768u;
        std::memset(mapped + constantOffset, 0, 768u);
        const uint32_t constants[4] = {request.vertexCount, request.boneOffset, 0, 0};
        std::memcpy(mapped + constantOffset, constants, sizeof(constants));
        BindlessIndicesConstants indices;
        indices.Reset();
        indices.pixel[14] = static_cast<uint32_t>(i * 3);
        indices.pixel[15] = static_cast<uint32_t>(i * 3 + 1);
        indices.uav[4] = static_cast<uint32_t>(i * 3 + 2);
        std::memcpy(mapped + constantOffset + 256u, &indices, sizeof(indices));
        const auto address = slot.upload->GetGPUVirtualAddress() + constantOffset;
        for (uint32_t cb = 0; cb < 14; ++cb) slot.commands->SetComputeRootConstantBufferView(cb, address + 512u);
        slot.commands->SetComputeRootConstantBufferView(0, address);
        slot.commands->SetComputeRootConstantBufferView(kBindlessIndicesRootParam, address + 256u);
        auto barrier = Transition(output.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot.commands->ResourceBarrier(1, &barrier);
        slot.commands->Dispatch((request.vertexCount - 1u) / 64u + 1u, 1, 1);
        barrier = Transition(output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        slot.commands->ResourceBarrier(1, &barrier);
        slot.writes.push_back({output.handle, previous, previous, false, request.vertexCount});
    }
    slot.upload->Unmap(0, nullptr);
    const HRESULT closeResult = slot.commands->Close();
    recording.closed = true;
    if (callbackObserved || !valid || FAILED(closeResult) || !owner->alive.load(std::memory_order_acquire)) return {};
    work->writes = slot.writes;
    work->instance = resources.GetInstanceId();
    work->epoch = resources.GetResetVersion();
    work->frame = resources.FrameStamp();
    return work;
}
} /// @note namespace fbzz::renderer
