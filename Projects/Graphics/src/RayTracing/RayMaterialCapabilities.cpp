/// @file    RayMaterialCapabilities.cpp
/// @brief   Static canonical geometry and alpha candidate capability.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/RayTracing/RayMaterialCapabilities.hpp>
#include <cmath>

namespace fbzz::renderer {

RayMaterialCapabilities ResolveStaticRayMaterialCapabilities(
    bool standardGeometry, BlendMode blend, float alpha, float cutoff, bool alphaTexture)
{
    RayMaterialCapabilities result;
    result.staticGeometry = standardGeometry;
    if (!standardGeometry || blend != BlendMode::OPAQUE_BLEND || !std::isfinite(alpha) || !std::isfinite(cutoff))
        return result;
    result.opacity = alphaTexture ? RayOpacity::ALPHA_TEST
        : alpha >= cutoff ? RayOpacity::OPAQUE_SURFACE : RayOpacity::EMPTY;
    return result;
}

} /// @note namespace fbzz::renderer
