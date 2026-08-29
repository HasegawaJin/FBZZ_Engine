/// @file    DX12Buffer.cpp
/// @brief   永続 Map した Upload Heap による動的頂点・インデックス更新。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12Buffer.hpp"
#include "DX12Context.hpp"
#include "DX12StateTracker.hpp"

#include <Engine/Core/Logger.hpp>
#include <cstring>

namespace fbzz::renderer {

DX12Buffer::~DX12Buffer()
{
    if (m_resource && m_mapped)
        m_resource->Unmap(0, nullptr);
    m_mapped = nullptr;
    // 状態追跡から外してから解放する。残すと同アドレスへ載った別リソースの状態を誤認する。
    if (m_tracker) m_tracker->Remove(m_resource.Get());
    if (m_context) m_context->DeferRelease(m_resource);
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

    // CreateDefaultBuffer は ALLOW_UNORDERED_ACCESS 付きで DEFAULT ヒープへ作る。
    // data=nullptr なので初期状態は COMMON。CPU からは触らない (m_mapped は null のまま)。
    if (!context->CreateDefaultBuffer(nullptr, sizeBytes, m_resource))
        return false;
    tracker->Register(m_resource.Get(), D3D12_RESOURCE_STATE_COMMON);

    // UAV は CPU 専用ヒープへ置き、Dispatch 時に shader-visible テーブルへコピーする
    // (DX12StructuredBuffer と同じ方式)。
    ID3D12Device* device = context->GetDevice();
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap))))
        return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format                      = DXGI_FORMAT_UNKNOWN; // Structured は UNKNOWN 固定
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

void DX12Buffer::Update(const void* data, size_t sizeBytes)
{
    // GPU 書き込み専用バッファは CPU から更新しない (m_mapped が null)。
    if (!m_mapped) {
        FBZZ_LOG_ERROR("DX12Buffer: GPU 書き込み専用バッファは CPU から更新できません");
        return;
    }
    if (!data || sizeBytes > m_size) {
        FBZZ_LOG_ERROR("DX12Buffer: 無効な更新サイズです (%zu / %zu)", sizeBytes, m_size);
        return;
    }
    std::memcpy(m_mapped, data, sizeBytes);
}

D3D12_VERTEX_BUFFER_VIEW DX12Buffer::GetVertexView() const
{
    return {m_resource->GetGPUVirtualAddress(), static_cast<UINT>(m_size), m_stride};
}

D3D12_INDEX_BUFFER_VIEW DX12Buffer::GetIndexView() const
{
    return {m_resource->GetGPUVirtualAddress(), static_cast<UINT>(m_size), DXGI_FORMAT_R32_UINT};
}

} // namespace fbzz::renderer
