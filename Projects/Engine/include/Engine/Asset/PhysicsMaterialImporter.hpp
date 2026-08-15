// FBZZ Engine
// PhysicsMaterialImporter.hpp | fbzz::asset
// .physmat TOML → PhysicsMaterialAsset ローダー
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>

namespace fbzz::asset {

class PhysicsMaterialImporter final : public IAssetImporter<PhysicsMaterialAsset> {
public:
    [[nodiscard]] std::unique_ptr<PhysicsMaterialAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".physmat" };
        return kExts;
    }
};

} // namespace fbzz::asset
