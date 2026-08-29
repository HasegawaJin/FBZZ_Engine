/// @file    ModelAssetImporter.hpp
/// @brief   .fzasset バイナリ → ModelAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/ModelAsset.hpp>

namespace fbzz::asset {

class ModelAssetImporter final : public IAssetImporter<ModelAsset> {
public:
    [[nodiscard]] std::unique_ptr<ModelAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".fzasset" };
        return kExts;
    }
};

} // namespace fbzz::asset
