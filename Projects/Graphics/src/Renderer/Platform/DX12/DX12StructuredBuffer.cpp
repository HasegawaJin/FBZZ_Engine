/// @file    DX12StructuredBuffer.cpp
/// @brief   CPU更新用Upload Buffer、読み取り専用Default Buffer、GPU書き込み用Default Bufferの生成。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12StructuredBuffer.hpp"

#include "DX12Context.hpp"
#include "DX12StateTracker.hpp"
#include <Core/Logger.hpp>
#include <cstring>
#include <algorithm>

namespace fbzz::renderer {

DX12StructuredBuffer::~DX12StructuredBuffer()
{
    if (m_resource && m_mapped) m_resource->Unmap(0, nullptr);
    m_mapped = nullptr;
    if (m_tracker) m_tracker->Remove(m_resource.Get());
    if (m_context) {
        m_context->FreeCpuResourceDescriptor(m_srvCpuIndex);
        m_context->FreeCpuResourceDescriptor(m_uavCpuIndex);
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
    if (!m_context || m_srvCpuIndex == INVALID_BINDLESS_INDEX || !m_context->SupportsBindless())
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

bool DX12StructuredBuffer::UpdateRegion(const void* data, size_t bytes, size_t offset)
{
    if (!data || bytes==0 || offset>GetSize() || bytes>GetSize()-offset || m_gpuLocalReadOnly) return false;
    if (!m_readWrite) { std::memcpy(m_mapped+offset,data,bytes); return true; }
    if (!m_context || !m_context->IsFrameOpen()) return false;
    m_tracker->Transition(m_context->GetCommandList(),m_resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
    const bool copied=m_context->StageBufferCopy(m_resource.Get(),data,bytes,offset);
    m_tracker->Transition(m_context->GetCommandList(),m_resource.Get(),D3D12_RESOURCE_STATE_COMMON);
    return copied;
}

bool DX12StructuredBuffer::UpdateRows(const void* data, size_t rowBytes, size_t rowCount,
    size_t destinationPitch, size_t offsetBytes)
{
    if (!m_readWrite || rowBytes == destinationPitch)
        return IStructuredBuffer::UpdateRows(data, rowBytes, rowCount, destinationPitch, offsetBytes);
    const auto size = GetSize();
    if (!data || rowBytes == 0 || rowCount == 0 || destinationPitch < rowBytes
        || offsetBytes > size || rowBytes > size - offsetBytes
        || rowCount - 1 > (size - offsetBytes - rowBytes) / destinationPitch
        || !m_context || !m_context->IsFrameOpen() || m_context->IsComputeListOpen()) return false;

    /// @note Keep the whole rectangle in COPY_DEST, then expose it to later draws on the direct list.
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states Using resource barriers to synchronize resource states.
    auto* commands = m_context->GetCommandList();
    m_tracker->Transition(commands, m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    const auto* source = static_cast<const uint8_t*>(data);
    bool copied = true;
    for (size_t row = 0; row < rowCount; ++row) {
        if (m_context->StageBufferCopy(m_resource.Get(), source + row * rowBytes,
                rowBytes, offsetBytes + row * destinationPitch)) continue;
        copied = false;
        break;
    }
    m_tracker->Transition(commands, m_resource.Get(), D3D12_RESOURCE_STATE_COMMON);
    return copied;
}

uint32_t DX12StructuredBuffer::GetBindlessUavIndex() const
{
    if (m_bindlessUavIndex != INVALID_BINDLESS_INDEX)
        return m_bindlessUavIndex;
    /// @note 読み取り専用で作られたバッファは UAV ディスクリプタを持たない。ここで弾かないと
    /// @note       GetUav() が SRV 枠を指し、「書けるつもりの SRV」を配ってしまう。
    if (!m_context || !m_readWrite || m_uavCpuIndex == INVALID_BINDLESS_INDEX || !m_context->SupportsBindless())
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
        /// @note       (immutable なスキニング入力を CPU 可視メモリへ置き続ける必要はない)。
        const bool stageUpload = gpuLocalReadOnly && data && context->IsFrameOpen() && !context->IsComputeListOpen();
        if (!context->CreateDefaultBuffer(stageUpload ? nullptr : data, sizeBytes, m_resource)) return false;
        tracker->Register(m_resource.Get(), D3D12_RESOURCE_STATE_COMMON);
        if (stageUpload) {
            /// @note 同じ描画リストの後続 SRV より先に転送し、草のパッチ追加ごとの同期投入を省く。
            auto* commands = context->GetCommandList();
            tracker->Transition(commands, m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            const bool copied = context->StageBufferCopy(m_resource.Get(), data, sizeBytes);
            tracker->Transition(commands, m_resource.Get(), D3D12_RESOURCE_STATE_COMMON);
            if (!copied) return false;
        }
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
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource)))) {
            FBZZ_LOG_ERROR("DX12StructuredBuffer: Upload resource allocation failed (bytes=%zu count=%u stride=%u bindless=%u/%u)",
                sizeBytes, elementCount, stride, context->GetBindlessUsage(), context->GetBindlessCapacity());
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(m_resource->Map(0, nullptr, &mapped))) {
            FBZZ_LOG_ERROR("DX12StructuredBuffer: Upload mapping failed (bytes=%zu count=%u stride=%u)",
                sizeBytes, elementCount, stride);
            return false;
        }
        m_mapped = static_cast<uint8_t*>(mapped);
        if (data) std::memcpy(m_mapped, data, sizeBytes);
        tracker->Register(m_resource.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    }

    m_srvCpuIndex = context->AllocateCpuResourceDescriptor();
    if (m_srvCpuIndex == INVALID_BINDLESS_INDEX) return false;
    if (readWrite) {
        m_uavCpuIndex = context->AllocateCpuResourceDescriptor();
        if (m_uavCpuIndex == INVALID_BINDLESS_INDEX) return false;
    }
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
    return m_context ? m_context->GetCpuResourceDescriptor(m_srvCpuIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12StructuredBuffer::GetUav() const
{
    return m_context ? m_context->GetCpuResourceDescriptor(m_uavCpuIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

} /// @note namespace fbzz::renderer
