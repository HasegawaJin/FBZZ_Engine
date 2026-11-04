/// @file    SequenceImporter.hpp
/// @brief   .sequence (TOML) → SequenceAsset ローダー
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/SequenceAsset.hpp>

namespace fbzz::asset {

class SequenceImporter final : public IAssetImporter<SequenceAsset> {
public:
    [[nodiscard]] FBZZ_ENGINE_API std::unique_ptr<SequenceAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override
    {
        static constexpr std::string_view kExts[] = { ".sequence" };
        return kExts;
    }
};

} /// @note namespace fbzz::asset
