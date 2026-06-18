// FBZZ Engine
// FzTerrainSerializer.hpp | fbzz::asset
// .terrain バイナリの読み書き (IAssetSerializer<TerrainAsset> 実装)
#pragma once
#include <Engine/Asset/IAssetSerializer.hpp>
#include <Engine/Asset/TerrainAsset.hpp>

namespace fbzz::asset {

class FzTerrainSerializer final : public IAssetSerializer<TerrainAsset> {
public:
    [[nodiscard]] bool Save(const TerrainAsset& asset, const std::string& absPath) const override;
    [[nodiscard]] bool Load(const std::string& absPath, TerrainAsset& outAsset) const override;
    std::string_view Extension() const override { return ".terrain"; }
};

} // namespace fbzz::asset
