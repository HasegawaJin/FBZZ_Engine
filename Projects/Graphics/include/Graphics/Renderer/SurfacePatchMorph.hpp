/// @file    SurfacePatchMorph.hpp
/// @brief   連続面の center-fan 格子を階層的に親レベルへ移す純粋な幾何 helper。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once

#include <Graphics/Renderer/SurfacePatchLod.hpp>
#include <cstdint>
#include <span>

namespace fbzz::renderer {

inline constexpr uint8_t SURFACE_MORPH_AXIS_X = 1u << 0;
inline constexpr uint8_t SURFACE_MORPH_AXIS_Z = 1u << 1;
inline constexpr uint8_t SURFACE_MORPH_AXIS_BOTH = SURFACE_MORPH_AXIS_X | SURFACE_MORPH_AXIS_Z;

/// @brief center-fan の三角形内を線形補間し、親格子上の連続座標を返す。
/// @param gridCoordinate 親へ移す現在レベルの絶対格子座標。
/// @param axisMask 位置を移す軸。far strip は接線軸だけを指定する。
/// @return 無効な座標または軸なら false。out は未変更。
/// @note 軸両方では子 center-fan の各三角形が親 center-fan と一致する。片軸では far strip の接線だけを縮約する。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 shared grid footprints.
[[nodiscard]] bool EvaluateSurfacePatchParentMap(math::Vector2 gridCoordinate, uint8_t axisMask,
    math::Vector2& out);

/// @brief 親パッチを採用できる密度で親へ 1 に達する連続モーフ係数を求める。
/// @param worldPosition 元格子頂点の world 位置。
/// @param parentCellSize 次の親レベルに対応する各軸の world セル幅。
/// @return 無効な metric なら false。outAlpha は未変更。
/// @note Near 解像度制約は nearDistance からその 2 倍の距離まで smoothstep で連続的に消える。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 view-dependent level selection.
[[nodiscard]] bool ComputeSurfacePatchMorphAlpha(math::Vector3 worldPosition,
    math::Vector2 parentCellSize, const SurfacePatchLodSettings& settings,
    const SurfacePatchView& view, float& outAlpha);

/// @brief 非一様な親係数で center-fan の格子座標を細密レベルから順に合成する。
/// @return 無効な座標、係数、軸、または階層数なら false。out は未変更。
[[nodiscard]] bool MorphSurfacePatchGridCoordinate(math::Vector2 gridCoordinate,
    std::span<const float> alpha, uint8_t axisMask, math::Vector2& out);

/// @brief 各階層の fan 頂点で密度を評価し、root UV と共有 band を保って座標を合成する。
/// @note rootUvとoriginalWorldPositionは変形前。rootUvPerGridX/ZとworldRootU/VAxisは各座標単位の差分。
/// @note nativeCellSizeはpatch固有の幅、sharedBandは共有境界の下限。axisMask=0は位置を固定する。
/// @return 無効な入力なら false。outGridCoordinate / outRootUv / outCellSize は未変更。
/// @note 密度は各 fan 三角形の 3 原頂点で評価し、変形後の頂点を三角形内で線形延長する。
/// @note far strip は接線軸だけを level scale し、外向き grid 座標を固定する。
/// @see Docs/design/surface-patch-lod.md Center-fan parent map and hierarchical morph field.
[[nodiscard]] bool MorphSurfacePatchGridCoordinateDensity(math::Vector2 gridCoordinate,
    math::Vector2 rootUv, math::Vector2 rootUvPerGridX, math::Vector2 rootUvPerGridZ,
    math::Vector3 originalWorldPosition, math::Vector3 worldRootUAxis,
    math::Vector3 worldRootVAxis, math::Vector2 nativeCellSize,
    math::Vector2 sharedBand, uint32_t levels,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view,
    uint8_t axisMask, math::Vector2& outGridCoordinate, math::Vector2& outRootUv,
    math::Vector2& outCellSize);

/// @brief 変形後の幾何セル幅と既存の共有境界 band の最大を返す。
/// @param nativeCellSize 現在 leaf の幾何セル幅。
/// @param sharedBand 既存の patch/edge/corner 共有セル幅。
/// @return 無効な幅または係数なら false。out は未変更。
/// @note sharedBand が既に表す親レベルまでを飛ばし、その上の同じ密度場だけを合成する。
[[nodiscard]] bool ResolveSurfacePatchMorphCellSize(math::Vector2 nativeCellSize,
    math::Vector2 sharedBand, std::span<const float> alpha,
    math::Vector2& out);

}
