// FBZZ Engine
// TexDescSerializer.hpp | fbzz::asset
// テクスチャインポート設定サイドカー (.meta) の読み書き (IAssetSerializer<TextureAsset> 実装)
// 命名規則: 元画像に二重拡張子で付随する。例: Foo.png -> Foo.png.meta (Unity 流)
//   WHY: 元画像は末尾 ".meta" を除けば一意に導出できるため source= フィールドは持たない。
// .meta フォーマット:
//   [texture]
//   type = "color" | "normal" | "data" | "hdr" | "ui"
//   srgb = true
//   compression = "Auto" | "BC1" | "BC3" | "BC4" | "BC5" | "BC6H" | "BC7" | "None"
//   ... 他すべての TextureImportSettings フィールド
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

    // ".meta" が渡されたら末尾 ".meta" を除いた元画像パスを返す。生画像パスはそのまま返す。
    // WHY: Scene / Material / Terrain の全テクスチャ参照を、元画像 / サイドカーどちらの表記でも解決させる。
    [[nodiscard]] static bool ResolveSourcePath(
        std::string_view texturePath, std::string& outSourcePath);
};

} // namespace fbzz::asset
