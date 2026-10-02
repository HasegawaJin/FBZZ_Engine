/// @file    RayMaterialCapabilities.hpp
/// @brief   Raster の振り分けと独立した形状・被覆の追跡対応能力。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Renderer/RenderState.hpp>
#include <cstdint>

namespace fbzz::renderer {

enum class RayOpacity : uint8_t {
    UNSUPPORTED,
    OPAQUE_SURFACE,
    ALPHA_TEST,
    EMPTY,
};

/// @note 未知の頂点変形や clip を標準 PBR として扱わない。surface / BSDF の対応は別契約。
struct RayMaterialCapabilities {
    bool staticGeometry = false;
    RayOpacity opacity = RayOpacity::UNSUPPORTED;
};

/// @note ALPHA_TEST requires nonopaque BLAS geometry and the same UV/LOD/cutoff at every RayQuery candidate, including shadows.
/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#rayquery Committing nonopaque triangle candidates.
[[nodiscard]] RayMaterialCapabilities ResolveStaticRayMaterialCapabilities(
    bool standardGeometry, BlendMode blend, float alpha, float cutoff, bool alphaTexture);

} /// @note namespace fbzz::renderer
