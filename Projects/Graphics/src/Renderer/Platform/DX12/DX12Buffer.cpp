/// @file    DX12Buffer.cpp
/// @brief   頂点・インデックスの更新内容を GPU 使用中のデータから分離する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12Buffer.hpp"
#include "DX12Context.hpp"
#include "DX12StateTracker.hpp"
#include "DX12UploadArena.hpp"

#include <Core/Logger.hpp>
#include <Core/HResult.hpp>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace fbzz::renderer {

DX12Buffer::~DX12Buffer()
{
    if (m_resource && m_mapped)
        m_resource->Unmap(0, nullptr);
    m_mapped = nullptr;
    /// @note 状態追跡から外してから解放する。残すと同アドレスへ載った別リソースの状態を誤認する。
    if (m_tracker) m_tracker->Remove(m_resource.Get());
    if (m_context) {
        /// @note リソース本体と同じフェンスで守る (DX12Texture のデストラクタと同じ理由)。
        m_context->FreeBindlessSlot(m_bindlessUavIndex);
        m_context->FreeBindlessSlot(m_bindlessSrvIndex);
        m_context->DeferRelease(m_srvResource);
        m_context->DeferRelease(m_resource);
    }
}

bool DX12Buffer::Init(DX12Context* context, const void* data, size_t sizeBytes, uint32_t stride, Kind kind)
{
    if (!context || sizeBytes == 0)
        return false;
    m_context = context;
    ID3D12Device* device = context->GetDevice();
    m_size = sizeBytes;
    m_stride = stride;
    m_kind = kind;
    m_dataSize = data ? sizeBytes : 0;
    m_contentVersion = 1;
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
    void* mapped = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource)))
        || FAILED(m_resource->Map(0, nullptr, &mapped))) {
        FBZZ_LOG_ERROR("DX12Buffer: %zu byte のバッファ生成に失敗しました", sizeBytes);
        return false;
    }
    m_mapped = static_cast<uint8_t*>(mapped);
    if (data)
        std::memcpy(m_mapped, data, sizeBytes);
    return true;
}

bool DX12Buffer::InitGpuWritableVertex(DX12Context* context, DX12StateTracker* tracker,
                                       size_t sizeBytes, uint32_t stride)
{
    if (!context || !tracker || sizeBytes == 0 || stride == 0)
        return false;

    m_context = context;
    m_tracker = tracker;
    m_size    = sizeBytes;
    m_stride  = stride;
    m_kind    = Kind::Vertex;

    /// @note CreateDefaultBuffer は ALLOW_UNORDERED_ACCESS 付きで DEFAULT ヒープへ作る。
    /// @note       data=nullptr なので初期状態は COMMON。CPU からは触らない (m_mapped は null のまま)。
    if (!context->CreateDefaultBuffer(nullptr, sizeBytes, m_resource))
        return false;
    tracker->Register(m_resource.Get(), D3D12_RESOURCE_STATE_COMMON);

    /// @note UAV は CPU 専用ヒープへ置き、Dispatch 時に shader-visible テーブルへコピーする
    /// @note       (DX12StructuredBuffer と同じ方式)。
    ID3D12Device* device = context->GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap))))
        return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    /// @note Structured は UNKNOWN 固定
    uav.Format                      = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements          = static_cast<UINT>(sizeBytes / stride);
    uav.Buffer.StructureByteStride  = stride;
    device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uav, GetUav());
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12Buffer::GetUav() const
{
    if (!m_descriptorHeap) return {};
    return m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
}

uint32_t DX12Buffer::GetBindlessUavIndex() const
{
    if (m_bindlessUavIndex != INVALID_BINDLESS_INDEX)
        return m_bindlessUavIndex;
    /// @note m_descriptorHeap を持つのは GPU 書き込み可能な頂点バッファだけ (IsGpuWritable と同じ条件)。
    if (!m_context || !m_descriptorHeap || !m_context->SupportsBindless())
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

uint32_t DX12Buffer::GetBindlessSrvIndex() const
{
    /// @note GPU 出力の descriptor は同じ実体を指す。再 Dispatch の内容版だけで枠を再確保しない。
    if (IsGpuWritable() && m_bindlessSrvIndex != INVALID_BINDLESS_INDEX)
        return m_bindlessSrvIndex;
    if (m_bindlessSrvIndex != INVALID_BINDLESS_INDEX && m_srvContentVersion == m_contentVersion)
        return m_bindlessSrvIndex;
    const size_t readableBytes = GetDataSize();
    if (!m_context || !m_resource || !m_context->SupportsBindless() || readableBytes == 0
        || readableBytes % sizeof(uint32_t) != 0
        || readableBytes / sizeof(uint32_t) > (std::numeric_limits<UINT>::max)())
        return INVALID_BINDLESS_INDEX;

    Microsoft::WRL::ComPtr<ID3D12Resource> readable;
    if (!CreateRawReadSnapshot(readableBytes, readable)) return INVALID_BINDLESS_INDEX;
    const uint32_t slot = m_context->AllocateBindlessSlot();
    if (slot == INVALID_BINDLESS_INDEX) return INVALID_BINDLESS_INDEX;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = static_cast<UINT>(readableBytes / sizeof(uint32_t));
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_buffer_srv Raw buffer SRV layout
    m_context->GetDevice()->CreateShaderResourceView(readable.Get(), &srv, m_context->GetBindlessCpu(slot));
    m_context->FreeBindlessSlot(m_bindlessSrvIndex);
    m_context->DeferRelease(m_srvResource);
    m_srvResource = std::move(readable);
    m_bindlessSrvIndex = slot;
    m_srvContentVersion = m_contentVersion;
    return slot;
}

bool DX12Buffer::CreateRawReadSnapshot(size_t readableBytes,
                                      Microsoft::WRL::ComPtr<ID3D12Resource>& readable) const
{
    if (m_cpuData.empty()) {
        readable = m_resource;
        return true;
    }
    /// @note Update は GPU が使う元実体へ書かない。内容版ごとに新しい immutable snapshot へ公開する。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data#mapping-and-unmapping D3D12 resource renaming responsibility
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC desc = m_resource->GetDesc();
    Microsoft::WRL::ComPtr<ID3D12Resource> snapshot;
    const HRESULT createResult = m_context->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&snapshot));
    FBZZ_HR_CHECK(createResult);
    void* mapped = nullptr;
    const HRESULT mapResult = snapshot->Map(0, nullptr, &mapped);
    FBZZ_HR_CHECK(mapResult);
    std::memcpy(mapped, m_cpuData.data(), readableBytes);
    snapshot->Unmap(0, nullptr);
    readable = std::move(snapshot);
    return true;
}

bool DX12Buffer::CopyData(size_t offset, size_t sizeBytes, void* output) const
{
    if (!m_mapped || !m_contentVersion || !output || !sizeBytes
        || offset > m_dataSize || sizeBytes > m_dataSize - offset) return false;
    const auto* source = m_cpuData.empty() ? m_mapped : m_cpuData.data();
    std::memcpy(output, source + offset, sizeBytes);
    return true;
}

void DX12Buffer::NotifyGpuWrite()
{
    if (!IsGpuWritable()) return;
    assert(m_contentVersion != (std::numeric_limits<uint64_t>::max)());
    ++m_contentVersion;
}

void DX12Buffer::Update(const void* data, size_t sizeBytes)
{
    /// @note GPU 書き込み専用バッファは CPU から更新しない (m_mapped が null)。
    if (!m_mapped) {
        FBZZ_LOG_ERROR("DX12Buffer: GPU 書き込み専用バッファは CPU から更新できません");
        return;
    }
    if (!data || sizeBytes == 0 || sizeBytes > m_size) {
        FBZZ_LOG_ERROR("DX12Buffer: 無効な更新サイズです (%zu / %zu)", sizeBytes, m_size);
        return;
    }
    /// @note 同じバッファを UI が次フレームで借りても、GPU が読む旧頂点を上書きしない。
    if (m_cpuData.empty()) {
        m_cpuData.resize(m_size, 0);
        if (m_dataSize > 0)
            std::memcpy(m_cpuData.data(), m_mapped, m_dataSize);
    }
    std::memcpy(m_cpuData.data(), data, sizeBytes);
    m_dataSize = (std::max)(m_dataSize, sizeBytes);
    m_dirty = true;
    ++m_contentVersion;
}

D3D12_GPU_VIRTUAL_ADDRESS DX12Buffer::PrepareForSubmit(DX12UploadArena& arena)
{
    if (m_cpuData.empty()) return m_resource->GetGPUVirtualAddress();
    if (!m_dirty && m_cachedAddress != 0 && m_cachedEpoch == arena.GetEpoch())
        return m_cachedAddress;
    const auto allocation = arena.Allocate(m_dataSize, 16);
    if (!allocation) return 0;
    std::memcpy(allocation.cpu, m_cpuData.data(), m_dataSize);
    m_cachedAddress = allocation.gpu;
    m_cachedEpoch = arena.GetEpoch();
    m_dirty = false;
    return m_cachedAddress;
}

D3D12_VERTEX_BUFFER_VIEW DX12Buffer::GetVertexView(DX12UploadArena& arena)
{
    return {PrepareForSubmit(arena),
            static_cast<UINT>(m_cpuData.empty() ? m_size : m_dataSize), m_stride};
}

D3D12_INDEX_BUFFER_VIEW DX12Buffer::GetIndexView(DX12UploadArena& arena)
{
    return {PrepareForSubmit(arena),
            static_cast<UINT>(m_cpuData.empty() ? m_size : m_dataSize), DXGI_FORMAT_R32_UINT};
}

bool DX12Buffer::ValidateIndexRange(uint32_t firstIndex, uint32_t indexCount, uint32_t vertexCount) const
{
    if (m_kind != Kind::Index || (static_cast<uint64_t>(firstIndex) + indexCount) * sizeof(uint32_t) > GetDataSize())
        return false;
    const uint8_t* data = m_cpuData.empty() ? m_mapped : m_cpuData.data();
    if (!data) return false;
    for (uint32_t index = 0; index < indexCount; ++index) {
        uint32_t vertexIndex = 0;
        std::memcpy(&vertexIndex, data + (static_cast<size_t>(firstIndex) + index) * sizeof(uint32_t), sizeof(vertexIndex));
        if (vertexIndex >= vertexCount) return false;
    }
    return true;
}

} /// @note namespace fbzz::renderer
