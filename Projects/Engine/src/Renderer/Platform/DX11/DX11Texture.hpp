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

    uint32_t GetWidth()  const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }

    // PSSetShaderResources / CSSetShaderResources に渡す SRV
    ID3D11ShaderResourceView*  GetSRV() const { return m_srv.Get(); }

    // CSSetUnorderedAccessViews に渡す UAV (InitForCompute で生成した場合のみ非 null)
    ID3D11UnorderedAccessView* GetUAV() const { return m_uav.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_uav;
    uint32_t m_width  = 0;
    uint32_t m_height = 0;
};

} // namespace fbzz::renderer
