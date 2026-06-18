// FBZZ Engine
// ImageImporter.hpp | fbzz::asset
// .tex descriptor または 生画像 (.png/.dds/.tga/.hdr) → TextureAsset ローダー
// .tex TOML がある場合: TextureImportSettings を解析して GPU ロード
// 生画像直参照の場合: GuessTextureType() でデフォルト設定を推定
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/TextureAsset.hpp>

namespace fbzz::asset {

class ImageImporter final : public IAssetImporter<TextureAsset> {
public:
    [[nodiscard]] std::unique_ptr<TextureAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = {
            ".tex", ".png", ".jpg", ".jpeg", ".tga", ".dds", ".hdr", ".exr", ".bmp"
        };
        return kExts;
    }
};

} // namespace fbzz::asset
