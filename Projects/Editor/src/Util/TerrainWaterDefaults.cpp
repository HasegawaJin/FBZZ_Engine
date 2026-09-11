/// @file    TerrainWaterDefaults.cpp
/// @brief   Terrain / Water の Editor 既定アセットパス定義。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include <Editor/Util/TerrainWaterDefaults.hpp>

namespace fbzz::editor {

namespace {

constexpr const char* kDefaultTerrainLayerMaterialPaths[4] = {
    "Assets/Materials/Terrain/Layer0_Ground.mat",
    "Assets/Materials/Terrain/Layer1_Grass.mat",
    "Assets/Materials/Terrain/Layer2_Sand.mat",
    "Assets/Materials/Terrain/Layer3_Rock.mat",
};

constexpr WaterMaterialPreset kWaterMaterialPresets[] = {
    { "Ocean", "Assets/Materials/Water/Ocean.mat",
      "外洋。大きなうねりと白波。環境風を強く受ける。" },
    { "Lake",  "Assets/Materials/Water/Lake.mat",
      "湖。穏やかな小波。環境風は弱めに受ける。" },
    { "River", "Assets/Materials/Water/River.mat",
      "川。Flow Direction へ流れ、浮いている物体を押し流す。" },
    { "Flat",  "Assets/Materials/Water/Flat.mat",
      "波のない静水 (池・プール)。風も水流も受けない。" },
};

} // namespace

const char* DefaultTerrainLayerMaterialPath(int layerIndex)
{
    if (layerIndex < 0 || layerIndex >= 4) return "";
    return kDefaultTerrainLayerMaterialPaths[layerIndex];
}

const char* DefaultWaterMaterialPath()
{
    return kWaterMaterialPresets[0].path;
}

std::span<const WaterMaterialPreset> WaterMaterialPresets()
{
    return kWaterMaterialPresets;
}

} // namespace fbzz::editor
