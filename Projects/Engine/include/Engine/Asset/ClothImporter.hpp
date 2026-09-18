/// @file    ClothImporter.hpp
/// @brief   .cloth を共有 CPU アセットとして読み込む。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Asset/ClothAsset.hpp>
#include <Engine/Asset/IAssetImporter.hpp>

namespace fbzz::asset {
class ClothImporter final : public IAssetImporter<ClothAsset> {
public:
    std::unique_ptr<ClothAsset> Import(const std::string& path, renderer::ResourceManager*) override
    {
        auto result = std::make_unique<ClothAsset>();
        if (!LoadClothAssetFromFile(path, *result)) return nullptr;
        return result;
    }
    std::span<const std::string_view> SupportedExtensions() const override
    {
        static constexpr std::string_view extensions[]{".cloth"};
        return extensions;
    }
};
}
