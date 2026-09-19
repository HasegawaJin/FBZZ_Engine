/// @file    TexDescSerializer.hpp
/// @brief   テクスチャインポート設定サイドカー (.meta) の読み書き (`IAssetSerializer<TextureAsset>` 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note 命名: 元画像に二重拡張子で付随する (`Foo.png` → `Foo.png.meta`、Unity 流)。元画像パスは末尾
///       `.meta` を除けば一意に導出できるため、source= フィールドは持たない。
/// @note .meta は TOML の [texture] セクションで、TextureImportSettings の全フィールドを持つ。
#pragma once
#include <Engine/Asset/IAssetSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <string>
#include <string_view>

namespace fbzz::asset {

class TexDescSerializer final : public IAssetSerializer<TextureAsset> {
public:
    [[nodiscard]] bool Save(const TextureAsset& asset, const std::string& absPath) const override;
    [[nodiscard]] bool Load(const std::string& absPath, TextureAsset& outAsset) const override;
    std::string_view Extension() const override { return ".meta"; }

    /// @brief ".meta" が渡されたら末尾を除いた元画像パスを返す。生画像パスはそのまま返す。
    /// @note Scene / Material / Terrain の全テクスチャ参照を、元画像 / サイドカーどちらの表記でも解決させる。
    [[nodiscard]] static bool ResolveSourcePath(
        std::string_view texturePath, std::string& outSourcePath);
};

} // namespace fbzz::asset
