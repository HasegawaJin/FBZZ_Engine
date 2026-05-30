// FBZZ Engine
// Material.cpp | fbzz::renderer
// Material の GPU パラメーター初期化と転送
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <cstring>

namespace fbzz::renderer {

void Material::Init(ResourceManager& resources, uint32_t cbufferSize)
{
    // サイズが変わった場合は cbuffer を再作成する。
    if (paramsBuffer.IsValid() && paramData.size() == cbufferSize) return;
    // WHY: 呼び出し元が事前に paramData を設定している場合 (SyncMaterial / SceneSerializer) は
    //      サイズが一致していれば上書きしない。サイズ不一致のときだけ 0 初期化する。
    if (paramData.size() != cbufferSize)
        paramData.assign(cbufferSize, 0u);
    paramsBuffer = resources.CreateConstantBuffer(cbufferSize);
}

void Material::Upload(ResourceManager& resources, const ShaderDescriptor& desc)
{
    if (!paramsBuffer.IsValid())
        Init(resources, desc.cbufferSize);
    if (!paramsBuffer.IsValid()) return;
    if (paramData.size() != desc.cbufferSize) return;

    // textureMask を textures の有効性から計算し paramData に書き込む。
    // エディターでテクスチャを差し替えても自動反映されるよう毎フレーム再計算する。
    if (desc.textureMaskOffset != UINT32_MAX
        && desc.textureMaskOffset + 4 <= static_cast<uint32_t>(paramData.size()))
    {
        uint32_t mask = 0;
        for (size_t i = 0; i < textures.size() && i < 5; ++i)
            if (textures[i].IsValid()) mask |= (1u << i);
        std::memcpy(paramData.data() + desc.textureMaskOffset, &mask, sizeof(uint32_t));
    }

    resources.Update(paramsBuffer, paramData.data(),
                     static_cast<uint32_t>(paramData.size()));
}

} // namespace fbzz::renderer
