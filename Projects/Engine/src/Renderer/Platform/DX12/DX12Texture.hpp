/// @file    DX12Texture.hpp
/// @brief   Default Heap テクスチャと永続CPU SRVを保持する DirectX 12 実装。
/// @author  Hasegawa Jin
/// @date    2026-07-15
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
    // RGBA16F の 3D テクスチャを SRV + UAV 両用で作る (フロクセルボリューム用)。
    bool InitForCompute3D(DX12Context* context, DX12StateTracker* tracker,
                          uint32_t width, uint32_t height, uint32_t depth);
    // CPU から矩形単位で書き換えられるテクスチャ (フォントの動的アトラス用)。
    // DEFAULT ヒープにゼロ初期化して作り、PIXEL_SHADER_RESOURCE 状態で待機する。
    bool InitDynamic(DX12Context* context, DX12StateTracker* tracker,
                     uint32_t width, uint32_t height, DynamicTextureFormat format);
    void RegisterState(DX12StateTracker* tracker, D3D12_RESOURCE_STATES state);
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetDepth() const override { return m_depth; }

    bool UpdateRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      const void* pixels, uint32_t srcRowPitch) override;
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpu() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetUavCpu() const;
    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    void TransitionForPixelRead(ID3D12GraphicsCommandList* commands);

private:
    bool CreateSrv(DX12Context* context, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM);
    // 矩形ぶんのアップロードバッファを作って CopyTextureRegion する共通処理。
    // currentState には呼び出し時点のリソース状態を渡す (生成直後は COPY_DEST、
    // 通常運用時は PIXEL_SHADER_RESOURCE)。完了後は必ず PIXEL_SHADER_RESOURCE になる。
    bool UploadRegionInternal(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                              const void* pixels, uint32_t srcRowPitch,
                              D3D12_RESOURCE_STATES currentState);
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    DX12StateTracker* m_tracker = nullptr;
    DX12Context* m_context = nullptr;
    uint32_t m_descriptorIncrement = 0;
    bool m_hasUav = false;
    // InitDynamic で作られたときだけ true。UpdateRegion はこれを見て可否を判断する。
    bool m_isDynamic = false;
    uint32_t m_bytesPerPixel = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t m_width = 0;
    /// SRV が見せるミップ段数。ファイルから読んだ DDS だけが 1 を超える。
    uint32_t m_mipLevels = 1;
    uint32_t m_height = 0;
    // 3D テクスチャの奥行き。2D では 1 のまま。
    uint32_t m_depth = 1;
};

} // namespace fbzz::renderer
