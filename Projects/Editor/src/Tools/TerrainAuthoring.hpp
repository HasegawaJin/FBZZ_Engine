/// @file    TerrainAuthoring.hpp
/// @brief   高さ画像の部分貼り付けと標高・斜面による地形の塗り分け。
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

enum class TerrainHeightStampBlend { ADD, REPLACE, MAXIMUM, MINIMUM };

struct TerrainHeightStampSettings {
    math::Vector3 centerLocal;
    float width = 20.0f;
    float depth = 20.0f;
    float rotationDegrees = 0.0f;
    float heightScale = 10.0f;
    float heightOffset = 0.0f;
    float strength = 1.0f;
    float edgeFade = 0.1f;
    TerrainHeightStampBlend blend = TerrainHeightStampBlend::ADD;
};

/// @return 地形寸法・設定が不正なら false。画像の読み込み前と範囲プレビューでも共用する。
bool IsValidTerrainHeightStamp(const scene::TerrainComponent& terrain, const TerrainHeightStampSettings& settings);

/// @return 不正な設定・配列・非有限値では false、地形と実行時世代は未変更。
/// @note 寸法・高さは地形ローカルの m、回転は +Y 軸の度数。画像上端を -Z、右端を +X に写す。
/// @note image.heightData は線形の正規化値。双線形補間し、矩形の外は保持、内側は strength と edgeFade [0,0.5] で合成する。
/// @note ADD は画像高さを加算、他は heightOffset + image * heightScale を目標高にする。結果は地形の [-maxHeight,+maxHeight] 内へ収める。
/// @see https://speakerdeck.com/cygames/dainamitukunabian-geng-woke-neng-nisurucyllista-game-enginefalseopunwarudoxiang-kepurosiziyarubei-jing-zhi-zuo-turutomiao-hua-ji-neng Cygames Tech Conference 2021、スライド21の画像ブラシ。
/// @see Docs/design/terrain-authoring.md
bool ApplyTerrainHeightStamp(scene::TerrainComponent& terrain, const scene::TerrainComponent& image,
    const TerrainHeightStampSettings& settings);

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
