/// @file    Material.hpp
/// @brief   シェーダー・テクスチャ・パラメータの束。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// シェーダーリフレクション駆動方式:
/// paramData  … CB_MATERIAL と同サイズの生バイト列。SyncMaterial() が
/// MaterialAsset の params から ShaderDescriptor 経由で構築し、
/// Upload() が textureMask を書き込んで GPU へ転送する。
/// textures   … スロット番号でインデックス。ShaderDescriptor::textures に合わせる。
///
/// MaterialParams 構造体はなくなった。レイアウトは ShaderDescriptor が保証する。
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
    Material() = default;
    // WHY: paramsBuffer は Material が唯一の所有者。破棄時に返さないと、シーンの
    //      読み直し (Play/Stop も通る) やマテリアル差し替えのたびに ConstantBuffer が
    //      ResourceManager 側へ取り残され、繰り返すぶんだけ積み上がる。
    ~Material();
    // WHY コピー禁止: 値を複製するとハンドルまで複製され、片方の破棄で
    //      もう片方が参照している ConstantBuffer が解放される。
    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;
    Material(Material&& other) noexcept;
    Material& operator=(Material&& other) noexcept;

    ResourceHandle<ShaderTag>   shader;
    std::vector<ResourceHandle<TextureTag>> textures; // スロット番号でインデックス

    ResourceHandle<ConstantBufferTag> paramsBuffer;
    std::vector<uint8_t>              paramData; // CB_MATERIAL 生バイト列

    std::string shaderPath;

    // 設定だけを写した複製を作る。paramsBuffer は空のまま (次の Init が確保し直す)。
    // WHY コピーコンストラクタにしないか: ConstantBuffer の所有者は 1 つに限る。
    //     複製が同じハンドルを持つと、片方の破棄でもう片方の参照先が消える。
    [[nodiscard]] Material CloneWithoutGpuResources() const;

    // paramData のサイズが変わったとき、または paramsBuffer が無効なときに cbuffer を再作成する。
    void Init(ResourceManager& resources, uint32_t cbufferSize);

    // textureMask を textures の有効性から計算して paramData に書き込み、GPU へ転送する。
    void Upload(ResourceManager& resources, const ShaderDescriptor& desc);
};

} // namespace fbzz::renderer
