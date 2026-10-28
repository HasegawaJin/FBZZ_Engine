/// @file    TerrainHeightHelpers.hpp
/// @brief   描画と衝突が共有する整数格子の高さ加算。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

namespace fbzz::scene {
struct TerrainComponent;
}
namespace fbzz::scene::terrain_height_detail {

/// @pre heightData が空でなければ rows * columns の格子を保持していること。
/// @return 欠損・範囲外は false、sum/count は未変更。成功時は高さを加算し count を 1 増やす。
/// @note x/z は整数格子座標。クランプ・補間・隣接地形の探索は行わない。
bool TryAddTerrainHeightSample(const TerrainComponent* terrain, int x, int z,
                                float& sum, int& count);

}
