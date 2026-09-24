/// @file    DX12RenderTarget.hpp
/// @brief   MRTカラー、深度、RTV/DSV/SRVを所有するDirectX 12描画先。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Graphics/Renderer/Format.hpp>
#include <Graphics/Renderer/IRenderTarget.hpp>
#include <array>
#include <d3d12.h>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12Context;
class DX12StateTracker;

class DX12RenderTarget final : public IRenderTarget {
public:
    static constexpr uint32_t MAX_COLOR = 8;
    ~DX12RenderTarget() override;
    bool Init(DX12Context* context, DX12StateTracker* tracker,
              uint32_t width, uint32_t height, const RenderTargetDesc& desc);
    bool InitCubemap(DX12Context* context, DX12StateTracker* tracker,
                     uint32_t size, uint32_t mipCount);
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetColorCount() const override { return m_colorCount; }
    void* GetNativeSRV(int slot = 0) const override;
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtv(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetColorSrv(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetColorSrvGpu(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthSrv() const;
    ID3D12Resource* GetColorResource(uint32_t index) const;
    ID3D12Resource* GetDepthResource() const { return m_depth.Get(); }
    bool            HasDepth() const { return m_depth != nullptr; }
    /// @brief 深度が Reversed-Z か。深度を持たない RT では false。
    bool            IsReversedZ() const { return m_reversedZ; }
    DXGI_FORMAT GetColorFormat() const { return m_colorFormat; }
    void TransitionColorForRead(ID3D12GraphicsCommandList* commands, uint32_t index);
    bool IsCubemap() const { return m_isCubemap; }
    uint32_t GetMipCount() const { return m_mipCount; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetFaceRtv(uint32_t face, uint32_t mip) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCubeSrv() const { return m_srvHeap->GetCPUDescriptorHandleForHeapStart(); }
    ID3D12Resource* GetCubeResource() const { return m_cube.Get(); }

private:
    DX12StateTracker* m_tracker = nullptr;
    DX12Context* m_context = nullptr;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, MAX_COLOR> m_colors;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cube;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    uint32_t m_rtvIncrement = 0;
    uint32_t m_srvIncrement = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_colorCount = 0;
    DXGI_FORMAT m_colorFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t m_mipCount = 1;
    bool m_isCubemap = false;
    bool m_reversedZ = false;
};

} /// @note namespace fbzz::renderer
