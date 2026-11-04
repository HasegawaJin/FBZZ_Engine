/// @file    FzTerrainSerializer.hpp
/// @brief   .terrain バイナリの読み書き (`IAssetSerializer<TerrainAsset>` 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/IAssetSerializer.hpp>
#include <Engine/Asset/TerrainAsset.hpp>

namespace fbzz::asset {

class FzTerrainSerializer final : public IAssetSerializer<TerrainAsset> {
public:
    [[nodiscard]] FBZZ_ENGINE_API bool Save(const TerrainAsset& asset, const std::string& absPath) const override;
    [[nodiscard]] FBZZ_ENGINE_API bool Load(const std::string& absPath, TerrainAsset& outAsset) const override;
    std::string_view Extension() const override { return ".terrain"; }
};

} /// @note namespace fbzz::asset
