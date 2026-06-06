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
#include <DirectXTex.h>
#include <Windows.h>
#include <string>
#include <vector>

namespace fbzz::renderer
{

namespace {

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

} // namespace

bool DX11Texture::Init(ID3D11Device* device, ID3D11DeviceContext* context, const std::string& path)
{
    // DirectXTex の API は wchar_t パスを要求するため、UTF-8 から wide path に変換する。
    // WHY: AssetManager は project root を UTF-8 絶対パスとして渡す。
    //      char をそのまま wchar_t に詰めると、日本語フォルダへ移動した配布版で読み込みに失敗する。
    std::wstring wpath = Utf8ToWide(path);

    DirectX::ScratchImage image;
    HRESULT hr;

    // 拡張子でロード関数を分岐:
    //   DDS / FZTX → LoadFromDDSFile (BC 圧縮・ミップ内包)
    //   TGA        → LoadFromTGAFile
    //   その他     → LoadFromWICFile (PNG / JPG / BMP 等)
    //
    // .fztex は "FZTX ヘッダー (16B) + DDS" の連結。
    // WHY: ファイル先頭 16 バイトを読み飛ばして残りを DDS メモリブロブとして渡す。
    if (path.ends_with(".fztex") || path.ends_with(".FZTX"))
    {
        // ファイル全体を読み込み FZTX ヘッダーをスキップする
        HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            FBZZ_LOG_ERROR("Texture load failed (open): %s", path.c_str());
            return false;
        }
        LARGE_INTEGER fileSize{};
        GetFileSizeEx(hFile, &fileSize);
        constexpr DWORD kFztxHeaderSize = 16;
        if (fileSize.QuadPart <= kFztxHeaderSize) {
            CloseHandle(hFile);
            FBZZ_LOG_ERROR("Texture load failed (too small): %s", path.c_str());
            return false;
        }
        const DWORD ddsBytes = static_cast<DWORD>(fileSize.QuadPart) - kFztxHeaderSize;
        std::vector<uint8_t> ddsData(ddsBytes);
        SetFilePointer(hFile, kFztxHeaderSize, nullptr, FILE_BEGIN);
        DWORD bytesRead = 0;
        ReadFile(hFile, ddsData.data(), ddsBytes, &bytesRead, nullptr);
        CloseHandle(hFile);
        if (bytesRead != ddsBytes) {
            FBZZ_LOG_ERROR("Texture load failed (read): %s", path.c_str());
            return false;
        }
        hr = DirectX::LoadFromDDSMemory(ddsData.data(), ddsData.size(),
                                         DirectX::DDS_FLAGS_NONE, nullptr, image);
    }
    else if (path.ends_with(".dds") || path.ends_with(".DDS"))
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

} // namespace fbzz::renderer
