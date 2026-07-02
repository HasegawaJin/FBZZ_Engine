// FBZZ Engine
// ThumbnailCache.cpp | fbzz::hub
// Thumbnail image cache for Hub project cards
#include "ThumbnailCache.hpp"

#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdint>
#include <vector>

namespace fbzz::hub {

namespace {

namespace engine_util = fbzz::util;

} // namespace

void ThumbnailCache::Init(fbzz::renderer::ResourceManager& resources, fbzz::renderer::IImGuiRenderer& imgui)
{
    m_resources = &resources;
    m_imgui     = &imgui;
    m_textures.clear();
}

void ThumbnailCache::Clear()
{
    m_textures.clear();
    m_resources = nullptr;
    m_imgui     = nullptr;
}

const ThumbnailTexture* ThumbnailCache::GetOrLoad(const std::string& path)
{
    if (!m_resources || !m_imgui || path.empty()) {
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
    std::vector<uint8_t> bytes;
    engine_util::FileSystem::ReadBinary(engine_util::FileSystem::PathFromUtf8(path), bytes);
    if (bytes.empty()) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
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

    // WHY: RGBA8 デコード済みピクセルから GPU テクスチャを作る処理はバックエンド依存なので、
    //      DX11 具象 API を直接叩かず ResourceManager に委譲する (DX12 でもそのまま動く)。
    const auto handle = m_resources->CreateTexture(
        pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    stbi_image_free(pixels);
    if (!handle.IsValid()) {
        return false;
    }

    // ImGui::Image へ渡す ID は SRV 等のバックエンド固有型なので IImGuiRenderer に解決させる。
    void* imTextureId = m_imgui->GetImTextureID(handle, *m_resources);
    if (!imTextureId) {
        m_resources->Release(handle);
        return false;
    }

    texture.handle      = handle;
    texture.imTextureId = imTextureId;
    texture.width       = width;
    texture.height      = height;
    return true;
}

} // namespace fbzz::hub
