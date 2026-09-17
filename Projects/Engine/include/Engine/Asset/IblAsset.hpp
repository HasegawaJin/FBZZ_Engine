/// @file    IblAsset.hpp
/// @brief   IBL ベイク済みテクスチャを束ねる Runtime アセット。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// IblImporter が .ibl descriptor を読んで 4 枚の DDS を ResourceManager 経由で GPU ロードし、
/// 各 ResourceHandle をここに格納する。
///
/// シェーダーへのバインド:
/// IBL.hlsli の EvaluateIBL() が期待するスロットに合わせて Binding.hlsli で定義済み:
/// TEX_IBL_IRRADIANCE  t16
/// TEX_IBL_PREFILTER   t17
/// TEX_IBL_BRDF_LUT    t18
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>

namespace fbzz::asset {

struct IblAsset {
    renderer::ResourceHandle<renderer::TextureTag> environmentCubemap; ///< 参照用 (skybox 兼用)
    renderer::ResourceHandle<renderer::TextureTag> irradianceCubemap;  ///< t16
    renderer::ResourceHandle<renderer::TextureTag> prefilteredCubemap; ///< t17
    renderer::ResourceHandle<renderer::TextureTag> brdfLut;            ///< t18

    /// PBR シェーダーで prefilterMap.SampleLevel(samp, R, mip) に渡す最大 mip レベル
    uint32_t prefilteredMipCount = 5;

    bool IsValid() const {
        return environmentCubemap.IsValid()
            && irradianceCubemap.IsValid()
            && prefilteredCubemap.IsValid()
            && brdfLut.IsValid();
    }
};

} // namespace fbzz::asset
