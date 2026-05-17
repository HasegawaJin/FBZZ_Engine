// FBZZ Engine
// DX11Texture.hpp | fbzz::renderer
// DX11 2D テクスチャ (DirectXTex による画像読み込み)
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
#include <engine/Renderer/ITexture.hpp>

namespace fbzz::renderer
{

class DX11Texture : public ITexture
{
public:
    // path: PNG / JPG / BMP / TGA / DDS いずれも受け付ける
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& path);

    uint32_t GetWidth()  const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }

    // PSSetShaderResources に渡す SRV (Submit 時に DX11Renderer がアクセスする)
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    uint32_t m_width  = 0;
    uint32_t m_height = 0;
};

} // namespace fbzz::renderer
