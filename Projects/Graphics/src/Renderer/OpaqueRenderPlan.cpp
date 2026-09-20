/// @file    OpaqueRenderPlan.cpp
/// @brief   不透明描画の実効経路を決定する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#include <Graphics/Renderer/RenderSettings.hpp>

namespace fbzz::renderer {

OpaqueRenderPlan ResolveOpaqueRenderPlan(
    const RenderSettings& settings, const OpaqueRenderAvailability& availability)
{
    if (settings.UsesGBuffer() && availability.depthNormal
        && availability.deferredLighting && availability.depthCopy)
        return { OpaqueRenderPath::DEFERRED };

    const bool needsScreenSpaceInputs = settings.postProcess.ambientOcclusion.enabled
        || settings.IsGtaoActive() || settings.contactShadow.enabled || settings.ssr.enabled;
    if (availability.depthNormal && needsScreenSpaceInputs && !settings.IsUnlit())
        return { OpaqueRenderPath::FORWARD_DEPTH_NORMAL };

    return { OpaqueRenderPath::FORWARD };
}

} /// @note namespace fbzz::renderer
