// FBZZ Engine
// DX11Texture.cpp | fbzz::renderer
// DX11 2D テクスチャ (DirectXTex による画像読み込み)
//
// ole32.lib は DirectXTex が内部で CoCreateInstance (WIC) を呼ぶために必要。
#pragma comment(lib, "ole32.lib")

#include "DX11Texture.hpp"
#include <Engine/Core/Logger.hpp>
#include <DirectXTex.h>
#include <string>

namespace fbzz::renderer
{

bool DX11Texture::Init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& path)
{
    // DirectXTex の API は wchar_t パスを要求するため変換する。
    // ASCII パスのみ対応。日本語パスが必要になった場合は MultiByteToWideChar に切り替える。
    std::wstring wpath(path.begin(), path.end());

    DirectX::ScratchImage image;
    HRESULT hr;

    // 拡張子でロード関数を分岐:
    //   DDS → LoadFromDDSFile  (BC 圧縮・キューブマップ・ミップ内包に対応)
    //   TGA → LoadFromTGAFile  (アルファ付きテクスチャに使われやすい)
    //   その他 → LoadFromWICFile (PNG / JPG / BMP 等を OS の WIC コーデックで処理)
    if (path.ends_with(".dds") || path.ends_with(".DDS"))
    {
        hr = DirectX::LoadFromDDSFile(wpath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
    }
    else if (path.ends_with(".tga") || path.ends_with(".TGA"))
    {
        hr = DirectX::LoadFromTGAFile(wpath.c_str(), nullptr, image);
    }
    else
    {
        hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    }

    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("テクスチャ読み込み失敗: %s", path.c_str());
        return false;
    }

    // CreateShaderResourceView はミップマップ全体を一括で SRV に変換する。
    // DDS にミップが内包されている場合はそのまま使い、
    // PNG 等の場合は image.GetImageCount() == 1 のため単一ミップの SRV になる。
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    hr = DirectX::CreateShaderResourceView(
        device,
        image.GetImages(),
        image.GetImageCount(),
        image.GetMetadata(),
        m_srv.GetAddressOf());

    if (FAILED(hr))
    {
        FBZZ_LOG_ERROR("SRV 生成失敗: %s", path.c_str());
        return false;
    }

    m_width  = static_cast<uint32_t>(image.GetMetadata().width);
    m_height = static_cast<uint32_t>(image.GetMetadata().height);

    return true;
}

void DX11Texture::InitFromSRV(ID3D11ShaderResourceView* srv, uint32_t width, uint32_t height)
{
    m_srv    = srv;   // ComPtr が AddRef して共同所有する
    m_width  = width;
    m_height = height;
}

bool DX11Texture::InitForCompute(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_width  = width;
    m_height = height;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width          = width;
    texDesc.Height         = height;
    texDesc.MipLevels      = 1;
    texDesc.ArraySize      = 1;
    texDesc.Format         = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc     = { 1, 0 };
    texDesc.Usage          = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, tex.GetAddressOf());
    if (FAILED(hr)) { FBZZ_LOG_ERROR("InitForCompute: Texture2D 生成失敗 0x%08X", (unsigned)hr); return false; }

    hr = device->CreateShaderResourceView(tex.Get(), nullptr, m_srv.GetAddressOf());
    if (FAILED(hr)) { FBZZ_LOG_ERROR("InitForCompute: SRV 生成失敗 0x%08X", (unsigned)hr); return false; }

    hr = device->CreateUnorderedAccessView(tex.Get(), nullptr, m_uav.GetAddressOf());
    if (FAILED(hr)) { FBZZ_LOG_ERROR("InitForCompute: UAV 生成失敗 0x%08X", (unsigned)hr); return false; }

    return true;
}

} // namespace fbzz::renderer
