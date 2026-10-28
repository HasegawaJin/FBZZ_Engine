/// @file    WaterSplineMesh.hpp
/// @brief   共有スプライン三角形の内部を連続した格子へ細分化する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
namespace fbzz::scene {
struct WaterSplineSnapshot;
/// @pre snapshotは検証済みの頂点と三角形を持ち、BVH構築前であること。
/// @note world mの目標辺長。共有辺の中点と線形属性を共有し、元の高さ・深さ・流速の面を維持する。
/// @note 予算上限では最後の完全な段階を保持し、実際の最大辺長とmeshQualityLimitedを返す。
void RefineWaterSplineMesh(WaterSplineSnapshot& snapshot, float maximumWorldCellSize);
} /// @note namespace fbzz::scene
