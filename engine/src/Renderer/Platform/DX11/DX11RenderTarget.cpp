// FBZZ Engine
// DX11RenderTarget.cpp | fbzz::renderer
// DX11 オフスクリーン描画ターゲット (Render-To-Texture)
#include "DX11RenderTarget.hpp"
#include "DX11Texture.hpp"
#include <engine/Core/Logger.hpp>
#include <engine/Core/HResult.hpp>
#include <cassert>

namespace fbzz::renderer
{

bool DX11RenderTarget::Init(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t colorCount)
{
    assert(colorCount >= 1 && colorCount <= MAX_COLOR);
    m_width      = width;
    m_height     = height;
    m_colorCount = colorCount;

    // -------------------------------------------------------------------------
    // 全スロット共通テクスチャ設定
    //   RGBA16_FLOAT を採用する理由:
    //     - 符号付き法線ベクトル (-1〜1) を精度損失なく格納できる
    //     - アルベド (0〜1) も収容できる (精度過剰だがバッファ統一のため許容)
    //   colorCount = 1 の単一 RT でも同様。フォーマットを変えたい場合は
    //   Init のシグネチャに DXGI_FORMAT 配列を追加して対応する。
    // -------------------------------------------------------------------------
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.MipLevels        = 1;
    texDesc.ArraySize        = 1;
    texDesc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage            = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for (uint32_t i = 0; i < colorCount; ++i)
    {
        FBZZ_HR_CHECK(device->CreateTexture2D(&texDesc, nullptr, m_colorBuffer[i].GetAddressOf()));
        FBZZ_HR_CHECK(device->CreateRenderTargetView(m_colorBuffer[i].Get(), nullptr, m_rtv[i].GetAddressOf()));
        FBZZ_HR_CHECK(device->CreateShaderResourceView(m_colorBuffer[i].Get(), nullptr, m_srv[i].GetAddressOf()));

        // DX11Texture::InitFromSRV で SRV を差し込み、Submit の static_cast<DX11Texture*> と整合させる
        auto tex = std::make_shared<DX11Texture>();
        tex->InitFromSRV(m_srv[i].Get(), width, height);
        m_colorTexture[i] = tex;
    }

    return true;
}

std::shared_ptr<ITexture> DX11RenderTarget::GetColorTexture(uint32_t index) const
{
    assert(index < m_colorCount);
    return m_colorTexture[index];
}

void DX11RenderTarget::GetRTVs(ID3D11RenderTargetView** out, uint32_t& count) const
{
    count = m_colorCount;
    for (uint32_t i = 0; i < m_colorCount; ++i)
        out[i] = m_rtv[i].Get();
}

} // namespace fbzz::renderer
