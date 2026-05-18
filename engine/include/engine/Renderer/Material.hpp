// FBZZ Engine
// Material.hpp | fbzz::renderer
// シェーダー・テクスチャ・パラメータのセット
#pragma once
#include <memory>
#include "IShader.hpp"
#include "ITexture.hpp"
#include "IConstantBuffer.hpp"
#include <math/Vector4.hpp>

namespace fbzz::renderer {

class IRenderer;

struct MaterialParams {
    math::Vector4 albedo    = { 1.0f, 1.0f, 1.0f, 1.0f };
    float         metallic  = 0.0f;
    float         roughness = 0.8f;
    float         _pad[2]   = {};
};

class Material {
public:
    std::shared_ptr<IShader>         shader;
    std::shared_ptr<ITexture>        albedoTexture;  // nullptr = 単色
    std::shared_ptr<IConstantBuffer> paramsBuffer;
    MaterialParams                   params;

    void Init(IRenderer& renderer);
    void Upload();
};

} // namespace fbzz::renderer
