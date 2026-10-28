/// @file    GpuGeometrySelection.cpp
/// @brief   候補とビューを検査し、GPU選択・間接引数生成をビュー別バッファで実行する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Renderer/GpuGeometrySelection.hpp>

#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fbzz::renderer {
namespace {

constexpr uint32_t kMaximumDispatchGroups = 65535;
constexpr uint32_t kMaximumCandidates = kGpuGeometrySelectionGroupSize * kMaximumDispatchGroups;
constexpr uint32_t kDecisionStride = sizeof(GpuGeometryDecision);
constexpr uint32_t kFramesInFlight = 3;
constexpr float kPlaneLengthTolerance = 0.01f;
constexpr const char* kSelectionShaderPath = "Assets/Shaders/Pipeline/GpuGeometrySelection.cs.hlsl";

bool IsFinite(float value)
{
    return std::isfinite(value);
}

bool ValidateView(const GpuGeometrySelectionView& view)
{
    for (const auto& plane : view.frustumPlanes) {
        float lengthSquared = 0.0f;
        for (float component : plane) {
            if (!IsFinite(component)) return false;
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
            lengthSquared += plane[axis] * plane[axis];
        if (!IsFinite(lengthSquared) || std::abs(lengthSquared - 1.0f) > kPlaneLengthTolerance)
            return false;
    }

    for (float component : view.cameraAndProjectionScale)
        if (!IsFinite(component)) return false;
    for (float component : view.projectionMetrics)
        if (!IsFinite(component)) return false;

    const float projectionScale = view.cameraAndProjectionScale[3];
    const float orthographicPixelsPerWorld = view.projectionMetrics[0];
    const float targetCellPixels = view.projectionMetrics[1];
    const float minPerspectiveDepth = view.projectionMetrics[2];
    const float orthographicFlag = view.projectionMetrics[3];
    return projectionScale > 0.0f
        && targetCellPixels > 0.0f
        && minPerspectiveDepth > 0.0f
        && (orthographicFlag == 0.0f || orthographicFlag == 1.0f)
        && (orthographicFlag == 0.0f || orthographicPixelsPerWorld > 0.0f);
}

bool ValidateCandidates(std::span<const GpuGeometryCandidate> candidates,
    uint32_t indexCapacityElements, uint32_t vertexCapacityElements)
{
    if (candidates.empty() || candidates.size() > kMaximumCandidates
        || indexCapacityElements == 0 || vertexCapacityElements == 0)
        return false;

    const uint64_t groupCount =
        (static_cast<uint64_t>(candidates.size()) + kGpuGeometrySelectionGroupSize - 1u)
        / kGpuGeometrySelectionGroupSize;
    if (groupCount == 0 || groupCount > kMaximumDispatchGroups)
        return false;

    for (const GpuGeometryCandidate& candidate : candidates) {
        if (candidate.lodCount == 0 || candidate.lodCount > kGpuGeometryMaxLods)
            return false;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (!IsFinite(candidate.boundsMin[axis]) || !IsFinite(candidate.boundsMax[axis])
                || candidate.boundsMin[axis] > candidate.boundsMax[axis])
                return false;
        }

        float previousCellSize = 0.0f;
        for (uint32_t lodIndex = 0; lodIndex < candidate.lodCount; ++lodIndex) {
            const GpuGeometryLodRange& lod = candidate.lods[lodIndex];
            const uint64_t indexEnd = static_cast<uint64_t>(lod.startIndex) + lod.indexCount;
            const uint64_t vertexEnd = static_cast<uint64_t>(static_cast<uint32_t>(lod.baseVertex))
                + lod.vertexCount;
            if (lod.indexCount == 0 || lod.indexCount % 3u != 0
                || indexEnd > indexCapacityElements
                || lod.baseVertex < 0 || lod.vertexCount == 0
                || vertexEnd > vertexCapacityElements
                || !IsFinite(lod.cellSizeWorld) || lod.cellSizeWorld <= 0.0f
                || (lodIndex != 0 && lod.cellSizeWorld < previousCellSize))
                return false;
            previousCellSize = lod.cellSizeWorld;
        }
    }
    return true;
}

class WritableStructuredBufferPool {
public:
    ResourceHandle<StructuredBufferTag> Acquire(ResourceManager& resources,
        uint32_t elementCount, uint32_t stride)
    {
        if (elementCount == 0 || stride == 0) return {};
        ResetForResources(resources);

        uint32_t capacity = 1;
        while (capacity < elementCount) {
            if (capacity > std::numeric_limits<uint32_t>::max() / 2u) return {};
            capacity *= 2u;
        }

        Bucket& bucket = BucketFor(capacity, stride);
        auto& lane = bucket.lanes[m_lane];
        if (bucket.next >= lane.size()) {
            const auto handle = resources.CreateRWStructuredBuffer(nullptr, capacity, stride);
            const auto* buffer = resources.Get(handle);
            if (!handle.IsValid() || !buffer || buffer->GetElementCount() < capacity
                || buffer->GetStride() != stride || buffer->GetSize() < static_cast<size_t>(capacity) * stride
                || buffer->GetBindlessUavIndex() == INVALID_BINDLESS_INDEX)
                return {};
            lane.push_back(handle);
        }
        return lane[bucket.next++];
    }

    void ReleaseAll(ResourceManager& resources)
    {
        for (Bucket& bucket : m_buckets)
            for (auto& lane : bucket.lanes)
                for (ResourceHandle<StructuredBufferTag> handle : lane)
                    if (handle.IsValid()) resources.Release(handle);
        m_buckets.clear();
        m_resetVersion = resources.GetResetVersion();
        m_frame = std::numeric_limits<uint64_t>::max();
        m_lane = 0;
    }

private:
    struct Bucket {
        uint32_t capacity = 0;
        uint32_t stride = 0;
        size_t next = 0;
        std::array<std::vector<ResourceHandle<StructuredBufferTag>>, kFramesInFlight> lanes;
    };

    void ResetForResources(ResourceManager& resources)
    {
        if (m_resetVersion != resources.GetResetVersion()) {
            m_resetVersion = resources.GetResetVersion();
            m_buckets.clear();
        }
        if (m_frame != resources.FrameStamp()) {
            m_frame = resources.FrameStamp();
            m_lane = static_cast<size_t>(m_frame % kFramesInFlight);
            for (Bucket& bucket : m_buckets) bucket.next = 0;
        }
    }

    Bucket& BucketFor(uint32_t capacity, uint32_t stride)
    {
        for (Bucket& bucket : m_buckets)
            if (bucket.capacity == capacity && bucket.stride == stride) return bucket;
        m_buckets.push_back(Bucket{capacity, stride, 0, {}});
        return m_buckets.back();
    }

    std::vector<Bucket> m_buckets;
    uint64_t m_frame = std::numeric_limits<uint64_t>::max();
    uint64_t m_resetVersion = 0;
    size_t m_lane = 0;
};

} /// @note namespace

struct GpuGeometrySelection::State {
    DynamicStructuredBufferPool candidateInputs{1};
    DynamicStructuredBufferPool viewInputs{1};
    WritableStructuredBufferPool decisions;
    WritableStructuredBufferPool arguments;
    ResourceHandle<ShaderTag> computeShader{};
    uint64_t resourceOwnerId = 0;
    uint64_t resetVersion = 0;
};

GpuGeometrySelection::GpuGeometrySelection()
    : m_state(std::make_unique<State>())
{
}

GpuGeometrySelection::~GpuGeometrySelection() = default;

bool GpuGeometrySelection::Prepare(IRenderer& renderer, ResourceManager& resources,
    const GpuGeometrySelectionView& view, std::span<const GpuGeometryCandidate> candidates,
    uint32_t indexCapacityElements, uint32_t vertexCapacityElements, GpuGeometryDispatch& out)
{
    if (!m_state || !renderer.SupportsIndirectDraw() || !ValidateView(view)
        || !ValidateCandidates(candidates, indexCapacityElements, vertexCapacityElements))
        return false;

    if (m_state->resourceOwnerId != resources.GetInstanceId()
        || m_state->resetVersion != resources.GetResetVersion()) {
        /// @note Reset invalidates the old handles; a different manager must never receive their Release calls.
        m_state = std::make_unique<State>();
        m_state->resourceOwnerId = resources.GetInstanceId();
        m_state->resetVersion = resources.GetResetVersion();
    }
    State& state = *m_state;
    if (!resources.Get(state.computeShader))
        state.computeShader = resources.LoadShader(kSelectionShaderPath);
    if (!resources.Get(state.computeShader))
        return false;

    const uint32_t candidateCount = static_cast<uint32_t>(candidates.size());
    const auto candidateInput = state.candidateInputs.Acquire(
        resources, candidateCount, sizeof(GpuGeometryCandidate));
    const auto viewInput = state.viewInputs.Acquire(resources, 1, sizeof(GpuGeometrySelectionView));
    const auto decisionOutput = state.decisions.Acquire(resources, candidateCount, kDecisionStride);
    const auto argumentOutput = state.arguments.Acquire(
        resources, candidateCount, kGpuGeometryIndexedArgumentStride);
    if (!candidateInput.IsValid() || !viewInput.IsValid()
        || !decisionOutput.IsValid() || !argumentOutput.IsValid())
        return false;

    const auto* candidateBuffer = resources.Get(candidateInput);
    const auto* viewBuffer = resources.Get(viewInput);
    const auto* decisionBuffer = resources.Get(decisionOutput);
    const auto* argumentBuffer = resources.Get(argumentOutput);
    const size_t candidateBytes = candidates.size_bytes();
    if (!candidateBuffer || !viewBuffer || !decisionBuffer || !argumentBuffer
        || candidateBuffer->GetStride() != sizeof(GpuGeometryCandidate)
        || candidateBuffer->GetElementCount() < candidateCount
        || candidateBuffer->GetSize() < candidateBytes
        || viewBuffer->GetStride() != sizeof(GpuGeometrySelectionView)
        || viewBuffer->GetElementCount() < 1
        || viewBuffer->GetSize() < sizeof(GpuGeometrySelectionView)
        || decisionBuffer->GetStride() != kDecisionStride
        || decisionBuffer->GetElementCount() < candidateCount
        || decisionBuffer->GetSize() < static_cast<size_t>(candidateCount) * kDecisionStride
        || argumentBuffer->GetStride() != kGpuGeometryIndexedArgumentStride
        || argumentBuffer->GetElementCount() < candidateCount
        || argumentBuffer->GetSize() < static_cast<size_t>(candidateCount) * kGpuGeometryIndexedArgumentStride)
        return false;

    resources.Update(candidateInput, candidates.data(), candidateBytes);
    GpuGeometrySelectionView viewWithCount = view;
    viewWithCount.candidateCount = candidateCount;
    resources.Update(viewInput, &viewWithCount, sizeof(viewWithCount));

    ComputeCall call;
    call.shader = state.computeShader;
    call.srvBuffers[14] = candidateInput;
    call.srvBuffers[15] = viewInput;
    call.uavBuffers[0] = decisionOutput;
    call.uavBuffers[1] = argumentOutput;
    call.dispatchX = (candidateCount - 1u) / kGpuGeometrySelectionGroupSize + 1u;
    call.dispatchY = 1;
    call.dispatchZ = 1;
    if (!renderer.TryDispatch(call, resources))
        return false;

    GpuGeometryDispatch prepared;
    prepared.decisions = decisionOutput;
    prepared.indirectArguments = argumentOutput;
    prepared.candidateCount = candidateCount;
    prepared.indirectArgumentOffset = 0;
    out = prepared;
    return true;
}

void GpuGeometrySelection::Release(ResourceManager& resources)
{
    if (!m_state) return;
    if (m_state->resourceOwnerId != resources.GetInstanceId()
        || m_state->resetVersion != resources.GetResetVersion()) {
        m_state = std::make_unique<State>();
        return;
    }
    m_state->candidateInputs.ReleaseAll(resources);
    m_state->viewInputs.ReleaseAll(resources);
    m_state->decisions.ReleaseAll(resources);
    m_state->arguments.ReleaseAll(resources);
    m_state->computeShader = {};
    m_state->resetVersion = resources.GetResetVersion();
}

} /// @note namespace fbzz::renderer
