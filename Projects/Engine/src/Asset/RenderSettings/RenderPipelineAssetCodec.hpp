/// @file    RenderPipelineAssetCodec.hpp
/// @brief   描画設定アセットを型付き値と TOML の間で変換する非公開の境界。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/RenderSettings/RenderPipelineAsset.hpp>
#include <toml++/toml.hpp>

namespace fbzz::asset {

struct RenderPipelineAssetCodec {
    /// @return 型が不正、スキーマが未対応、または値が非有限の場合は out を変更しない。
    FBZZ_ENGINE_API static bool Load(const toml::table& table, RenderPipelineAsset& out);
    /// @return 編集値が不正な場合は out を変更せず、ディスク上の有効なアセットを上書きできない。
    FBZZ_ENGINE_API static bool Save(const RenderPipelineAsset& asset, toml::table& out);
    /// @brief ハイブリッド描画の品質設定を読み込む。
    /// @param[in] table 品質設定の TOML テーブル。省略した項目は既定値を使う。
    /// @param[out] out 型と値域の検証に成功した場合だけ設定を格納する。
    /// @note .fzdata と ProjectSettings のインラインの代替設定で共用する。入力が不正なら out は変更しない。
    FBZZ_ENGINE_API static bool LoadHybridQuality(const toml::table& table, renderer::HybridQualitySettings& out);
    FBZZ_ENGINE_API static void SaveHybridQuality(const renderer::HybridQualitySettings& value, toml::table& out);
};

} // namespace fbzz::asset
