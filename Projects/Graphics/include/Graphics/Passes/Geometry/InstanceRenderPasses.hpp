/// @file    InstanceRenderPasses.hpp
/// @brief   Persistent tree/detail sets participate in the existing color, shadow, selection and motion passes.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
namespace fbzz::renderer {
FBZZ_GRAPHICS_API void ExecuteInstanceGBuffer(RenderPassContext& context);
FBZZ_GRAPHICS_API void ExecuteInstanceForward(RenderPassContext& context);
FBZZ_GRAPHICS_API void ExecuteInstanceSelection(RenderPassContext& context);
FBZZ_GRAPHICS_API void ExecuteInstanceVelocity(RenderPassContext& context);
/// @note Normal-Z probe capture uses its own camera and shadow/cookie providers; the probe owner is excluded.
FBZZ_GRAPHICS_API void ExecuteInstanceCapture(RenderPassContext& context,const Camera& captureCamera,uint32_t resolution,
    uint32_t excludedIndex,uint32_t excludedGeneration);
FBZZ_GRAPHICS_API void SubmitInstanceShadows(RenderPassContext& context,const math::Frustum& frustum,
    ResourceHandle<ConstantBufferTag> frameConstants);
} /// @note namespace fbzz::renderer
