/// @file    ParticleCurveImporter.hpp
/// @brief   .curve / .gradient → ParticleCurveAsset ローダー。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/ParticleCurveAsset.hpp>

namespace fbzz::asset {

class ParticleCurveImporter final : public IAssetImporter<ParticleCurveAsset> {
public:
    [[nodiscard]] std::unique_ptr<ParticleCurveAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".curve", ".gradient" };
        return kExts;
    }
};

} // namespace fbzz::asset
