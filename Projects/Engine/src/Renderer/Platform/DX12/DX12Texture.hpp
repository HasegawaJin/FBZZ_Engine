// FBZZ Engine
// DX12Texture.hpp | fbzz::renderer
// Default Heap テクスチャと永続CPU SRVを保持する DirectX 12 実装
#pragma once

#include <Engine/Renderer/ITexture.hpp>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace fbzz::renderer {

class DX12Context;
class DX12StateTracker;

class DX12Texture final : public ITexture {
public:
    ~DX12Texture() override;
    bool Init(DX12Context* context, const std::string& path);
    bool InitFromData(DX12Context* context, const uint8_t* rgba, uint32_t width, uint32_t height);
    // 3D テクスチャ (R8G8B8A8_UNORM)。ボリューメトリック雲ノイズ / 3D カラー LUT 用。
    // WHY: DX11 の Init3DFromData 相当。未実装だと 3D テクスチャが null になり、シェーダーの
    //      Texture3D スロットへ null Texture2D SRV がバインドされて次元不一致の検証エラーになる。
    bool InitFromData3D(DX12Context* context, const uint8_t* rgba,
                        uint32_t width, uint32_t height, uint32_t depth);
    bool InitFromResource(DX12Context* context, ID3D12Resource* resource, DXGI_FORMAT srvFormat,
                          uint32_t width, uint32_t height);
    bool InitCubeFromResource(DX12Context* context, ID3D12Resource* resource, DXGI_FORMAT srvFormat,
                              uint32_t size, uint32_t mipCount);
    bool InitForCompute(DX12Context* context, DX12StateTracker* tracker, uint32_t width, uint32_t height);
    void RegisterState(DX12StateTracker* tracker, D3D12_RESOURCE_STATES state);
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpu() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetUavCpu() const;
    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    void TransitionForPixelRead(ID3D12GraphicsCommandList* commands);

private:
    bool CreateSrv(DX12Context* context, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM);
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    DX12StateTracker* m_tracker = nullptr;
    DX12Context* m_context = nullptr;
    uint32_t m_descriptorIncrement = 0;
    bool m_hasUav = false;
    DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace fbzz::renderer
