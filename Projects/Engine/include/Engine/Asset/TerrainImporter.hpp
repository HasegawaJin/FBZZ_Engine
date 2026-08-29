/// @file    TerrainImporter.hpp
/// @brief   .terrain バイナリ → TerrainAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/TerrainAsset.hpp>

namespace fbzz::asset {

class TerrainImporter final : public IAssetImporter<TerrainAsset> {
public:
    [[nodiscard]] std::unique_ptr<TerrainAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".terrain" };
        return kExts;
    }
};

} // namespace fbzz::asset
