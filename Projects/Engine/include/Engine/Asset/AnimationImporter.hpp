/// @file    AnimationImporter.hpp
/// @brief   .anim バイナリ → AnimationClip ローダー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/IAssetImporter.hpp>

namespace fbzz::asset {

class AnimationImporter final : public IAssetImporter<AnimationClip> {
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
