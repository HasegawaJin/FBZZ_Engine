/// @file    MatAssetImporter.hpp
/// @brief   .mat TOML → MaterialAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/MaterialAsset.hpp>

namespace fbzz::asset {

class MatAssetImporter final : public IAssetImporter<MaterialAsset> {
public:
    [[nodiscard]] std::unique_ptr<MaterialAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".mat" };
        return kExts;
    }
};

} // namespace fbzz::asset
