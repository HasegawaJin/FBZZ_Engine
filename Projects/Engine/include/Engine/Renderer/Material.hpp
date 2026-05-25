// FBZZ Engine
// Material.hpp | fbzz::renderer
// シェーダー・テクスチャ・パラメータの束
// MaterialParams は HLSL の MaterialConstants と一致させる。
// GPU リソースは ResourceHandle で保持し、ResourceManager が所有する。
#pragma once
#include "ResourceHandle.hpp"
#include <cstdint>
#include <string>
#include <Math/Vector4.hpp>

namespace fbzz::renderer {

class ResourceManager;

struct MaterialParams {
    math::Vector4 albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
    float metallic = 0.0f;
    float roughness = 0.8f;
    float emissiveScale = 0.0f;
    // ビット 0 = albedo テクスチャあり, ビット 1 = normal テクスチャあり。
    // HLSL 側の HAS_ALBEDO_TEX / HAS_NORMAL_TEX マクロと対応する。
    uint32_t textureMask = 0;
};

class Material {
public:
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<TextureTag> albedoTexture;
    ResourceHandle<TextureTag> normalTexture;
    ResourceHandle<ConstantBufferTag> paramsBuffer;
    MaterialParams params;

    std::string shaderPath;

    void Init(ResourceManager& resources);
    void Upload(ResourceManager& resources);
};

} // namespace fbzz::renderer
