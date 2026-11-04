/// @file    SurfaceFarMesh.hpp
/// @brief   有限面の境界と同じ頂点列で接続する低密度の外周メッシュ。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/SurfacePatchLod.hpp>
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
    /// @note key は region と geometry の depth/x/z。strip は接続元の leaf、corner は depth/x/z=0。
    SurfaceFarRegion region = SurfaceFarRegion::MIN_X;
    SurfacePatch geometry;
};

/// @brief inner の外側から outer までを、境界 leaf と同じ分割数の strip と corner で埋める。
/// @param innerLeaves SurfacePatchView::includeCulledPatches で得た全境界を含む leaf 配列。
/// @return 無効な境界・外周・導出幾何なら false。out は未変更。拡張不要なら空配列で成功。
/// @note cellsPerPatch は inner と同じ二の冪。stitch mask は0、morphEnabled はモーフの移動範囲を AABB へ含める。
/// @note 物理領域は拡張しない。uv は inner の基準矩形に対する値で [0,1] の外にも出る。
/// @note 内側の辺と角は inner と同じセル幅、その他の共有頂点は全 far patch の共通セル幅で評価する。
/// @note 各 strip の外向き分割数は共通。隣接 strip の幅が違っても、共有辺の頂点列は一致する。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 shared grid footprints.
[[nodiscard]] FBZZ_GRAPHICS_API bool BuildSurfaceFarMesh(const SurfacePatchSurface& inner,
    std::span<const SurfacePatch> innerLeaves, math::Vector2 outerMin, math::Vector2 outerMax,
    uint32_t cellsPerPatch, std::vector<SurfaceFarPatch>& out, bool morphEnabled = false);

} /// @note namespace fbzz::renderer
