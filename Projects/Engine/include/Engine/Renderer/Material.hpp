// FBZZ Engine
// Material.hpp | fbzz::renderer
// シェーダー・テクスチャ・パラメータの束
//
// シェーダーリフレクション駆動方式:
//   paramData  … CB_MATERIAL と同サイズの生バイト列。SyncMaterial() が
//                MaterialAsset の params から ShaderDescriptor 経由で構築し、
//                Upload() が textureMask を書き込んで GPU へ転送する。
//   textures   … スロット番号でインデックス。ShaderDescriptor::textures に合わせる。
//
// MaterialParams 構造体はなくなった。レイアウトは ShaderDescriptor が保証する。
#pragma once
#include "ResourceHandle.hpp"
#include "ShaderDescriptor.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

class ResourceManager;

class Material {
public:
    ResourceHandle<ShaderTag>   shader;
    std::vector<ResourceHandle<TextureTag>> textures; // スロット番号でインデックス

    ResourceHandle<ConstantBufferTag> paramsBuffer;
    std::vector<uint8_t>              paramData; // CB_MATERIAL 生バイト列

    std::string shaderPath;

    // paramData のサイズが変わったとき、または paramsBuffer が無効なときに cbuffer を再作成する。
    void Init(ResourceManager& resources, uint32_t cbufferSize);

    // textureMask を textures の有効性から計算して paramData に書き込み、GPU へ転送する。
    void Upload(ResourceManager& resources, const ShaderDescriptor& desc);
};

} // namespace fbzz::renderer
