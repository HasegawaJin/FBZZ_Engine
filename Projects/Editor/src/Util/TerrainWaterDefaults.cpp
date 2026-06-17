// FBZZ Engine
// TerrainWaterDefaults.cpp | fbzz::editor
// Terrain / Water の Editor 既定アセットパス定義
#include <Editor/Util/TerrainWaterDefaults.hpp>

namespace fbzz::editor {

namespace {

constexpr const char* kDefaultTerrainLayerMaterialPaths[4] = {
    "Assets/Materials/Terrain/Layer0_Ground.mat",
    "Assets/Materials/Terrain/Layer1_Grass.mat",
    "Assets/Materials/Terrain/Layer2_Sand.mat",
    "Assets/Materials/Terrain/Layer3_Rock.mat",
};
constexpr const char* kDefaultWaterMaterialPath = "Assets/Materials/Water/Water.mat";

} // namespace

const char* DefaultTerrainLayerMaterialPath(int layerIndex)
{
    if (layerIndex < 0 || layerIndex >= 4) return "";
    return kDefaultTerrainLayerMaterialPaths[layerIndex];
}

const char* DefaultWaterMaterialPath()
{
    return kDefaultWaterMaterialPath;
}

} // namespace fbzz::editor
