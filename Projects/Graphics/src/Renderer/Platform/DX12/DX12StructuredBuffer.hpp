/// @file    DX12StructuredBuffer.hpp
/// @brief   StructuredBuffer / RWStructuredBuffer のDirectX 12実装。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <d3d12.h>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12Context;
class DX12StateTracker;

class DX12StructuredBuffer final : public IStructuredBuffer {
public:
    FBZZ_GRAPHICS_API ~DX12StructuredBuffer() override;
    FBZZ_GRAPHICS_API bool Init(DX12Context* context, DX12StateTracker* tracker, const void* data,
              uint32_t elementCount, uint32_t stride, bool readWrite,
              bool gpuLocalReadOnly = false);
    FBZZ_GRAPHICS_API void Update(const void* data, size_t sizeBytes) override;
    FBZZ_GRAPHICS_API bool UpdateRegion(const void* data, size_t sizeBytes, size_t offsetBytes) override;
    FBZZ_GRAPHICS_API bool UpdateRows(const void* data, size_t rowBytes, size_t rowCount,
        size_t destinationPitch, size_t offsetBytes) override;
    uint32_t GetElementCount() const override { return m_elementCount; }
    uint32_t GetStride() const override { return m_stride; }
    size_t GetSize() const override { return static_cast<size_t>(m_elementCount) * m_stride; }
    bool IsReadWrite() const { return m_readWrite; }
    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    FBZZ_GRAPHICS_API D3D12_CPU_DESCRIPTOR_HANDLE GetSrv() const;
    FBZZ_GRAPHICS_API D3D12_CPU_DESCRIPTOR_HANDLE GetUav() const;

    /// @brief 永続 bindless ディスクリプタの添字 (SRV)。初回呼び出しで確保して発行する。
    /// @note 遅延発行の理由と GPU 実行中に書ける理由は DX12Texture::GetBindlessIndex と同じ。
    /// @see  Docs/design/bindless.md
    FBZZ_GRAPHICS_API uint32_t GetBindlessIndex() const override;

    /// @brief UAV 側の永続 bindless 添字。読み取り専用で作られたバッファは INVALID を返す。
    FBZZ_GRAPHICS_API uint32_t GetBindlessUavIndex() const override;

private:
    DX12StateTracker* m_tracker = nullptr;
    DX12Context* m_context = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    uint32_t m_srvCpuIndex = INVALID_BINDLESS_INDEX;
    uint32_t m_uavCpuIndex = INVALID_BINDLESS_INDEX;
    uint8_t* m_mapped = nullptr;
    uint32_t m_elementCount = 0;
    uint32_t m_stride = 0;
    bool m_readWrite = false;
    /// @note DEFAULT Heap 上の immutable SRV。生成時の upload 後は CPU Update を受け付けない。
    bool m_gpuLocalReadOnly = false;
    /// @note 永続 bindless 枠。GetBindlessIndex() の初回呼び出しで確保するため mutable。
    mutable uint32_t m_bindlessIndex = INVALID_BINDLESS_INDEX;
    mutable uint32_t m_bindlessUavIndex = INVALID_BINDLESS_INDEX;
};

} /// @note namespace fbzz::renderer
