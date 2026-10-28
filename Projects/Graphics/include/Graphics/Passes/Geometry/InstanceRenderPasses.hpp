/// @file    InstanceRenderPasses.hpp
/// @brief   Persistent tree/detail sets participate in the existing color, shadow, selection and motion passes.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Pipeline/RenderPassContext.hpp>
namespace fbzz::renderer {
void ExecuteInstanceGBuffer(RenderPassContext& context);
void ExecuteInstanceForward(RenderPassContext& context);
void ExecuteInstanceSelection(RenderPassContext& context);
void ExecuteInstanceVelocity(RenderPassContext& context);
/// @note Normal-Z probe capture uses its own camera and shadow/cookie providers; the probe owner is excluded.
void ExecuteInstanceCapture(RenderPassContext& context,const Camera& captureCamera,uint32_t resolution,
    uint32_t excludedIndex,uint32_t excludedGeneration);
void SubmitInstanceShadows(RenderPassContext& context,const math::Frustum& frustum,
    ResourceHandle<ConstantBufferTag> frameConstants);
} /// @note namespace fbzz::renderer
