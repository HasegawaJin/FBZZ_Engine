// FBZZ Engine
// ThumbnailCache.cpp | fbzz::hub
// Thumbnail image cache for Hub project cards
#include "ThumbnailCache.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace fbzz::hub {

namespace {

std::vector<unsigned char> ReadBinary(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};

    const std::streamsize size = file.tellg();
    if (size <= 0) return {};

    std::vector<char> buffer(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(buffer.data(), size)) {
        return {};
    }

    return std::vector<unsigned char>(buffer.begin(), buffer.end());
}

} // namespace

void ThumbnailCache::Init(ID3D11Device* device)
{
    m_device = device;
    m_textures.clear();
}

void ThumbnailCache::Clear()
{
    m_textures.clear();
    m_device = nullptr;
}

const ThumbnailTexture* ThumbnailCache::GetOrLoad(const std::string& path)
{
    if (!m_device || path.empty()) {
        return nullptr;
    }

    const auto found = m_textures.find(path);
    if (found != m_textures.end()) {
        return &found->second;
    }

    ThumbnailTexture texture;
    if (!LoadTexture(path, texture)) {
        return nullptr;
    }

    auto [it, inserted] = m_textures.emplace(path, std::move(texture));
    return inserted ? &it->second : nullptr;
}

bool ThumbnailCache::LoadTexture(const std::string& path, ThumbnailTexture& texture) const
{
    const std::vector<unsigned char> bytes = ReadBinary(std::filesystem::path(path));
    if (bytes.empty()) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        bytes.data(),
        static_cast<int>(bytes.size()),
        &width,
        &height,
        &channels,
        STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) {
            stbi_image_free(pixels);
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = pixels;
    initData.SysMemPitch = static_cast<UINT>(width * 4);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dTexture;
    const HRESULT textureResult = m_device->CreateTexture2D(&desc, &initData, d3dTexture.GetAddressOf());
    stbi_image_free(pixels);
    if (FAILED(textureResult)) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(m_device->CreateShaderResourceView(
            d3dTexture.Get(),
            &srvDesc,
            texture.shaderResourceView.GetAddressOf()))) {
        return false;
    }

    texture.width = width;
    texture.height = height;
    return true;
}

} // namespace fbzz::hub
