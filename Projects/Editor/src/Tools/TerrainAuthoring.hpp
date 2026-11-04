/// @file    TerrainAuthoring.hpp
/// @brief   標高・斜面から保存可能な地形レイヤー分布を作る。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Transform.hpp>

namespace fbzz::editor {

/// @note 保存後の参照と実行時世代を保持する。geometry=false は塗りだけを失効させ、形状・物理を再構築しない。
void ApplyTerrainAuthoringSnapshot(scene::TerrainComponent& terrain, const scene::TerrainComponent& snapshot, bool geometry);

/// @note 保存時に割り当てる terrainAssetPath と描画・物理の実行時状態を除き、編集内容の競合を判定する。
bool TerrainAuthoringValuesEqual(const scene::TerrainComponent& first, const scene::TerrainComponent& second);

struct TerrainBiomeSettings {
    int grassLayer = 0;
    int rockLayer = 1;
    int snowLayer = 2;
    float snowHeight = 420.0f;
    float snowFade = 60.0f;
    float rockSlope = 38.0f;
    float slopeFade = 10.0f;
};

/// @return 無効な寸法・非有限値・重複層・非可逆変換では false、地形は未変更。
/// @note 標高は world Y [m]、傾斜は world 上方向からの角度 [度]。高さ場の三層を置換し、穴と追加メッシュは保持する。
/// @note 雪は緩斜面へ積もり、急斜面では標高によらず岩が残る。結果は保存スプラットなので Fiber と共有できる。
/// @see Docs/design/terrain-authoring.md
bool BakeTerrainBiome(scene::TerrainComponent& terrain, const scene::Transform& transform,
    const TerrainBiomeSettings& settings);

} /// @note namespace fbzz::editor
