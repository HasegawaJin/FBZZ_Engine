/// @file    RayDebugPass.cpp
/// @brief   Inline RayQuery の typed 入力と LDR 診断合成を記録する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#include <Graphics/Passes/RayTracing/RayDebugPass.hpp>
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>

namespace fbzz::renderer {
bool MakeRayDebugCameraConstants(const Camera& camera, RayDebugConstants& output)
{
    if (!std::isfinite(camera.m_near) || !std::isfinite(camera.m_far)
        || camera.m_near <= 0 || camera.m_far <= camera.m_near
        || !std::isfinite(camera.m_aspect) || camera.m_aspect <= 0
        || !std::isfinite(camera.m_fovY) || camera.m_fovY <= 0 || camera.m_fovY >= 180
        || !std::isfinite(camera.m_orthoHeight) || camera.m_orthoHeight <= 0) return false;
    auto forward = camera.GetForward();
    auto up = camera.GetUp();
    const auto finite = [](const math::Vector3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
            && std::isfinite(value.LengthSq());
    };
    if (!finite(forward) || !finite(up) || !finite(camera.m_position)
        || forward.LengthSq() < 1e-6f || up.LengthSq() < 1e-6f) return false;
    forward = forward.Normalized();
    auto right = math::Vector3::Cross(up, forward);
    if (right.LengthSq() < 1e-6f) return false;
    right = right.Normalized();
    up = math::Vector3::Cross(forward, right);
    const float halfHeight = camera.m_projection == ProjectionMode::Orthographic
        ? camera.m_orthoHeight * 0.5f : std::tan(math::ToRad(camera.m_fovY) * 0.5f);
    const float halfWidth = halfHeight * camera.m_aspect;
    if (!std::isfinite(halfWidth) || halfWidth <= 0 || !std::isfinite(halfHeight)
        || halfWidth > 1e15f || camera.m_far / forward.Length() > 1e30f) return false;
    output.cameraPosition = {camera.m_position.x, camera.m_position.y, camera.m_position.z, 1};
    output.cameraRight = {right.x, right.y, right.z, halfWidth};
    output.cameraUp = {up.x, up.y, up.z, halfHeight};
    output.cameraForward = {forward.x, forward.y, forward.z, 0};
    output.orthographic = camera.m_projection == ProjectionMode::Orthographic;
    output.nearDistance = camera.m_near;
    output.farDistance = camera.m_far;
    return true;
}

void RayDebugPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    using Purpose = RenderGraph::ResourceAccessPurpose;
    builder.Write("RayDebugResult", Purpose::UAV);
    if (m_scene.instanceCount) {
        builder.Read("RaySceneTLAS", Purpose::TRACE_READ);
        builder.Read("RayHitRecords", Purpose::SHADER_READ);
        for (size_t i = 0; i < m_scene.readBuffers.size(); ++i)
            builder.Read("RayReadBuffer" + std::to_string(i), Purpose::SHADER_READ);
    }
}

void RayDebugPass::Execute(PassResources& resources, RenderPassContext& context)
{
    RayDebugConstants constants{};
    if (!MakeRayDebugCameraConstants(context.camera, constants)) return;
    constants.width = context.width;
    constants.height = context.height;
    constants.mode = context.settings.viewMode == ViewMode::RayGeometricNormal ? 1u
        : context.settings.viewMode == ViewMode::RayInstanceId ? 2u : 0u;
    constants.instanceCount = m_scene.instanceCount;
    constants.incomplete = m_incomplete;
    context.resources.Update(m_constants, &constants, sizeof(constants));
    ComputeCall call;
    call.shader = m_shader;
    call.constantBuffers[0] = m_constants;
    call.uavOutputs[0] = resources.Texture("RayDebugResult");
    call.dispatchX = (context.width + 7) / 8;
    call.dispatchY = (context.height + 7) / 8;
    if (m_scene.instanceCount) {
        call.accelerationStructures[0] = resources.AccelerationStructure("RaySceneTLAS");
        call.srvBuffers[1] = resources.StructuredBuffer("RayHitRecords");
        for (size_t i = 0; i < m_scene.readBuffers.size(); ++i)
            call.indirectReadBuffers.push_back(resources.Buffer("RayReadBuffer" + std::to_string(i)));
    }
    context.renderer.Dispatch(call, context.resources);
}

void RayDebugCopyPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("RayDebugResult", RenderGraph::ResourceAccessPurpose::SHADER_READ);
    builder.ReadWrite(m_outputName);
}
void RayDebugCopyPass::Execute(PassResources& resources, RenderPassContext& context)
{
    context.renderer.SetRenderTarget(resources.Target(m_outputName), context.resources);
    context.renderer.SetViewport(0, 0, static_cast<float>(context.width), static_cast<float>(context.height));
    DrawCall draw;
    draw.shader = m_shader;
    draw.pipelineState = m_state;
    draw.textures[5] = resources.Texture("RayDebugResult");
    draw.vertexCount = 3;
    context.renderer.Submit(draw, context.resources);
}
} /// @note namespace fbzz::renderer
