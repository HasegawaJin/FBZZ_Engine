// FBZZ Engine
// DX11Texture.hpp | fbzz::renderer
// DX11 2D テクスチャ実装
// ITexture を継承し、SRV / UAV とサイズ情報を保持する。
// DirectXTex の読み込み結果を Renderer 抽象へ接続する。
//
// 設計方針:
//   DirectXTex を採用した理由:
//     - DDS (ブロック圧縮 BC1〜BC7 含む) / TGA / PNG / JPG を統一 API で扱える
//     - ミップマップ生成・SRV 生成まで一貫して対応している
//     - Microsoft 公式ライブラリで DirectX との親和性が高い
//   外部に公開するのは ITexture::GetWidth/GetHeight と GetSRV() のみ。
//   Submit 時に DX11Renderer が static_cast して GetSRV() を取り出す。
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>
#include <Engine/Renderer/ITexture.hpp>

namespace fbzz::renderer
{

class DX11Texture : public ITexture
{
public:
    // path: PNG / JPG / BMP / TGA / DDS いずれも受け付ける
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& path);

    // CPU メモリ上の RGBA8 ピクセルデータからテクスチャを生成する (白 1×1 等の手続き生成用)
    bool InitFromData(ID3D11Device* device, const uint8_t* rgba, uint32_t width, uint32_t height);

    // CPU生成RGBA8ボリュームをImmutable Texture3Dとして作り、LUT用SRVを公開する。
    bool Init3DFromData(ID3D11Device* device, const uint8_t* rgba,
                        uint32_t width, uint32_t height, uint32_t depth);

    // 既存 SRV から直接初期化する (ResourceManager が RenderTarget の TextureTag 化に使用)
    void InitFromSRV(ID3D11ShaderResourceView* srv, uint32_t width, uint32_t height);

    // RGBA16F テクスチャを SRV + UAV 両用で生成する (Compute Shader の出力先として使用)
    bool InitForCompute(ID3D11Device* device, uint32_t width, uint32_t height);

    // RGBA16F の 3D テクスチャを SRV + UAV 両用で生成する (フロクセルボリューム用)。
    // GetDepth() で奥行きを取れる点だけが 2D 版との違い。
    bool InitForCompute3D(ID3D11Device* device,
                          uint32_t width, uint32_t height, uint32_t depth);

    // CPU から矩形単位で書き換えられるテクスチャを生成する (フォントの動的アトラス用)。
    // USAGE_DEFAULT + UpdateSubresource 方式。中身はゼロクリアして返す。
    //
    // WHY (DYNAMIC + Map ではなく DEFAULT + UpdateSubresource): D3D11_USAGE_DYNAMIC は
    //   Map(WRITE_DISCARD) で全面を書き直す用途向けで、部分更新には向かない。
    //   グリフ 1 個の追記でアトラス全体を再転送するのは無駄が大きい。
    //   DEFAULT + UpdateSubresource なら更新矩形のバイトだけを送れる。
    bool InitDynamic(ID3D11Device* device, ID3D11DeviceContext* context,
                     uint32_t width, uint32_t height, DynamicTextureFormat format);

    uint32_t GetWidth()  const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
    uint32_t GetDepth()  const override { return m_depth; }

    bool UpdateRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      const void* pixels, uint32_t srcRowPitch) override;

    // PSSetShaderResources / CSSetShaderResources に渡す SRV
    ID3D11ShaderResourceView*  GetSRV() const { return m_srv.Get(); }

    // CSSetUnorderedAccessViews に渡す UAV (InitForCompute で生成した場合のみ非 null)
    ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_uav;

    // 動的テクスチャ (InitDynamic) のときだけ保持する更新経路。
    // UpdateRegion はこの 2 つが揃っているときのみ動作し、それ以外では false を返す。
    Microsoft::WRL::ComPtr<ID3D11Texture2D>   m_dynamicTexture;
    ID3D11DeviceContext*                      m_dynamicContext = nullptr;  // 非所有 (Renderer が所有)
    uint32_t m_bytesPerPixel = 0;

    uint32_t m_width  = 0;
    uint32_t m_height = 0;
    // 3D テクスチャの奥行き。2D では 1 のまま。
    uint32_t m_depth  = 1;
};

} // namespace fbzz::renderer
