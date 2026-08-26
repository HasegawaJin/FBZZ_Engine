// FBZZ Engine
// DX11Texture.cpp | fbzz::renderer
// DX11 2D テクスチャ実装
// DirectXTex で画像を読み込み、SRV / UAV を必要に応じて作る。
// ファイル由来とメモリ由来の両方のテクスチャ生成を扱う。
//
// ole32.lib は DirectXTex が内部で CoCreateInstance (WIC) を呼ぶために必要。
#pragma comment(lib, "ole32.lib")

#include "DX11Texture.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <DirectXTex.h>
#include <Windows.h>
#include <string>
#include <vector>

namespace fbzz::renderer
{

bool DX11Texture::Init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& path)
{
    // DirectXTex の API は wchar_t パスを要求するため、UTF-8 から wide path に変換する。
    // WHY: AssetManager は project root を UTF-8 絶対パスとして渡す。
    //      char をそのまま wchar_t に詰めると、日本語フォルダへ移動した配布版で読み込みに失敗する。
    std::wstring wpath = fbzz::util::StringUtils::ToWide(path);

    DirectX::ScratchImage image;
    HRESULT hr;

    // 拡張子でロード関数を分岐:
    //   DDS → LoadFromDDSFile (BC 圧縮・ミップ内包)
    //   TGA → LoadFromTGAFile
    //   その他 → LoadFromWICFile (PNG / JPG / BMP 等)
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
        FBZZ_LOG_ERROR("Texture load failed: %s", path.c_str());
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
        FBZZ_LOG_ERROR("SRV creation failed: %s", path.c_str());
        return false;
    }

    m_width  = static_cast<uint32_t>(image.GetMetadata().width);
    m_height = static_cast<uint32_t>(image.GetMetadata().height);

    return true;
}

bool DX11Texture::InitFromData(ID3D11Device* device, const uint8_t* rgba, uint32_t width, uint32_t height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = width;
    desc.Height           = height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc       = { 1, 0 };
    desc.Usage            = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem      = rgba;
    initData.SysMemPitch  = width * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&desc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11Texture::InitFromData: CreateTexture2D failed 0x%08X", (unsigned)hr);
        return false;
    }

    hr = device->CreateShaderResourceView(tex.Get(), nullptr, m_srv.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11Texture::InitFromData: CreateSRV failed 0x%08X", (unsigned)hr);
        return false;
    }

    m_width  = width;
    m_height = height;
    return true;
}

bool DX11Texture::Init3DFromData(ID3D11Device* device, const uint8_t* rgba,
                                 uint32_t width, uint32_t height, uint32_t depth)
{
    if (!device || !rgba || width == 0 || height == 0 || depth == 0)
        return false;

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width          = width;
    desc.Height         = height;
    desc.Depth          = depth;
    desc.MipLevels      = 1;
    desc.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage          = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem          = rgba;
    initData.SysMemPitch      = width * 4u;
    initData.SysMemSlicePitch = width * height * 4u;

    Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
    HRESULT hr = device->CreateTexture3D(&desc, &initData, texture.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11Texture::Init3DFromData: CreateTexture3D failed 0x%08X", (unsigned)hr);
        return false;
    }
    hr = device->CreateShaderResourceView(texture.Get(), nullptr, m_srv.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11Texture::Init3DFromData: CreateSRV failed 0x%08X", (unsigned)hr);
        return false;
    }

    m_width  = width;
    m_height = height;
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
    if (FAILED(hr)) { FBZZ_LOG_ERROR("InitForCompute: Texture2D creation failed 0x%08X", (unsigned)hr); return false; }

    hr = device->CreateShaderResourceView(tex.Get(), nullptr, m_srv.GetAddressOf());
    if (FAILED(hr)) { FBZZ_LOG_ERROR("InitForCompute: SRV creation failed 0x%08X", (unsigned)hr); return false; }

    hr = device->CreateUnorderedAccessView(tex.Get(), nullptr, m_uav.GetAddressOf());
    if (FAILED(hr)) { FBZZ_LOG_ERROR("InitForCompute: UAV creation failed 0x%08X", (unsigned)hr); return false; }

    return true;
}

bool DX11Texture::InitForCompute3D(ID3D11Device* device,
                                   uint32_t width, uint32_t height, uint32_t depth)
{
    if (!device || width == 0 || height == 0 || depth == 0) return false;

    m_width  = width;
    m_height = height;
    m_depth  = depth;

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width     = width;
    desc.Height    = height;
    desc.Depth     = depth;
    desc.MipLevels = 1;
    desc.Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Usage     = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D11Texture3D> tex;
    HRESULT hr = device->CreateTexture3D(&desc, nullptr, tex.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("InitForCompute3D: CreateTexture3D failed 0x%08X", (unsigned)hr);
        return false;
    }

    // 既定ビューで足りる。単一 Mip の Texture3D なので、記述子を手で組むと
    // リソース記述との食い違いをデバッグレイヤーに拾われるだけで得がない。
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, m_srv.GetAddressOf());
    if (FAILED(hr)) { FBZZ_LOG_ERROR("InitForCompute3D: SRV creation failed 0x%08X", (unsigned)hr); return false; }

    hr = device->CreateUnorderedAccessView(tex.Get(), nullptr, m_uav.GetAddressOf());
    if (FAILED(hr)) { FBZZ_LOG_ERROR("InitForCompute3D: UAV creation failed 0x%08X", (unsigned)hr); return false; }

    return true;
}

bool DX11Texture::InitDynamic(ID3D11Device* device, ID3D11DeviceContext* context,
                              uint32_t width, uint32_t height, DynamicTextureFormat format)
{
    if (!device || !context || width == 0 || height == 0) return false;

    const bool isSingleChannel = (format == DynamicTextureFormat::R8);
    m_bytesPerPixel = isSingleChannel ? 1u : 4u;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width      = width;
    desc.Height     = height;
    desc.MipLevels  = 1;   // 動的アトラスはミップを持たない (部分更新のたびに再生成できないため)
    desc.ArraySize  = 1;
    desc.Format     = isSingleChannel ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage      = D3D11_USAGE_DEFAULT;
    desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;

    // WHY (ゼロクリアした初期データを渡す): 未初期化のまま SRV を張ると、
    //   まだグリフを焼いていない領域にドライバ依存のゴミが出る。フォントアトラスでは
    //   それが「知らない模様が字の周りに出る」形で見えてしまうため、明示的に 0 で埋める。
    const size_t rowPitch = static_cast<size_t>(width) * m_bytesPerPixel;
    std::vector<uint8_t> zeros(rowPitch * height, 0u);

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem     = zeros.data();
    initData.SysMemPitch = static_cast<UINT>(rowPitch);

    HRESULT hr = device->CreateTexture2D(&desc, &initData, m_dynamicTexture.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11Texture::InitDynamic: CreateTexture2D failed 0x%08X", (unsigned)hr);
        return false;
    }

    hr = device->CreateShaderResourceView(m_dynamicTexture.Get(), nullptr, m_srv.GetAddressOf());
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("DX11Texture::InitDynamic: CreateSRV failed 0x%08X", (unsigned)hr);
        m_dynamicTexture.Reset();
        return false;
    }

    m_dynamicContext = context;
    m_width  = width;
    m_height = height;
    return true;
}

bool DX11Texture::UpdateRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                               const void* pixels, uint32_t srcRowPitch)
{
    // InitDynamic 以外で作られたテクスチャは更新できない。
    if (!m_dynamicTexture || !m_dynamicContext || !pixels) return false;
    if (width == 0 || height == 0) return true;   // 空の更新は成功扱い (呼び出し側の分岐を減らす)

    // テクスチャ外へはみ出す矩形は拒否する。
    // WHY: UpdateSubresource に範囲外の box を渡すと未定義動作になる。
    //      アトラスのパッキング側にバグがあったときに、静かに壊れるのではなく
    //      false で気づけるようにする。
    if (x + width > m_width || y + height > m_height) {
        FBZZ_LOG_ERROR("DX11Texture::UpdateRegion: region (%u,%u,%u,%u) exceeds texture %ux%u",
                       x, y, width, height, m_width, m_height);
        return false;
    }

    D3D11_BOX box = {};
    box.left   = x;
    box.top    = y;
    box.front  = 0;
    box.right  = x + width;
    box.bottom = y + height;
    box.back   = 1;

    // srcRowPitch は「更新元バッファの 1 行のバイト数」。CPU 側がアトラス全面の
    // バッファを持ったまま部分矩形だけを渡せるよう、そのまま UpdateSubresource へ流す。
    m_dynamicContext->UpdateSubresource(m_dynamicTexture.Get(), 0, &box,
                                        pixels, srcRowPitch, 0);
    return true;
}

} // namespace fbzz::renderer
