/// @file    DX12Texture.hpp
/// @brief   Default Heap テクスチャと永続CPU SRVを保持する DirectX 12 実装。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Graphics/Renderer/ITexture.hpp>
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
    /// @note CPU で焼いたミップ連鎖 (RGBA8) をそのまま全段転送する。
    /// @param mips 0 段目から順。行ピッチは width*4 固定。
    bool InitFromDataMips(DX12Context* context, const TextureMipData* mips, uint32_t mipCount);
    /// @brief InitFromDataMips の待たない版。転送を graphics queue へ投入して戻る。
    /// @param outUploadFence 転送完了で通過するフェンス値。
    /// @note 同じキューの後続コマンドは転送の後に実行されるので、状態は生成直後から PIXEL_SHADER_RESOURCE として扱える。
    bool InitFromDataMipsAsync(DX12Context* context, const TextureMipData* mips, uint32_t mipCount,
                               uint64_t& outUploadFence);
    /// @note 3D テクスチャ (R8G8B8A8_UNORM)。ボリューメトリック雲ノイズ / 3D カラー LUT 用 (DX11 の Init3DFromData 相当)。
    /// @note 未実装だと 3D テクスチャが null になり、シェーダーの Texture3D スロットへ null Texture2D SRV が
    /// @note       バインドされて次元不一致の検証エラーになる。
    bool InitFromData3D(DX12Context* context, const uint8_t* rgba,
                        uint32_t width, uint32_t height, uint32_t depth);
    bool InitFromResource(DX12Context* context, ID3D12Resource* resource, DXGI_FORMAT srvFormat,
                          uint32_t width, uint32_t height);
    bool InitCubeFromResource(DX12Context* context, ID3D12Resource* resource, DXGI_FORMAT srvFormat,
                              uint32_t size, uint32_t mipCount);
    bool InitForCompute(DX12Context* context, DX12StateTracker* tracker, uint32_t width, uint32_t height);
    /// @note RGBA16F の 3D テクスチャを SRV + UAV 両用で作る (フロクセルボリューム用)。
    bool InitForCompute3D(DX12Context* context, DX12StateTracker* tracker,
                          uint32_t width, uint32_t height, uint32_t depth);
    /// @note CPU から矩形単位で書き換えられるテクスチャ (フォントの動的アトラス用)。
    /// @note DEFAULT ヒープにゼロ初期化して作り、PIXEL_SHADER_RESOURCE 状態で待機する。
    bool InitDynamic(DX12Context* context, DX12StateTracker* tracker,
                     uint32_t width, uint32_t height, DynamicTextureFormat format);
    void RegisterState(DX12StateTracker* tracker, D3D12_RESOURCE_STATES state);
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetDepth() const override { return m_depth; }

    /// @brief 永続 bindless ディスクリプタの添字。初回呼び出しで確保して発行する。
    /// @return bindless 非対応、SRV 未生成、または枠が枯渇していれば INVALID_BINDLESS_INDEX。
    /// @note 遅延発行にしているのは、Init 経路が 8 本あり全てに発行を足すと «足し忘れたものだけ
    /// @note       黙って bindless から消える» 事故が起きるため。加えて、実際に bindless で参照された
    /// @note       テクスチャだけが枠を消費するので容量も節約できる。
    /// @note 書き込むのは «まだ誰も参照していない新しい枠» なので、GPU 実行中に
    /// @note       shader-visible ヒープへコピーしても競合しない。
    /// @see  Docs/design/bindless.md
    uint32_t GetBindlessIndex() const override;

    /// @brief UAV 側の永続 bindless 添字。
    /// @return UAV を持たないテクスチャ (InitForCompute* 以外) は INVALID_BINDLESS_INDEX。
    /// @note SRV と UAV はディスクリプタが別物なので枠も別に取る。
    uint32_t GetBindlessUavIndex() const override;

    bool UpdateRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      const void* pixels, uint32_t srcRowPitch) override;
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpu() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetUavCpu() const;
    ID3D12Resource* GetResource() const { return m_resource.Get(); }
    void TransitionForPixelRead(ID3D12GraphicsCommandList* commands);

private:
    bool CreateSrv(DX12Context* context, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM);
    /// @note 矩形ぶんのアップロードバッファを作って CopyTextureRegion する共通処理。
    /// @note currentState には呼び出し時点のリソース状態を渡す (生成直後は COPY_DEST、
    /// @note 通常運用時は PIXEL_SHADER_RESOURCE)。完了後は必ず PIXEL_SHADER_RESOURCE になる。
    bool UploadRegionInternal(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                              const void* pixels, uint32_t srcRowPitch,
                              D3D12_RESOURCE_STATES currentState);
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    DX12StateTracker* m_tracker = nullptr;
    DX12Context* m_context = nullptr;
    uint32_t m_descriptorIncrement = 0;
    bool m_hasUav = false;
    /// @note InitDynamic で作られたときだけ true。UpdateRegion はこれを見て可否を判断する。
    bool m_isDynamic = false;
    uint32_t m_bytesPerPixel = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t m_width = 0;
    /// @note SRV が見せるミップ段数。ファイルから読んだ DDS だけが 1 を超える。
    uint32_t m_mipLevels = 1;
    uint32_t m_height = 0;
    /// @note 3D テクスチャの奥行き。2D では 1 のまま。
    uint32_t m_depth = 1;
    /// @note 永続 bindless 枠。GetBindlessIndex() の初回呼び出しで確保するため mutable。
    mutable uint32_t m_bindlessIndex = INVALID_BINDLESS_INDEX;
    mutable uint32_t m_bindlessUavIndex = INVALID_BINDLESS_INDEX;
};

} /// @note namespace fbzz::renderer
