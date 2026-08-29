/// @file    SequenceImporter.hpp
/// @brief   .sequence (TOML) → SequenceAsset ローダー
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/SequenceAsset.hpp>

namespace fbzz::asset {

class SequenceImporter final : public IAssetImporter<SequenceAsset> {
public:
    [[nodiscard]] std::unique_ptr<SequenceAsset> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override
    {
        static constexpr std::string_view kExts[] = { ".sequence" };
        return kExts;
    }
};

} // namespace fbzz::asset
