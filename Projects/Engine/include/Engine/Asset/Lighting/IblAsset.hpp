/// @file    IblAsset.hpp
/// @brief   IBL ベイク済みテクスチャを束ねる Runtime アセット。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note IblImporter が .ibl descriptor を読んで 4 枚の DDS を ResourceManager 経由で GPU ロードし、
/// @note 各 ResourceHandle をここに格納する。

/// @note シェーダーへのバインド:
/// @note IBL.hlsli の EvaluateIBL() が期待するスロットに合わせて Binding.hlsli で定義済み:
/// @note TEX_IBL_IRRADIANCE  t16
/// @note TEX_IBL_PREFILTER   t17
/// @note TEX_IBL_BRDF_LUT    t18
#pragma once
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <cstdint>
#include <Engine/Asset/Streaming/AssetStreaming.hpp>
#include <Engine/Asset/Texture/TextureAsset.hpp>
#include <array>

namespace fbzz::asset {

struct IblAsset {
    std::array<AssetLease<TextureAsset>, 4> textureLeases; ///< descriptor が保持する DDS を自動解放から守る。GPU 実体の所有は ResourceManager に残る。
    renderer::ResourceHandle<renderer::TextureTag> environmentCubemap; ///< 参照用 (skybox 兼用)
    renderer::ResourceHandle<renderer::TextureTag> irradianceCubemap; ///< 拡散間接照明用の照度キューブの資源ハンドル。シェーダーの t16。
    renderer::ResourceHandle<renderer::TextureTag> prefilteredCubemap; ///< 粗さ別の鏡面反射に使う事前フィルター済み環境キューブ。シェーダーの t17。
    renderer::ResourceHandle<renderer::TextureTag> brdfLut; ///< 鏡面 IBL の BRDF 積分結果を照会するテクスチャ。シェーダーの t18。

    uint32_t prefilteredMipCount = 5; ///< PBR シェーダーで prefilterMap.SampleLevel(samp, R, mip) に渡す最大 mip レベル

    bool IsValid() const {
        return environmentCubemap.IsValid()
            && irradianceCubemap.IsValid()
            && prefilteredCubemap.IsValid()
            && brdfLut.IsValid();
    }
};

} // namespace fbzz::asset
