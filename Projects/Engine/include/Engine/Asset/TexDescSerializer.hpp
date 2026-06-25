// FBZZ Engine
// TexDescSerializer.hpp | fbzz::asset
// .tex TOML descriptor の読み書き (IAssetSerializer<TextureAsset> 実装)
// .tex フォーマット:
//   [texture]
//   source = "relative/to/tex/file/image.png"
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
    std::string_view Extension() const override { return ".tex"; }

    // .tex は source を descriptor 基準で解決し、生画像パスは変更せず返す。
    // WHY: Scene / Material / Terrain の全テクスチャ参照を両形式へ統一対応させる。
    [[nodiscard]] static bool ResolveSourcePath(
        std::string_view texturePath, std::string& outSourcePath);
};

} // namespace fbzz::asset
