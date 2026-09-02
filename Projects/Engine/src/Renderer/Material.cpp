/// @file    Material.cpp
/// @brief   Material の GPU パラメーター初期化と転送。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <cstring>
#include <utility>

namespace fbzz::renderer {

namespace {

// WHY Active() を使うか: Material は Model / MaterialComponent / 各パスのキャッシュへ
//     埋め込まれて畳まれるため、破棄地点へ ResourceManager& を渡す経路が無い。
//     ResourceManager より後に消える Material では Active() が空になり、
//     そのときは解放先そのものが既に無いので何もしないのが正しい。
void ReleaseParamsBuffer(ResourceHandle<ConstantBufferTag>& buffer)
{
    if (!buffer.IsValid()) return;
    if (ResourceManager* resources = ResourceManager::Active())
        resources->Release(buffer);
    buffer = {};
}

} // namespace

Material::~Material()
{
    ReleaseParamsBuffer(paramsBuffer);
}

Material::Material(Material&& other) noexcept
    : shader(other.shader)
    , textures(std::move(other.textures))
    , paramsBuffer(other.paramsBuffer)
    , paramData(std::move(other.paramData))
    , shaderPath(std::move(other.shaderPath))
{
    other.paramsBuffer = {};
}

Material& Material::operator=(Material&& other) noexcept
{
    if (this == &other) return *this;
    ReleaseParamsBuffer(paramsBuffer);
    shader       = other.shader;
    textures     = std::move(other.textures);
    paramsBuffer = other.paramsBuffer;
    paramData    = std::move(other.paramData);
    shaderPath   = std::move(other.shaderPath);
    other.paramsBuffer = {};
    return *this;
}

Material Material::CloneWithoutGpuResources() const
{
    Material clone;
    clone.shader     = shader;
    clone.textures   = textures;
    clone.paramData  = paramData;
    clone.shaderPath = shaderPath;
    return clone;
}

void Material::Init(ResourceManager& resources, uint32_t cbufferSize)
{
    // サイズが変わった場合は cbuffer を再作成する。
    if (paramsBuffer.IsValid() && paramData.size() == cbufferSize) return;
    if (paramsBuffer.IsValid()) {
        // WHY: ハンドルの上書きだけでは旧 ConstantBuffer が ResourceManager に残るため、
        //      シェーダー変更でレイアウトが変わる前に明示的に解放する。
        resources.Release(paramsBuffer);
        paramsBuffer = {};
    }
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
        for (size_t i = 0; i < textures.size() && i < 8; ++i)
            if (textures[i].IsValid()) mask |= (1u << i);
        std::memcpy(paramData.data() + desc.textureMaskOffset, &mask, sizeof(uint32_t));
    }

    resources.Update(paramsBuffer, paramData.data(),
                     static_cast<uint32_t>(paramData.size()));
}

} // namespace fbzz::renderer
