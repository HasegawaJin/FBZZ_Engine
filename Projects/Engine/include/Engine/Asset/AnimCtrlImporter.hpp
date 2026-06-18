// FBZZ Engine
// AnimCtrlImporter.hpp | fbzz::asset
// .animctrl TOML → AnimatorControllerAsset ローダー
#pragma once
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/IAssetImporter.hpp>

namespace fbzz::asset {

class AnimCtrlImporter final : public IAssetImporter<AnimatorControllerAsset> {
public:
    [[nodiscard]] std::unique_ptr<AnimatorControllerAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".animctrl", ".animcontroller" };
        return kExts;
    }
};

} // namespace fbzz::asset
