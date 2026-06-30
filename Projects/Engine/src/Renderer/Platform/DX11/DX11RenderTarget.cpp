// FBZZ Engine
// DX11RenderTarget.cpp | fbzz::renderer
// DX11 オフスクリーン描画ターゲット実装
// MRT、深度、SRV 取得をまとめ、ポストプロセスやシャドウに使う。
// RenderTarget の生成と解放を DX11 リソース寿命に合わせる。
#include "DX11RenderTarget.hpp"
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

    return true;
}

bool DX11RenderTarget::InitCubemap(ID3D11Device* device, uint32_t size, uint32_t mipCount,
                                   DXGI_FORMAT format)
{
    m_isCubemap    = true;
    m_width        = size;
    m_height       = size;
    m_colorCount   = 0;            // DSV/2D カラーは持たない (面 RTV で描画する)
    m_cubeMipCount = mipCount > 0 ? mipCount : 1;

    // 6 面 Texture2DArray (MISC_TEXTURECUBE) を描画先 + サンプリング両用で生成する。
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = size;
    desc.Height           = size;
    desc.MipLevels        = m_cubeMipCount;
    desc.ArraySize        = 6;
    desc.Format           = format;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags        = D3D11_RESOURCE_MISC_TEXTURECUBE;
    // mip>1 のときは mip0 描画後に GenerateMips でローパス mip を作れるようにする。
    if (m_cubeMipCount > 1)
        desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    FBZZ_HR_CHECK(device->CreateTexture2D(&desc, nullptr, m_cubeTex.GetAddressOf()));

    // 面×mip ごとに 1 スライスの RTV を作る (Texture2DArray スライス = キューブ面)。
    m_cubeFaceRTV.assign(static_cast<size_t>(6) * m_cubeMipCount, nullptr);
    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t mip = 0; mip < m_cubeMipCount; ++mip)
        {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format                         = format;
            rtvDesc.ViewDimension                  = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.MipSlice        = mip;
            rtvDesc.Texture2DArray.FirstArraySlice = face;
            rtvDesc.Texture2DArray.ArraySize       = 1;
            FBZZ_HR_CHECK(device->CreateRenderTargetView(
                m_cubeTex.Get(), &rtvDesc,
                m_cubeFaceRTV[static_cast<size_t>(face) * m_cubeMipCount + mip].GetAddressOf()));
        }
    }

    // 全 mip を含む TextureCube SRV (Lit パス / 畳み込み入力でサンプリングする)。
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                      = format;
    srvDesc.ViewDimension               = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels       = m_cubeMipCount;
    FBZZ_HR_CHECK(device->CreateShaderResourceView(m_cubeTex.Get(), &srvDesc, m_cubeSRV.GetAddressOf()));

    return true;
}

ID3D11RenderTargetView* DX11RenderTarget::GetFaceRTV(uint32_t face, uint32_t mip) const
{
    if (!m_isCubemap || face >= 6 || mip >= m_cubeMipCount) return nullptr;
    return m_cubeFaceRTV[static_cast<size_t>(face) * m_cubeMipCount + mip].Get();
}

ID3D11ShaderResourceView* DX11RenderTarget::GetColorSRV(uint32_t index) const
{
    assert(index < m_colorCount);
    return m_srv[index].Get();
}

void DX11RenderTarget::GetRTVs(ID3D11RenderTargetView** out, uint32_t& count) const
{
    count = m_colorCount;
    for (uint32_t i = 0; i < m_colorCount; ++i)
        out[i] = m_rtv[i].Get();
}

} // namespace fbzz::renderer
