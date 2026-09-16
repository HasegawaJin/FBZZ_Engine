/// @file    ImageImporter.hpp
/// @brief   生画像 (.png/.dds/.tga/.hdr 等) → TextureAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// 元画像の隣に "<画像>.meta" サイドカーがあれば TextureImportSettings を解析して GPU ロード
/// サイドカーが無い場合: GuessTextureType() でデフォルト設定を推定
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
        // ".meta" は列挙しない: サイドカーは元画像をインポートする際に受動的に発見される。
        static constexpr std::string_view kExts[] = {
            ".png", ".jpg", ".jpeg", ".tga", ".dds", ".hdr", ".exr", ".bmp"
        };
        return kExts;
    }
};

} // namespace fbzz::asset
