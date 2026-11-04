/// @file    GpuCullDrawQueue.hpp
/// @brief   Ordered value-owned GPU cull/draw requests with a direct fallback.
/// @author  Hasegawa Jin
/// @date    2026-10-19
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
namespace fbzz::renderer {
class IRenderer;
class ResourceManager;
namespace gpu_cull_detail {
/// @note Requests own their handles; Manager has the same frame lifetime as ordinary Submit. No Scene is borrowed.
FBZZ_GRAPHICS_API void SubmitCullDraw(IRenderer& renderer, ResourceManager& resources,
    const ComputeCall& compute, const DrawCall& indirect, const DrawCall& fallback);
}
}
