/// @file    GpuInstanceCulling.cpp
/// @brief   View-scoped GPU instance filtering with an indexed indirect fallback contract.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Renderer/GpuInstanceCulling.hpp>
#include "GpuCullDrawQueue.hpp"
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Math/Frustum.hpp>
#include <array>
#include <vector>
#include <cmath>
#include <limits>

namespace fbzz::renderer {
namespace {
struct CullView {
    std::array<math::Vector4, 6> planes{};
    uint32_t count = 0;
    std::array<uint32_t, 3> padding{};
};
static_assert(sizeof(CullView) == 112);
struct StaticCullInstance { math::Matrix4 world, worldInvTranspose; };
static_assert(sizeof(StaticCullInstance) == 128);
struct IndexedArguments {
    uint32_t indexCount = 0, instanceCount = 0, startIndex = 0;
    int32_t baseVertex = 0;
    uint32_t startInstance = 0;
};
static_assert(sizeof(IndexedArguments) == 20);
bool Finite(const GpuCullInstance& input)
{
    if (!std::isfinite(input.bounds.x) || !std::isfinite(input.bounds.y)
        || !std::isfinite(input.bounds.z) || !std::isfinite(input.bounds.w) || input.bounds.w < 0) return false;
    for (const auto& row : input.world.m) for (float v : row) if (!std::isfinite(v)) return false;
    for (const auto& row : input.worldInvTranspose.m) for (float v : row) if (!std::isfinite(v)) return false;
    return true;
}
} /// @note namespace
struct GpuInstanceCulling::State {
    uint64_t owner = 0, version = 0;
    ResourceHandle<ShaderTag> shader, deformedShader, queuedShader;
    DynamicStructuredBufferPool deformedInputs{64}, deformedBounds{64}, queuedInputs{64};
    DynamicRWStructuredBufferPool deformedOutputs{64};
    DynamicStructuredBufferPool inputs{64}, views{1};
    DynamicRWStructuredBufferPool outputs{64}, arguments{1};
};
GpuInstanceCulling::GpuInstanceCulling() : m_state(std::make_unique<State>()) {}
GpuInstanceCulling::~GpuInstanceCulling() = default;
bool GpuInstanceCulling::Prepare(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
    std::span<const GpuCullInstance> instances, DrawCall& call)
{
    if (!renderer.SupportsIndirectDraw() || instances.empty() || instances.size() > 65535u * 64u
        || call.indirectArguments.IsValid() || call.meshGroupCountX != 0 || call.indexCount == 0
        || call.indirectArgumentOffset != 0 || call.indirectCommandCount != 1 || call.indirectInstanceOffset
        || call.topology != PrimitiveTopology::TRIANGLE_LIST || call.indexCount % 3u != 0
        || call.baseVertex > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) return false;
    const auto* indices = resources.Get(call.indexBuffer);
    if (!indices || static_cast<uint64_t>(call.startIndex) + call.indexCount > indices->GetSize() / sizeof(uint32_t)) return false;
    for (const auto& instance : instances) if (!Finite(instance)) return false;
    CullView view;
    view.count = static_cast<uint32_t>(instances.size());
    for (int i = 0; i < math::Frustum::PLANE_COUNT; ++i) {
        const auto& plane = frustum.GetPlane(i);
        const float lengthSquared = plane.normal.LengthSq();
        if (!std::isfinite(lengthSquared) || std::abs(lengthSquared - 1.0f) > 0.01f
            || !std::isfinite(plane.distance)) return false;
        view.planes[i] = {plane.normal.x, plane.normal.y, plane.normal.z, plane.distance};
    }
    if (m_state->owner != resources.GetInstanceId() || m_state->version != resources.GetResetVersion()) {
        m_state = std::make_unique<State>();
        m_state->owner = resources.GetInstanceId(); m_state->version = resources.GetResetVersion();
    }
    auto& state = *m_state;
    if (!resources.Get(state.shader)) state.shader = resources.LoadShader("Assets/Shaders/Pipeline/GpuInstanceCull.cs.hlsl");
    if (!resources.Get(state.shader)) return false;
    const auto input = state.inputs.Acquire(resources, instances.size(), sizeof(GpuCullInstance));
    const auto viewInput = state.views.Acquire(resources, 1, sizeof(CullView));
    const auto output = state.outputs.Acquire(resources, instances.size(), 128);
    const auto arguments = state.arguments.Acquire(resources, 1, sizeof(IndexedArguments));
    if (!resources.Get(input) || !resources.Get(viewInput) || !resources.Get(output) || !resources.Get(arguments)) return false;
    if (resources.Get(output)->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(output)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(input)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(viewInput)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(arguments)->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return false;
    resources.Update(input, instances.data(), instances.size_bytes());
    resources.Update(viewInput, &view, sizeof(view));
    const IndexedArguments initial{call.indexCount, 0, call.startIndex, static_cast<int32_t>(call.baseVertex), 0};
    resources.Update(arguments, &initial, sizeof(initial));
    ComputeCall compute;
    compute.shader = state.shader;
    compute.srvBuffers[14] = input; compute.srvBuffers[15] = viewInput;
    compute.uavBuffers[0] = output; compute.uavBuffers[1] = arguments;
    compute.dispatchX = (view.count + 63) / 64;
    compute.dispatchY = 1; compute.dispatchZ = 1;
    if (!renderer.TryDispatch(compute, resources)) return false;
    call.instanceBuffer = output;
    call.instanceCount = view.count;
    call.indirectArguments = arguments;
    return true;
}
bool GpuInstanceCulling::SubmitDeformed(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
    std::span<const GpuDeformedInstance> instances, std::span<const math::Vector4> bounds, DrawCall& call)
{
    if (!renderer.SupportsIndirectDraw() || instances.empty() || instances.size() > 4096
        || instances.size() != bounds.size() || call.indirectArguments.IsValid() || call.preparedSkinning
        || call.meshGroupCountX || !call.indexCount || call.indexCount % 3
        || call.indirectArgumentOffset || call.indirectCommandCount != 1 || call.indirectInstanceOffset
        || call.topology != PrimitiveTopology::TRIANGLE_LIST
        || call.baseVertex > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) return false;
    if (!call.indexBuffer.IsValid()) return false;
    for (size_t i = 0; i < instances.size(); ++i) {
        if (!Finite({instances[i].world, instances[i].worldInvTranspose, bounds[i]})
            || instances[i].vertexIndex == INVALID_BINDLESS_INDEX) return false;
    }
    CullView view;
    view.count = static_cast<uint32_t>(instances.size());
    for (int i = 0; i < math::Frustum::PLANE_COUNT; ++i) {
        const auto plane = frustum.GetPlane(i);
        const float length = plane.normal.LengthSq();
        if (!std::isfinite(length) || std::abs(length-1.0f)>0.01f || !std::isfinite(plane.distance)) return false;
        view.planes[i] = {plane.normal.x, plane.normal.y, plane.normal.z, plane.distance};
    }
    if (m_state->owner != resources.GetInstanceId() || m_state->version != resources.GetResetVersion()) {
        m_state = std::make_unique<State>();
        m_state->owner = resources.GetInstanceId(); m_state->version = resources.GetResetVersion();
    }
    auto& state = *m_state;
    if (!resources.Get(state.deformedShader))
        state.deformedShader = resources.LoadShader("Assets/Shaders/Pipeline/GpuDeformedInstanceCull.cs.hlsl");
    if (!resources.Get(state.deformedShader)) return false;
    const auto input = state.deformedInputs.Acquire(resources, instances.size(), sizeof(GpuDeformedInstance));
    const auto spheres = state.deformedBounds.Acquire(resources, bounds.size(), sizeof(math::Vector4));
    const auto viewInput = state.views.Acquire(resources, 1, sizeof(CullView));
    const auto output = state.deformedOutputs.Acquire(resources, instances.size(), sizeof(GpuDeformedInstance));
    const auto arguments = state.arguments.Acquire(resources, 1, sizeof(IndexedArguments));
    if (!resources.Get(input) || !resources.Get(spheres) || !resources.Get(viewInput)
        || !resources.Get(output) || !resources.Get(arguments)) return false;
    if (resources.Get(input)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(spheres)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(viewInput)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(output)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(output)->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(arguments)->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return false;
    resources.Update(input, instances.data(), instances.size_bytes());
    resources.Update(spheres, bounds.data(), bounds.size_bytes());
    resources.Update(viewInput, &view, sizeof(view));
    const IndexedArguments initial{call.indexCount,0,call.startIndex,static_cast<int32_t>(call.baseVertex),0};
    resources.Update(arguments, &initial, sizeof(initial));
    ComputeCall compute;
    compute.shader = state.deformedShader;
    compute.srvBuffers[13] = spheres; compute.srvBuffers[14] = input; compute.srvBuffers[15] = viewInput;
    compute.uavBuffers[0] = output; compute.uavBuffers[1] = arguments;
    compute.dispatchX = compute.dispatchY = compute.dispatchZ = 1;
    DrawCall fallback = call;
    fallback.instanceBuffer = input; fallback.instanceCount = view.count;
    DrawCall indirect = fallback;
    indirect.instanceBuffer = output; indirect.indirectArguments = arguments;
    gpu_cull_detail::SubmitCullDraw(renderer, resources, compute, indirect, fallback);
    call = std::move(indirect);
    return true;
}

bool GpuInstanceCulling::SubmitStatic(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
    std::span<const GpuCullInstance> source, DrawCall& call)
{
    if (source.empty() || source.size() > 4096) return false;
    std::vector<StaticCullInstance> rows;
    std::vector<math::Vector4> spheresData;
    rows.reserve(source.size()); spheresData.reserve(source.size());
    for (const auto& row : source) { rows.push_back({row.world,row.worldInvTranspose}); spheresData.push_back(row.bounds); }
    const std::span<const StaticCullInstance> instances(rows);
    const std::span<const math::Vector4> bounds(spheresData);
    if (!renderer.SupportsIndirectDraw() || instances.empty() || instances.size() > 4096
        || instances.size() != bounds.size() || call.indirectArguments.IsValid() || call.preparedSkinning
        || call.meshGroupCountX || !call.indexCount || call.indexCount % 3
        || call.indirectArgumentOffset || call.indirectCommandCount != 1 || call.indirectInstanceOffset
        || call.topology != PrimitiveTopology::TRIANGLE_LIST
        || call.baseVertex > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) return false;
    if (!call.indexBuffer.IsValid()) return false;
    for (size_t i = 0; i < instances.size(); ++i) {
        if (!Finite({instances[i].world, instances[i].worldInvTranspose, bounds[i]})) return false;
    }
    CullView view;
    view.count = static_cast<uint32_t>(instances.size());
    for (int i = 0; i < math::Frustum::PLANE_COUNT; ++i) {
        const auto plane = frustum.GetPlane(i);
        const float length = plane.normal.LengthSq();
        if (!std::isfinite(length) || std::abs(length-1.0f)>0.01f || !std::isfinite(plane.distance)) return false;
        view.planes[i] = {plane.normal.x, plane.normal.y, plane.normal.z, plane.distance};
    }
    if (m_state->owner != resources.GetInstanceId() || m_state->version != resources.GetResetVersion()) {
        m_state = std::make_unique<State>();
        m_state->owner = resources.GetInstanceId(); m_state->version = resources.GetResetVersion();
    }
    auto& state = *m_state;
    if (!resources.Get(state.queuedShader))
        state.queuedShader = resources.LoadShader("Assets/Shaders/Pipeline/GpuQueuedInstanceCull.cs.hlsl");
    if (!resources.Get(state.queuedShader)) return false;
    const auto input = state.queuedInputs.Acquire(resources, instances.size(), sizeof(StaticCullInstance));
    const auto spheres = state.deformedBounds.Acquire(resources, bounds.size(), sizeof(math::Vector4));
    const auto viewInput = state.views.Acquire(resources, 1, sizeof(CullView));
    const auto output = state.outputs.Acquire(resources, instances.size(), sizeof(StaticCullInstance));
    const auto arguments = state.arguments.Acquire(resources, 1, sizeof(IndexedArguments));
    if (!resources.Get(input) || !resources.Get(spheres) || !resources.Get(viewInput)
        || !resources.Get(output) || !resources.Get(arguments)) return false;
    if (resources.Get(input)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(spheres)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(viewInput)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(output)->GetBindlessIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(output)->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX
        || resources.Get(arguments)->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX) return false;
    resources.Update(input, instances.data(), instances.size_bytes());
    resources.Update(spheres, bounds.data(), bounds.size_bytes());
    resources.Update(viewInput, &view, sizeof(view));
    const IndexedArguments initial{call.indexCount,0,call.startIndex,static_cast<int32_t>(call.baseVertex),0};
    resources.Update(arguments, &initial, sizeof(initial));
    ComputeCall compute;
    compute.shader = state.queuedShader;
    compute.srvBuffers[13] = spheres; compute.srvBuffers[14] = input; compute.srvBuffers[15] = viewInput;
    compute.uavBuffers[0] = output; compute.uavBuffers[1] = arguments;
    compute.dispatchX = compute.dispatchY = compute.dispatchZ = 1;
    DrawCall fallback = call;
    fallback.instanceBuffer = input; fallback.instanceCount = view.count;
    DrawCall indirect = fallback;
    indirect.instanceBuffer = output; indirect.indirectArguments = arguments;
    gpu_cull_detail::SubmitCullDraw(renderer, resources, compute, indirect, fallback);
    call = std::move(indirect);
    return true;
}

void GpuInstanceCulling::Release(ResourceManager& resources)
{
    if (m_state->owner != resources.GetInstanceId() || m_state->version != resources.GetResetVersion()) return;
    m_state->queuedInputs.ReleaseAll(resources);
    m_state->deformedInputs.ReleaseAll(resources); m_state->deformedBounds.ReleaseAll(resources);
    m_state->deformedOutputs.ReleaseAll(resources);
    m_state->inputs.ReleaseAll(resources); m_state->views.ReleaseAll(resources);
    m_state->outputs.ReleaseAll(resources); m_state->arguments.ReleaseAll(resources);
}
} /// @note namespace fbzz::renderer
