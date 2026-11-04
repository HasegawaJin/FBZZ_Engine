/// @file    QueuedComputeComposite.hpp
/// @brief   Value-owned compute and conditional composite without a producer receipt wait.
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>

namespace fbzz::renderer {
class IRenderer;
class ResourceManager;
namespace compute_composite_detail {
/// @note Dispatch and draw remain one ordered native operation; a rejected dispatch leaves the target untouched.
/// @pre ResourceManager has the same completion lifetime as Submit. No producer/native success receipt is returned.
FBZZ_GRAPHICS_API void Submit(IRenderer& renderer, ResourceManager& resources,
    const ComputeCall& compute, ResourceHandle<RenderTargetTag> target, const DrawCall& composite);
}
}
