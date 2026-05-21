// FBZZ Engine
// DX11RenderTarget.cpp | fbzz::renderer
// DX11 オフスクリーン描画ターゲット (Render-To-Texture)
#include "DX11RenderTarget.hpp"
#include "DX11Texture.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/HResult.hpp>
#include <cassert>

namespace fbzz::renderer
{

bool DX11RenderTarget::Init(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t colorCount)
{
    assert(colorCount <= MAX_COLOR);
    m_width      = width;
    m_height     = height;
    m_colorCount = colorCount;

    // -------------------------------------------------------------------------
    // カラーバッファ (colorCount=0 の深度専用 RT ではスキップ)
    // -------------------------------------------------------------------------
    if (colorCount > 0)
    {
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

            auto tex = std::make_shared<DX11Texture>();
            tex->InitFromSRV(m_srv[i].Get(), width, height);
            m_colorTexture[i] = tex;
        }
    }

    // -------------------------------------------------------------------------
    // 深度バッファ (全 RT で生成)
    //   R32_TYPELESS + DSV(D32_FLOAT) + SRV(R32_FLOAT) の組み合わせで
    //   深度書き込みと SRV 読み取りを両立させる。
    // -------------------------------------------------------------------------
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width            = width;
    depthDesc.Height           = height;
    depthDesc.MipLevels        = 1;
    depthDesc.ArraySize        = 1;
    depthDesc.Format           = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage            = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags        = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    FBZZ_HR_CHECK(device->CreateTexture2D(&depthDesc, nullptr, m_depthBuffer.GetAddressOf()));

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    FBZZ_HR_CHECK(device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_dsv.GetAddressOf()));

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                    = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels       = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    FBZZ_HR_CHECK(device->CreateShaderResourceView(m_depthBuffer.Get(), &srvDesc, m_depthSRV.GetAddressOf()));

    auto depthTex = std::make_shared<DX11Texture>();
    depthTex->InitFromSRV(m_depthSRV.Get(), width, height);
    m_depthTexture = depthTex;

    return true;
}

std::shared_ptr<ITexture> DX11RenderTarget::GetColorTexture(uint32_t index) const
{
    assert(index < m_colorCount);
    return m_colorTexture[index];
}

std::shared_ptr<ITexture> DX11RenderTarget::GetDepthTexture() const
{
    return m_depthTexture;
}

void DX11RenderTarget::GetRTVs(ID3D11RenderTargetView** out, uint32_t& count) const
{
    count = m_colorCount;
    for (uint32_t i = 0; i < m_colorCount; ++i)
        out[i] = m_rtv[i].Get();
}

} // namespace fbzz::renderer
