/// @file    VectorFieldImporter.hpp
/// @brief   .vfield / .fga → VectorFieldAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/VectorFieldAsset.hpp>

namespace fbzz::asset {

class VectorFieldImporter final : public IAssetImporter<VectorFieldAsset> {
public:
    [[nodiscard]] std::unique_ptr<VectorFieldAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".vfield", ".fga" };
        return kExts;
    }
};

} // namespace fbzz::asset
