/// @file    VectorFieldImporter.hpp
/// @brief   速度場 PNG / .fga → VectorFieldAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Fluid/VectorFieldAsset.hpp>

namespace fbzz::asset {

class VectorFieldImporter final : public IAssetImporter<fluid::VectorFieldAsset> {
public:
    [[nodiscard]] std::unique_ptr<fluid::VectorFieldAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".png", ".fga" };
        return kExts;
    }
};

} // namespace fbzz::asset
