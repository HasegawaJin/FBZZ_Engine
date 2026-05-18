// FBZZ Engine
// Material.cpp | fbzz::renderer
// Material の GPU バッファ初期化とアップロード
#include "engine/Renderer/Material.hpp"
#include "engine/Renderer/IRenderer.hpp"
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
    paramsBuffer->Update(&params, sizeof(MaterialParams));
}

} // namespace fbzz::renderer
