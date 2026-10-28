/// @file    TerrainHeightHelpers.cpp
/// @brief   Terrain の整数格子から高さを一度だけ累積する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "TerrainHeightHelpers.hpp"
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <cstddef>

namespace fbzz::scene::terrain_height_detail {

bool TryAddTerrainHeightSample(const TerrainComponent* terrain, int x, int z,
                                float& sum, int& count)
{
    if (!terrain || terrain->heightData.empty())
        return false;
    if (x < 0 || x >= terrain->columns || z < 0 || z >= terrain->rows)
        return false;
    const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(terrain->columns)
                     + static_cast<size_t>(x);
    sum += terrain->heightData[idx] * terrain->maxHeight;
    ++count;
    return true;
}

}
