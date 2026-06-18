// FBZZ Engine
// FzAnimImporter.hpp | fbzz::asset
// .anim バイナリ → AnimationClip ローダー
#pragma once
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/IAssetImporter.hpp>

namespace fbzz::asset {

class FzAnimImporter final : public IAssetImporter<AnimationClip> {
public:
    [[nodiscard]] std::unique_ptr<AnimationClip> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".anim" };
        return kExts;
    }
};

} // namespace fbzz::asset
