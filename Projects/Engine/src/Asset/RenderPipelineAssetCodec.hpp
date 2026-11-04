/// @file    RenderPipelineAssetCodec.hpp
/// @brief   Private typed TOML boundary for rendering configuration assets.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <toml++/toml.hpp>

namespace fbzz::asset {

struct RenderPipelineAssetCodec {
    /// @return Invalid types, unsupported schema or non-finite values leave out unchanged.
    FBZZ_ENGINE_API static bool Load(const toml::table& table, RenderPipelineAsset& out);
    /// @return Invalid edited values leave out unchanged and cannot overwrite a valid disk asset.
    FBZZ_ENGINE_API static bool Save(const RenderPipelineAsset& asset, toml::table& out);
    /// @note Shared by .fzdata and the inline ProjectSettings fallback; invalid input leaves out unchanged.
    FBZZ_ENGINE_API static bool LoadHybridQuality(const toml::table& table, renderer::HybridQualitySettings& out);
    FBZZ_ENGINE_API static void SaveHybridQuality(const renderer::HybridQualitySettings& value, toml::table& out);
};

} /// @note namespace fbzz::asset
