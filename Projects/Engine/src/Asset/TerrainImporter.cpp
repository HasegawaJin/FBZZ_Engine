/// @file    TerrainImporter.cpp
/// @brief   .terrain バイナリ → TerrainAsset デシリアライザ。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Engine/Asset/FzTerrainSerializer.hpp>
#include <Engine/Asset/TerrainImporter.hpp>

namespace fbzz::asset {

/// @note 読み取りと版の移行 (v1 → v2) は FzTerrainSerializer::Load の 1 か所だけが持つ。
///       以前は同じ解析を 2 か所に写していて、版を上げるたびに片方だけ直す危険があった。
std::unique_ptr<TerrainAsset> TerrainImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* /*resources*/)
{
    auto terrain = std::make_unique<TerrainAsset>();
    if (!FzTerrainSerializer{}.Load(absPath, *terrain))
        return nullptr;
    return terrain;
}

} // namespace fbzz::asset
