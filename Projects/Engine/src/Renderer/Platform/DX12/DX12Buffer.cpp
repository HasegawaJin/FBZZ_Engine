// FBZZ Engine
// DX12Buffer.cpp | fbzz::renderer
// 永続 Map した Upload Heap による動的頂点・インデックス更新
#include "DX12Buffer.hpp"
#include "DX12Context.hpp"

#include <Engine/Core/Logger.hpp>
#include <cstring>

namespace fbzz::renderer {

DX12Buffer::~DX12Buffer()
{
    if (m_resource && m_mapped)
        m_resource->Unmap(0, nullptr);
    m_mapped = nullptr;
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

void DX12Buffer::Update(const void* data, size_t sizeBytes)
{
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
