// FBZZ Engine
// ThumbnailCache.hpp | fbzz::hub
// Thumbnail image cache for Hub project cards
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <unordered_map>

namespace fbzz::hub {

struct ThumbnailTexture {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
    int width = 0;
    int height = 0;
};

class ThumbnailCache {
public:
    void Init(ID3D11Device* device);
    void Clear();

    [[nodiscard]] const ThumbnailTexture* GetOrLoad(const std::string& path);

private:
    [[nodiscard]] bool LoadTexture(const std::string& path, ThumbnailTexture& texture) const;

    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::string, ThumbnailTexture> m_textures;
};

} // namespace fbzz::hub
