// FBZZ Engine
// Material.cpp | fbzz::renderer
// Material の GPU パラメーター初期化と転送
// textureMask を現在のハンドル状態と同期し、定数バッファへアップロードする。
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

    // textureMask はハンドルの有効性から毎フレーム再計算する。
    // エディター側でテクスチャを差し替えたとき自動で反映されるよう、
    // キャッシュではなく毎回フルリビルドにしている。
    params.textureMask = 0u;
    if (albedoTexture.IsValid())        params.textureMask |= (1u << 0);
    if (normalTexture.IsValid())        params.textureMask |= (1u << 1);
    if (metallicRoughTexture.IsValid()) params.textureMask |= (1u << 2);
    if (emissiveTexture.IsValid())      params.textureMask |= (1u << 3);
    if (aoTexture.IsValid())            params.textureMask |= (1u << 4);

    resources.Update(paramsBuffer, &params, sizeof(MaterialParams));
}

} // namespace fbzz::renderer
