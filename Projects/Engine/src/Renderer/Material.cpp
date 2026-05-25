// FBZZ Engine
// Material.cpp | fbzz::renderer
// Material の GPU パラメーター初期化と転送
// MaterialParams を定数バッファへアップロードし、テクスチャマスクを同期する。
// ResourceManager 経由でリソース実体へアクセスする。
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/ResourceManager.hpp"

namespace fbzz::renderer {

void Material::Init(ResourceManager& resources)
{
    if (paramsBuffer.IsValid()) return;

    paramsBuffer = resources.CreateConstantBuffer(sizeof(MaterialParams));
}

void Material::Upload(ResourceManager& resources)
{
    if (!paramsBuffer.IsValid())
        Init(resources);
    if (!paramsBuffer.IsValid()) return;

    params.textureMask = 0u;
    if (albedoTexture.IsValid()) params.textureMask |= (1u << 0);
    if (normalTexture.IsValid()) params.textureMask |= (1u << 1);
    resources.Update(paramsBuffer, &params, sizeof(MaterialParams));
}

} // namespace fbzz::renderer
