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
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <array>

namespace fbzz::asset {

struct IblAsset {
    /// @note descriptor が保持する DDS を自動解放から守る。GPU 実体の所有は ResourceManager に残る。
    std::array<AssetLease<TextureAsset>, 4> textureLeases;
    renderer::ResourceHandle<renderer::TextureTag> environmentCubemap; ///< @note 参照用 (skybox 兼用)
    renderer::ResourceHandle<renderer::TextureTag> irradianceCubemap;  ///< @note t16
    renderer::ResourceHandle<renderer::TextureTag> prefilteredCubemap; ///< @note t17
    renderer::ResourceHandle<renderer::TextureTag> brdfLut;            ///< @note t18

    /// @note PBR シェーダーで prefilterMap.SampleLevel(samp, R, mip) に渡す最大 mip レベル
    uint32_t prefilteredMipCount = 5;

    bool IsValid() const {
        return environmentCubemap.IsValid()
            && irradianceCubemap.IsValid()
            && prefilteredCubemap.IsValid()
            && brdfLut.IsValid();
    }
};

} /// @note namespace fbzz::asset
