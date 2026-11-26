/// @file    SurfaceFarMesh.hpp
/// @brief   有限面の境界と同じ頂点列で接続する低密度の外周メッシュ。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/Geometry/SurfacePatchLod.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::renderer {

enum class SurfaceFarRegion : uint8_t {
    MIN_X,
    MAX_X,
    MIN_Z,
    MAX_Z,
    MIN_X_MIN_Z,
    MAX_X_MIN_Z,
    MIN_X_MAX_Z,
    MAX_X_MAX_Z,
};

struct SurfaceFarPatch {
    SurfaceFarRegion region = SurfaceFarRegion::MIN_X; ///< 基準矩形に接する辺または角。geometry の depth/x/z と組み合わせて識別する。
    SurfacePatch geometry; ///< 外周パッチの幾何。depth/x/z は辺なら接続元の葉、角なら 0。
};

/// @brief inner の外側から outer までを、境界 leaf と同じ分割数の strip と corner で埋める。
/// @param[in] inner 内側の基準矩形、変位の範囲とワールド変換。
/// @param[in] innerLeaves SurfacePatchView::includeCulledPatches で得た全境界を含む leaf 配列。
/// @param[in] outerMin 内側と同じローカル座標系での外周 XZ の最小点。
/// @param[in] outerMax 内側と同じローカル座標系での外周 XZ の最大点。
/// @param[in] cellsPerPatch 内側のパッチの各辺と同じセル数。
/// @param[out] out 接続用の辺と角のパッチを格納する配列。
/// @param[in] morphEnabled モーフの移動範囲を外周パッチの境界に含めるかどうか。
/// @return 無効な境界・外周・導出幾何なら false。out は未変更。拡張不要なら空配列で成功。
/// @note cellsPerPatch は 2〜256 の二の冪。外周パッチの縫合マスクは常に 0。
/// @note 物理領域は拡張しない。uv は inner の基準矩形に対する値で [0,1] の外にも出る。
/// @note 内側の辺と角は inner と同じセル幅、その他の共有頂点は全 far patch の共通セル幅で評価する。
/// @note 各 strip の外向き分割数は共通。隣接 strip の幅が違っても、共有辺の頂点列は一致する。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 shared grid footprints.
[[nodiscard]] FBZZ_GRAPHICS_API bool BuildSurfaceFarMesh(const SurfacePatchSurface& inner,
    std::span<const SurfacePatch> innerLeaves, math::Vector2 outerMin, math::Vector2 outerMax,
    uint32_t cellsPerPatch, std::vector<SurfaceFarPatch>& out, bool morphEnabled = false);

} // namespace fbzz::renderer
