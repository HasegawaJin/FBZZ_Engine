/// @file    Material.hpp
/// @brief   シェーダー・テクスチャ・パラメータの束。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note シェーダーリフレクション駆動方式:
/// @note paramData  … CB_MATERIAL と同サイズの生バイト列。SyncMaterial() が
/// @note MaterialAsset の params から ShaderDescriptor 経由で構築し、
/// @note Upload() が textureMask を書き込んで GPU へ転送する。
/// @note textures   … スロット番号でインデックス。ShaderDescriptor::textures に合わせる。
/// @note MaterialParams 構造体はなくなった。レイアウトは ShaderDescriptor が保証する。
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
    /// @note paramsBuffer は Material が唯一の所有者。破棄時に返さないと、シーンの読み直し
    /// @note       (Play/Stop も通る) やマテリアル差し替えのたびに ConstantBuffer が ResourceManager
    /// @note       側へ取り残され積み上がる。
    ~Material();
    /// @note コピー禁止: 値を複製するとハンドルまで複製され、片方の破棄でもう片方が
    /// @note       参照している ConstantBuffer が解放される。
    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;
    Material(Material&& other) noexcept;
    Material& operator=(Material&& other) noexcept;

    ResourceHandle<ShaderTag>   shader;
    std::vector<ResourceHandle<TextureTag>> textures; ///< @note スロット番号でインデックス

    ResourceHandle<ConstantBufferTag> paramsBuffer;
    std::vector<uint8_t>              paramData; ///< @note CB_MATERIAL 生バイト列

    std::string shaderPath;

    /// @note 設定だけを写した複製を作る。paramsBuffer は空のまま (次の Init が確保し直す)。
    /// @note コピーコンストラクタにしないのは、ConstantBuffer の所有者を 1 つに限るため。
    /// @note       複製が同じハンドルを持つと片方の破棄でもう片方の参照先が消える。
    [[nodiscard]] Material CloneWithoutGpuResources() const;

    /// @note paramData のサイズが変わったとき、または paramsBuffer が無効なときに cbuffer を再作成する。
    /// @note where は既定のまま渡すこと。cbuffer の発生位置が «この Init» ではなく
    /// @note «どの経路の Material か» として記録される (理由は Where を参照)。
    void Init(ResourceManager& resources, uint32_t cbufferSize, Where where = Where::current());

    /// @note textureMask を textures の有効性から計算して paramData に書き込み、GPU へ転送する。
    void Upload(ResourceManager& resources, const ShaderDescriptor& desc,
                Where where = Where::current());
};

} /// @note namespace fbzz::renderer
