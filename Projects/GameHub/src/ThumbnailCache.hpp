// FBZZ Engine
// ThumbnailCache.hpp | fbzz::hub
// Thumbnail image cache for Hub project cards
//
// WHY: 以前は ID3D11Device を直接受け取り DX11 テクスチャ / SRV を生成していたが、
//      DX12 移行に備え GPU リソース生成を Engine の抽象層へ寄せる。テクスチャ生成は
//      ResourceManager、ImGui 表示用 ID の解決は IImGuiRenderer に委譲し、
//      GameHub からは DX11 具象型を排除する。
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>

#include <string>
#include <unordered_map>

namespace fbzz::renderer {
class ResourceManager;
class IImGuiRenderer;
} // namespace fbzz::renderer

namespace fbzz::hub {

struct ThumbnailTexture {
    // ResourceManager が所有する GPU テクスチャへのハンドル。
    fbzz::renderer::ResourceHandle<fbzz::renderer::TextureTag> handle;
    // ImGui::Image へ渡すテクスチャ ID (SRV 等の具体型は抽象境界の内側に隠す)。
    void* imTextureId = nullptr;
    int   width = 0;
    int   height = 0;
};

class ThumbnailCache {
public:
    // GPU リソース生成用の ResourceManager と、ImGui 表示 ID を解決する IImGuiRenderer を束ねる。
    void Init(fbzz::renderer::ResourceManager& resources, fbzz::renderer::IImGuiRenderer& imgui);
    void Clear();

    [[nodiscard]] const ThumbnailTexture* GetOrLoad(const std::string& path);

private:
    [[nodiscard]] bool LoadTexture(const std::string& path, ThumbnailTexture& texture) const;

    fbzz::renderer::ResourceManager* m_resources = nullptr;
    fbzz::renderer::IImGuiRenderer*  m_imgui = nullptr;
    std::unordered_map<std::string, ThumbnailTexture> m_textures;
};

} // namespace fbzz::hub
