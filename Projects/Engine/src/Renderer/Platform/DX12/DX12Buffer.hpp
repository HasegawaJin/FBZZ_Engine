// FBZZ Engine
// DX12Buffer.hpp | fbzz::renderer
// CPU 更新可能な DirectX 12 頂点・インデックスバッファ
#pragma once

#include <Engine/Renderer/IBuffer.hpp>
#include <d3d12.h>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12Context;

class DX12Buffer final : public IBuffer {
public:
    enum class Kind { Vertex, Index };
    ~DX12Buffer() override;
    bool Init(DX12Context* context, const void* data, size_t sizeBytes, uint32_t stride, Kind kind);
    void Update(const void* data, size_t sizeBytes) override;
    size_t GetSize() const override { return m_size; }
    uint32_t GetStride() const override { return m_stride; }
    D3D12_VERTEX_BUFFER_VIEW GetVertexView() const;
    D3D12_INDEX_BUFFER_VIEW GetIndexView() const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    uint8_t* m_mapped = nullptr;
    size_t m_size = 0;
    uint32_t m_stride = 0;
    Kind m_kind = Kind::Vertex;
    DX12Context* m_context = nullptr;
};

} // namespace fbzz::renderer
