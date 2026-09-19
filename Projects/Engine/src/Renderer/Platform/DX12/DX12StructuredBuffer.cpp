/// @file    DX12StructuredBuffer.cpp
/// @brief   CPU更新用Upload Buffer、読み取り専用Default Buffer、GPU書き込み用Default Bufferの生成。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12StructuredBuffer.hpp"

#include "DX12Context.hpp"
#include "DX12StateTracker.hpp"
#include <Engine/Core/Logger.hpp>
#include <cstring>
#include <algorithm>

namespace fbzz::renderer {

DX12StructuredBuffer::~DX12StructuredBuffer()
{
    if (m_resource && m_mapped) m_resource->Unmap(0, nullptr);
    m_mapped = nullptr;
    if (m_tracker) m_tracker->Remove(m_resource.Get());
    if (m_context) {
        /// @note リソース本体と同じフェンスで守る (DX12Texture のデストラクタと同じ理由)。
        m_context->FreeBindlessSlot(m_bindlessIndex);
        m_context->FreeBindlessSlot(m_bindlessUavIndex);
        m_context->DeferRelease(m_resource);
    }
}

uint32_t DX12StructuredBuffer::GetBindlessIndex() const
{
    if (m_bindlessIndex != INVALID_BINDLESS_INDEX)
        return m_bindlessIndex;
    if (!m_context || !m_descriptorHeap || !m_context->SupportsBindless())
        return INVALID_BINDLESS_INDEX;

    const uint32_t slot = m_context->AllocateBindlessSlot();
    if (slot == DX12Context::INVALID_BINDLESS_INDEX)
        return INVALID_BINDLESS_INDEX;

    m_context->GetDevice()->CopyDescriptorsSimple(
        1, m_context->GetBindlessCpu(slot), GetSrv(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_bindlessIndex = slot;
    return slot;
}

uint32_t DX12StructuredBuffer::GetBindlessUavIndex() const
{
    if (m_bindlessUavIndex != INVALID_BINDLESS_INDEX)
        return m_bindlessUavIndex;
    /// @note 読み取り専用で作られたバッファは UAV ディスクリプタを持たない。ここで弾かないと
    ///       GetUav() が SRV 枠を指し、「書けるつもりの SRV」を配ってしまう。
    if (!m_context || !m_readWrite || !m_descriptorHeap || !m_context->SupportsBindless())
        return INVALID_BINDLESS_INDEX;

    const uint32_t slot = m_context->AllocateBindlessSlot();
    if (slot == DX12Context::INVALID_BINDLESS_INDEX)
        return INVALID_BINDLESS_INDEX;

    m_context->GetDevice()->CopyDescriptorsSimple(
        1, m_context->GetBindlessCpu(slot), GetUav(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_bindlessUavIndex = slot;
    return slot;
}

bool DX12StructuredBuffer::Init(DX12Context* context, DX12StateTracker* tracker, const void* data,
                                uint32_t elementCount, uint32_t stride, bool readWrite,
                                bool gpuLocalReadOnly)
{
    if (!context || !tracker || elementCount == 0 || stride == 0 ||
        (readWrite && gpuLocalReadOnly)) return false;
    m_tracker = tracker;
    m_context = context;
    m_elementCount = elementCount;
    m_stride = stride;
    m_readWrite = readWrite;
    m_gpuLocalReadOnly = gpuLocalReadOnly;
    const size_t sizeBytes = GetSize();
    ID3D12Device* device = context->GetDevice();
    if (readWrite || gpuLocalReadOnly) {
        /// @note 初期データは一度だけ Upload Heap を経由し、以後の SRV 読み取りは DEFAULT Heap から行う
        ///       (immutable なスキニング入力を CPU 可視メモリへ置き続ける必要はない)。
        if (!context->CreateDefaultBuffer(data, sizeBytes, m_resource)) return false;
        tracker->Register(m_resource.Get(), D3D12_RESOURCE_STATE_COMMON);
    } else {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource)))) return false;
        void* mapped = nullptr;
        if (FAILED(m_resource->Map(0, nullptr, &mapped))) return false;
        m_mapped = static_cast<uint8_t*>(mapped);
        if (data) std::memcpy(m_mapped, data, sizeBytes);
        tracker->Register(m_resource.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = readWrite ? 2 : 1;
    /// @note Resource SRV/UAVは動的shader-visibleテーブルへコピーするCPU stagingとして保持する。
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap)))) return false;
    m_descriptorIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = elementCount;
    srv.Buffer.StructureByteStride = stride;
    device->CreateShaderResourceView(m_resource.Get(), &srv, GetSrv());
    if (readWrite) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = elementCount;
        uav.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uav, GetUav());
    }
    return true;
}

void DX12StructuredBuffer::Update(const void* data, size_t sizeBytes)
{
    if (!data || sizeBytes == 0) return;
    if (m_gpuLocalReadOnly) {
        /// @note immutable SRV の更新要求は契約違反。再生成による明示的な差し替えを要求する。
        FBZZ_LOG_WARN("DX12StructuredBuffer: GPUローカル読み取り専用バッファは更新できません");
        return;
    }
    if (m_readWrite) {
        const size_t copySize = (std::min)(sizeBytes, GetSize());
        if (!m_context->IsFrameOpen()) {
            FBZZ_LOG_WARN("DX12StructuredBuffer: RW buffer 更新は BeginFrame 内で行う必要があります");
            return;
        }
        m_tracker->Transition(m_context->GetCommandList(), m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        if (m_context->StageBufferCopy(m_resource.Get(), data, copySize))
            m_tracker->Transition(m_context->GetCommandList(), m_resource.Get(), D3D12_RESOURCE_STATE_COMMON);
        return;
    }
    std::memcpy(m_mapped, data, (std::min)(sizeBytes, GetSize()));
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12StructuredBuffer::GetSrv() const
{
    return m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12StructuredBuffer::GetUav() const
{
    auto handle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += m_descriptorIncrement;
    return handle;
}

} // namespace fbzz::renderer
