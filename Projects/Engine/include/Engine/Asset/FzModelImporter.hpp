// FBZZ Engine
// FzModelImporter.hpp | fbzz::asset
// .model バイナリ → ModelAsset ローダー
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/ModelAsset.hpp>

namespace fbzz::asset {

class FzModelImporter final : public IAssetImporter<ModelAsset> {
public:
    [[nodiscard]] std::unique_ptr<ModelAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".model" };
        return kExts;
    }
};

} // namespace fbzz::asset
