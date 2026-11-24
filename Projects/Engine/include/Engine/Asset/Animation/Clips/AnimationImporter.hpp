/// @file    AnimationImporter.hpp
/// @brief   .anim バイナリ → AnimationClip ローダー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Animation/Clips/AnimationClip.hpp>
#include <Engine/Asset/Import/IAssetImporter.hpp>

namespace fbzz::asset {

class AnimationImporter final : public IAssetImporter<AnimationClip> {
public:
    [[nodiscard]] FBZZ_ENGINE_API std::unique_ptr<AnimationClip> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".anim" };
        return kExts;
    }
};

} /// @note namespace fbzz::asset
