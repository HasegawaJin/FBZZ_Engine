/// @file    TerrainPaintMix.hpp
/// @brief   地形ペイントの素材配合とブラシ条件を保存・再利用する。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include "TerrainScatter.hpp"

namespace fbzz::editor {
struct TerrainPaintMixSource {
    std::string reference;
    float weight = 100.0f;
};
struct TerrainPaintMix {
    std::string name;
    std::vector<TerrainPaintMixSource> sources;
    TerrainScatterSettings settings;
    float radius = 5.0f;
    bool avoidWater = true;
    bool avoidPaths = true;
    int excludedLayer = -1;
    std::vector<std::string> excludedMaterials;
    float feather = 0.0f;
};
/// @return 不正な条件・未知形式・書込失敗は false。読込失敗時は out を変更しない。
[[nodiscard]] bool SaveTerrainPaintMix(const std::string& path, const TerrainPaintMix& mix);
[[nodiscard]] bool LoadTerrainPaintMix(const std::string& path, TerrainPaintMix& out);
void RegisterTerrainPaintMixOperators(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
