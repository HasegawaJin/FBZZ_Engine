/// @file    SurfacePatchMorph.hpp
/// @brief   連続面の center-fan 格子を階層的に親レベルへ移す純粋な幾何 helper。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/Geometry/SurfacePatchLod.hpp>
#include <cstdint>
#include <span>

namespace fbzz::renderer {

inline constexpr uint8_t SURFACE_MORPH_AXIS_X = 1u << 0;
inline constexpr uint8_t SURFACE_MORPH_AXIS_Z = 1u << 1;
inline constexpr uint8_t SURFACE_MORPH_AXIS_BOTH = SURFACE_MORPH_AXIS_X | SURFACE_MORPH_AXIS_Z;

/// @brief center-fan の三角形内を線形補間し、親格子上の連続座標を返す。
/// @param[in] gridCoordinate 親へ移す現在レベルの絶対格子座標。
/// @param[in] axisMask 位置を移す軸。far strip は接線軸だけを指定する。
/// @return 無効な座標または軸なら false。out は未変更。
/// @note 軸両方では子 center-fan の各三角形が親 center-fan と一致する。片軸では far strip の接線だけを縮約する。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 shared grid footprints.
[[nodiscard]] FBZZ_GRAPHICS_API bool EvaluateSurfacePatchParentMap(math::Vector2 gridCoordinate, uint8_t axisMask,
    math::Vector2& out);

/// @brief 親パッチを採用できる密度で親へ 1 に達する連続モーフ係数を求める。
/// @param[in] worldPosition 元格子頂点の world 位置。
/// @param[in] parentCellSize 次の親レベルに対応する各軸の world セル幅。
/// @return 無効な metric なら false。outAlpha は未変更。
/// @note Near 解像度制約は nearDistance からその 2 倍の距離まで smoothstep で連続的に消える。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 view-dependent level selection.
[[nodiscard]] FBZZ_GRAPHICS_API bool ComputeSurfacePatchMorphAlpha(math::Vector3 worldPosition,
    math::Vector2 parentCellSize, const SurfacePatchLodSettings& settings,
    const SurfacePatchView& view, float& outAlpha);

/// @brief 非一様な親係数で center-fan の格子座標を細密レベルから順に合成する。
/// @return 無効な座標、係数、軸、または階層数なら false。out は未変更。
[[nodiscard]] FBZZ_GRAPHICS_API bool MorphSurfacePatchGridCoordinate(math::Vector2 gridCoordinate,
    std::span<const float> alpha, uint8_t axisMask, math::Vector2& out);

/// @brief 各階層の fan 頂点で密度を評価し、root UV と共有 band を保って座標を合成する。
/// @param[in] gridCoordinate 変形前の共有格子座標。格子の 1 セルが 1 単位。
/// @param[in] rootUv 変形前の頂点を基準矩形全体に対して表した UV。
/// @param[in] rootUvPerGridX 格子 X が 1 増えたときの基準矩形 UV の差分。
/// @param[in] rootUvPerGridZ 格子 Z が 1 増えたときの基準矩形 UV の差分。
/// @param[in] originalWorldPosition 変形前のワールド座標 [m]。
/// @param[in] worldRootUAxis 基準矩形の U が 1 増えたときのワールド座標の差分 [m]。
/// @param[in] worldRootVAxis 基準矩形の V が 1 増えたときのワールド座標の差分 [m]。
/// @param[in] nativeCellSize パッチ固有の各軸のセル幅 [m]。
/// @param[in] sharedBand 共有境界で使う各軸のセル幅の下限 [m]。
/// @param[in] levels 細かい格子から評価する親レベルの数。
/// @param[in] settings 親格子の採用を決める投影密度とモーフの設定。
/// @param[in] view モーフを評価するカメラと投影条件。
/// @param[in] axisMask 移動を許す軸のビット集合。0 の場合は頂点位置を固定する。
/// @param[out] outGridCoordinate 変形後の共有格子座標。
/// @param[out] outRootUv 変形後の基準矩形 UV。
/// @param[out] outCellSize 帯域制限に使う変形後の各軸のセル幅 [m]。
/// @return 無効な入力なら false。outGridCoordinate / outRootUv / outCellSize は未変更。
/// @note 密度は各 fan 三角形の 3 原頂点で評価し、変形後の頂点を三角形内で線形延長する。
/// @note far strip は接線軸だけを level scale し、外向き grid 座標を固定する。
/// @see Docs/design/surface-patch-lod.md Center-fan parent map and hierarchical morph field.
[[nodiscard]] FBZZ_GRAPHICS_API bool MorphSurfacePatchGridCoordinateDensity(math::Vector2 gridCoordinate,
    math::Vector2 rootUv, math::Vector2 rootUvPerGridX, math::Vector2 rootUvPerGridZ,
    math::Vector3 originalWorldPosition, math::Vector3 worldRootUAxis,
    math::Vector3 worldRootVAxis, math::Vector2 nativeCellSize,
    math::Vector2 sharedBand, uint32_t levels,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view,
    uint8_t axisMask, math::Vector2& outGridCoordinate, math::Vector2& outRootUv,
    math::Vector2& outCellSize);

/// @brief 変形後の幾何セル幅と既存の共有境界 band の最大を返す。
/// @param[in] nativeCellSize 現在 leaf の幾何セル幅。
/// @param[in] sharedBand 既存の patch/edge/corner 共有セル幅。
/// @return 無効な幅または係数なら false。out は未変更。
/// @note sharedBand が既に表す親レベルまでを飛ばし、その上の同じ密度場だけを合成する。
[[nodiscard]] FBZZ_GRAPHICS_API bool ResolveSurfacePatchMorphCellSize(math::Vector2 nativeCellSize,
    math::Vector2 sharedBand, std::span<const float> alpha,
    math::Vector2& out);

}
