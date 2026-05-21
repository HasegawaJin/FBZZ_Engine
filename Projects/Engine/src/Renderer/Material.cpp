// FBZZ Engine
// Material.cpp | fbzz::renderer
// Material の GPU バッファ初期化とアップロード
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include <cassert>

namespace fbzz::renderer {

void Material::Init(IRenderer& renderer)
{
    paramsBuffer = renderer.CreateConstantBuffer(sizeof(MaterialParams));
    assert(paramsBuffer);
}

void Material::Upload()
{
    assert(paramsBuffer);
    params.textureMask = 0u;
    if (albedoTexture) params.textureMask |= (1u << 0);
    if (normalTexture) params.textureMask |= (1u << 1);
    paramsBuffer->Update(&params, sizeof(MaterialParams));
}

} // namespace fbzz::renderer
