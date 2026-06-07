// FBZZ Engine
// TerrainWaterDefaults.cpp | fbzz::editor
// Terrain / Water の Editor 既定アセットパス定義
#include <Editor/Util/TerrainWaterDefaults.hpp>

namespace fbzz::editor {

namespace {

constexpr const char* kDefaultTerrainMaterialPath = "Assets/Materials/Terrain/Terrain.fzmat";
constexpr const char* kDefaultWaterMaterialPath   = "Assets/Materials/Water/Water.fzmat";

} // namespace

const char* DefaultTerrainMaterialPath()
{
    return kDefaultTerrainMaterialPath;
}

const char* DefaultWaterMaterialPath()
{
    return kDefaultWaterMaterialPath;
}

} // namespace fbzz::editor
