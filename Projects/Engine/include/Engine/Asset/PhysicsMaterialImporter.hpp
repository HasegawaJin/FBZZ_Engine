/// @file    PhysicsMaterialImporter.hpp
/// @brief   .physmat TOML → PhysicsMaterialAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-08-16
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>

namespace fbzz::asset {

class PhysicsMaterialImporter final : public IAssetImporter<PhysicsMaterialAsset> {
public:
    [[nodiscard]] FBZZ_ENGINE_API std::unique_ptr<PhysicsMaterialAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".physmat" };
        return kExts;
    }
};

} /// @note namespace fbzz::asset
