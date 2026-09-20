/// @file    DX12Buffer.hpp
/// @brief   CPU 更新可能な DirectX 12 頂点・インデックスバッファ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Graphics/Renderer/IBuffer.hpp>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace fbzz::renderer {

class DX12Context;
class DX12UploadArena;

class DX12StateTracker;

class DX12Buffer final : public IBuffer {
public:
    enum class Kind { Vertex, Index };
    ~DX12Buffer() override;
    bool Init(DX12Context* context, const void* data, size_t sizeBytes, uint32_t stride, Kind kind);

    /// @note CS が書き込み、IA が頂点として読むバッファを確保する (コンピュートスキニングの出力先)。
    /// @note DEFAULT ヒープ+ALLOW_UNORDERED_ACCESS で作り、UAV デスクリプタは CPU ステージング用
    /// @note       ヒープに保持する (Dispatch 時に shader-visible テーブルへコピーする既存方式に合わせる)。
    /// @note DX12 は状態遷移が明示的なため、書くとき UNORDERED_ACCESS、読むとき
    /// @note       VERTEX_AND_CONSTANT_BUFFER へ遷移させる必要がある。遷移は DX12Renderer の
    /// @note       Dispatch/Submit が StateTracker 経由で行う。
    bool InitGpuWritableVertex(DX12Context* context, DX12StateTracker* tracker,
                               size_t sizeBytes, uint32_t stride);

    void Update(const void* data, size_t sizeBytes) override;
    size_t GetSize() const override { return m_size; }
    uint32_t GetStride() const override { return m_stride; }
    D3D12_VERTEX_BUFFER_VIEW GetVertexView(DX12UploadArena& arena);
    D3D12_INDEX_BUFFER_VIEW GetIndexView(DX12UploadArena& arena);

    /// @note InitGpuWritableVertex で作った場合のみ有効。それ以外は nullptr / 空ハンドル。
    ID3D12Resource*             GetResource() const { return m_resource.Get(); }
    bool                        IsGpuWritable() const { return m_descriptorHeap != nullptr; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetUav() const;

    /// @brief UAV 側の永続 bindless 添字。GPU 書き込み不可のバッファは INVALID を返す。
    /// @see  Docs/design/bindless.md
    uint32_t GetBindlessUavIndex() const override;

private:
    D3D12_GPU_VIRTUAL_ADDRESS PrepareForSubmit(DX12UploadArena& arena);
    std::vector<uint8_t> m_cpuData;
    size_t m_dataSize = 0;
    uint64_t m_cachedEpoch = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_cachedAddress = 0;
    bool m_dirty = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    /// @note GPU 書き込み可能バッファの UAV を置く CPU 専用ヒープ。
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    /// @note 永続 bindless 枠 (UAV)。初回参照で確保するため mutable。
    mutable uint32_t m_bindlessUavIndex = INVALID_BINDLESS_INDEX;
    DX12StateTracker* m_tracker = nullptr;
    uint8_t* m_mapped = nullptr;
    size_t m_size = 0;
    uint32_t m_stride = 0;
    Kind m_kind = Kind::Vertex;
    DX12Context* m_context = nullptr;
};

} /// @note namespace fbzz::renderer
