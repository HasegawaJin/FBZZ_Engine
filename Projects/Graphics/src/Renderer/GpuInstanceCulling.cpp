/// @file    GpuInstanceCulling.cpp
/// @brief   View-scoped GPU instance filtering with an indexed indirect fallback contract.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Renderer/GpuInstanceCulling.hpp>
#include "GpuCullDrawQueue.hpp"
#include <Graphics/Pipeline/InstanceBatch.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Math/Frustum.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::renderer {
namespace {

constexpr uint32_t CULL_THREADS_PER_GROUP = 64;
constexpr uint32_t MAX_CULL_GROUPS = 65535;
constexpr size_t MAX_QUEUED_INSTANCES = 4096;
constexpr size_t BOUNDS_SRV_SLOT = 13;
constexpr size_t INSTANCES_SRV_SLOT = 14;
constexpr size_t VIEW_SRV_SLOT = 15;
constexpr size_t VISIBLE_INSTANCES_UAV_SLOT = 0;
constexpr size_t DRAW_ARGUMENTS_UAV_SLOT = 1;

struct CullView {
    std::array<math::Vector4, 6> planes{};
    uint32_t count = 0;
    std::array<uint32_t, 3> padding{};
};
static_assert(sizeof(CullView) == 112);

struct IndexedArguments {
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    uint32_t startIndex = 0;
    int32_t baseVertex = 0;
    uint32_t startInstance = 0;
};
static_assert(sizeof(IndexedArguments) == 20);

bool IsFiniteCullInstance(const GpuCullInstance& input)
{
    if (!std::isfinite(input.bounds.x) || !std::isfinite(input.bounds.y)
        || !std::isfinite(input.bounds.z) || !std::isfinite(input.bounds.w) || input.bounds.w < 0)
        return false;

    for (const auto* matrix : {&input.world, &input.worldInvTranspose})
        for (const auto& row : matrix->m)
            for (float component : row)
                if (!std::isfinite(component)) return false;
    return true;
}

bool BuildCullView(const math::Frustum& frustum, size_t instanceCount, CullView& view)
{
    view.count = static_cast<uint32_t>(instanceCount);
    for (int planeIndex = 0; planeIndex < math::Frustum::PLANE_COUNT; ++planeIndex) {
        const auto& plane = frustum.GetPlane(planeIndex);
        const float lengthSquared = plane.normal.LengthSq();
        if (!std::isfinite(lengthSquared) || std::abs(lengthSquared - 1.0f) > 0.01f
            || !std::isfinite(plane.distance)) {
            return false;
        }
        view.planes[planeIndex] = {plane.normal.x, plane.normal.y, plane.normal.z, plane.distance};
    }
    return true;
}

bool IsIndexedCullDraw(const DrawCall& call)
{
    return !call.indirectArguments.IsValid() && call.meshGroupCountX == 0
        && call.indexCount != 0 && call.indexCount % 3 == 0
        && call.indirectArgumentOffset == 0 && call.indirectCommandCount == 1 && !call.indirectInstanceOffset
        && call.topology == PrimitiveTopology::TRIANGLE_LIST
        && call.baseVertex <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
}

IndexedArguments MakeEmptyDrawArguments(const DrawCall& call)
{
    return {call.indexCount, 0, call.startIndex, static_cast<int32_t>(call.baseVertex), 0};
}

} /// @note namespace

struct GpuInstanceCulling::State {
    uint64_t owner = 0;
    uint64_t resetVersion = 0;
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<ShaderTag> deformedShader;
    ResourceHandle<ShaderTag> queuedShader;
    DynamicStructuredBufferPool deformedInputs{64};
    DynamicStructuredBufferPool queuedBounds{64};
    DynamicStructuredBufferPool queuedInputs{64};
    DynamicRWStructuredBufferPool deformedOutputs{64};
    DynamicStructuredBufferPool inputs{64};
    DynamicStructuredBufferPool views{1};
    DynamicRWStructuredBufferPool outputs{64};
    DynamicRWStructuredBufferPool arguments{1};

    [[nodiscard]] bool BelongsTo(const ResourceManager& resources) const
    {
        return owner == resources.GetInstanceId() && resetVersion == resources.GetResetVersion();
    }

    /// @note Prepare packs bounds with each instance. Queued draws upload a separate bounds buffer for either layout.
    template<class Instance>
    static bool Cull(std::unique_ptr<State>& storage, IRenderer& renderer, ResourceManager& resources,
        const math::Frustum& frustum, std::span<const Instance> instances,
        std::span<const math::Vector4> bounds, DrawCall& call)
    {
        CullView view;
        if (!BuildCullView(frustum, instances.size(), view)) return false;

        if (!storage->BelongsTo(resources)) {
            storage = std::make_unique<State>();
            storage->owner = resources.GetInstanceId();
            storage->resetVersion = resources.GetResetVersion();
        }
        auto& state = *storage;

        constexpr bool queued = !std::is_same_v<Instance, GpuCullInstance>;
        constexpr bool deformed = std::is_same_v<Instance, GpuDeformedInstance>;
        auto& cullShader = deformed ? state.deformedShader : queued ? state.queuedShader : state.shader;
        if (!resources.Get(cullShader)) {
            const char* shaderPath = "Assets/Shaders/Pipeline/GpuInstanceCull.cs.hlsl";
            if constexpr (deformed)
                shaderPath = "Assets/Shaders/Pipeline/GpuDeformedInstanceCull.cs.hlsl";
            else if constexpr (queued)
                shaderPath = "Assets/Shaders/Pipeline/GpuQueuedInstanceCull.cs.hlsl";
            cullShader = resources.LoadShader(shaderPath);
        }
        if (!resources.Get(cullShader)) return false;

        auto& inputPool = deformed ? state.deformedInputs : queued ? state.queuedInputs : state.inputs;
        auto& outputPool = deformed ? state.deformedOutputs : state.outputs;
        constexpr size_t outputStride = deformed ? sizeof(GpuDeformedInstance) : sizeof(PerInstanceData);
        const auto input = inputPool.Acquire(resources, instances.size(), sizeof(Instance));
        const auto spheres = queued ? state.queuedBounds.Acquire(resources, bounds.size(), sizeof(math::Vector4))
            : ResourceHandle<StructuredBufferTag>{};
        const auto viewInput = state.views.Acquire(resources, 1, sizeof(CullView));
        const auto output = outputPool.Acquire(resources, instances.size(), outputStride);
        const auto drawArguments = state.arguments.Acquire(resources, 1, sizeof(IndexedArguments));
        if (!resources.Get(input) || (queued && !resources.Get(spheres)) || !resources.Get(viewInput)
            || !resources.Get(output) || !resources.Get(drawArguments)) return false;

        const auto hasSrv = [&](auto handle) { return resources.Get(handle)->GetBindlessIndex() != INVALID_BINDLESS_INDEX; };
        const auto hasUav = [&](auto handle) { return resources.Get(handle)->GetBindlessUavIndex() != INVALID_BINDLESS_INDEX; };
        if constexpr (!queued)
            if (!hasUav(output) || !hasSrv(output)) return false;
        if (!hasSrv(input) || (queued && !hasSrv(spheres)) || !hasSrv(viewInput)) return false;
        if constexpr (queued)
            if (!hasSrv(output) || !hasUav(output)) return false;
        if (!hasUav(drawArguments)) return false;

        resources.Update(input, instances.data(), instances.size_bytes());
        if constexpr (queued) resources.Update(spheres, bounds.data(), bounds.size_bytes());
        resources.Update(viewInput, &view, sizeof(view));
        const auto initialArguments = MakeEmptyDrawArguments(call);
        resources.Update(drawArguments, &initialArguments, sizeof(initialArguments));

        ComputeCall compute;
        compute.shader = cullShader;
        compute.srvBuffers[BOUNDS_SRV_SLOT] = spheres;
        compute.srvBuffers[INSTANCES_SRV_SLOT] = input;
        compute.srvBuffers[VIEW_SRV_SLOT] = viewInput;
        compute.uavBuffers[VISIBLE_INSTANCES_UAV_SLOT] = output;
        compute.uavBuffers[DRAW_ARGUMENTS_UAV_SLOT] = drawArguments;
        if constexpr (queued) {
            /// @note Native rejection uses the original instance order and bypasses indirect drawing.
            DrawCall fallback = call;
            fallback.instanceBuffer = input;
            fallback.instanceCount = view.count;
            DrawCall indirect = fallback;
            indirect.instanceBuffer = output;
            indirect.indirectArguments = drawArguments;
            gpu_cull_detail::SubmitCullDraw(renderer, resources, compute, indirect, fallback);
            call = std::move(indirect);
        } else {
            compute.dispatchX = (view.count + CULL_THREADS_PER_GROUP - 1) / CULL_THREADS_PER_GROUP;
            if (!renderer.TryDispatch(compute, resources)) return false;
            call.instanceBuffer = output;
            call.instanceCount = view.count;
            call.indirectArguments = drawArguments;
        }
        return true;
    }
};

GpuInstanceCulling::GpuInstanceCulling() : m_state(std::make_unique<State>()) {}
GpuInstanceCulling::~GpuInstanceCulling() = default;

bool GpuInstanceCulling::Prepare(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
    std::span<const GpuCullInstance> instances, DrawCall& call)
{
    if (!renderer.SupportsIndirectDraw() || instances.empty() || instances.size() > MAX_CULL_GROUPS * CULL_THREADS_PER_GROUP
        || !IsIndexedCullDraw(call)) return false;

    const auto* indices = resources.Get(call.indexBuffer);
    if (!indices || static_cast<uint64_t>(call.startIndex) + call.indexCount > indices->GetSize() / sizeof(uint32_t))
        return false;
    for (const auto& instance : instances)
        if (!IsFiniteCullInstance(instance)) return false;

    return State::Cull(m_state, renderer, resources, frustum, instances, {}, call);
}

bool GpuInstanceCulling::SubmitDeformed(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
    std::span<const GpuDeformedInstance> instances, std::span<const math::Vector4> bounds, DrawCall& call)
{
    if (!renderer.SupportsIndirectDraw() || instances.empty() || instances.size() > MAX_QUEUED_INSTANCES
        || instances.size() != bounds.size() || !IsIndexedCullDraw(call)
        || call.preparedSkinning || !call.indexBuffer.IsValid()) {
        return false;
    }
    for (size_t index = 0; index < instances.size(); ++index) {
        const auto& instance = instances[index];
        if (!IsFiniteCullInstance({instance.world, instance.worldInvTranspose, bounds[index]})
            || instance.vertexIndex == INVALID_BINDLESS_INDEX) {
            return false;
        }
    }

    return State::Cull(m_state, renderer, resources, frustum, instances, bounds, call);
}

bool GpuInstanceCulling::SubmitStatic(IRenderer& renderer, ResourceManager& resources, const math::Frustum& frustum,
    std::span<const GpuCullInstance> source, DrawCall& call)
{
    if (source.empty() || source.size() > MAX_QUEUED_INSTANCES) return false;

    std::vector<PerInstanceData> transforms;
    std::vector<math::Vector4> spheres;
    transforms.reserve(source.size());
    spheres.reserve(source.size());
    for (const auto& instance : source) {
        transforms.push_back({instance.world, instance.worldInvTranspose});
        spheres.push_back(instance.bounds);
    }

    if (!renderer.SupportsIndirectDraw() || !IsIndexedCullDraw(call) || call.preparedSkinning || !call.indexBuffer.IsValid())
        return false;
    for (size_t index = 0; index < transforms.size(); ++index) {
        const auto& transform = transforms[index];
        if (!IsFiniteCullInstance({transform.world, transform.worldInvTranspose, spheres[index]})) return false;
    }

    return State::Cull(m_state, renderer, resources, frustum, std::span<const PerInstanceData>(transforms), spheres, call);
}

void GpuInstanceCulling::Release(ResourceManager& resources)
{
    if (!m_state->BelongsTo(resources)) return;

    m_state->queuedInputs.ReleaseAll(resources);
    m_state->deformedInputs.ReleaseAll(resources);
    m_state->queuedBounds.ReleaseAll(resources);
    m_state->deformedOutputs.ReleaseAll(resources);
    m_state->inputs.ReleaseAll(resources);
    m_state->views.ReleaseAll(resources);
    m_state->outputs.ReleaseAll(resources);
    m_state->arguments.ReleaseAll(resources);
}
} /// @note namespace fbzz::renderer
