/// @file    RenderMeshTrailSampling.cpp
/// @brief   残像の姿勢履歴とサンプル GPU 資源の寿命を管理する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderMeshTrailExtraction.hpp"
#include "../../Passes/Geometry/GeometryPasses.hpp"
#include <Engine/Asset/Model/Model.hpp>
#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <algorithm>
#include <utility>
#include <vector>

namespace fbzz::scene::mesh_trail_extraction_detail {

MeshTrailSample& ReadMeshTrailSample(MeshTrailComponent& trail, int logicalIndex)
{
    const int physicalIndex = (trail.sampleHead + logicalIndex) % trail.allocatedMaxSamples;
    return trail.samples[static_cast<size_t>(physicalIndex)];
}

namespace {

float DistanceSq(const math::Vector3& a, const math::Vector3& b)
{
    return (a - b).LengthSq();
}

void ReleaseSampleResources(MeshTrailSample& sample)
{
    sample.skinningCB.Reset();
    sample.skinningCBDirty = true;
}

void EnsureSampleStorage(MeshTrailComponent& trail)
{
    const int sampleCapacity = (std::max)(trail.maxSamples, 1);
    if (trail.allocatedMaxSamples == sampleCapacity &&
        trail.samples.size() == static_cast<size_t>(sampleCapacity)) {
        return;
    }

    std::vector<MeshTrailSample> retainedSamples;
    retainedSamples.reserve(static_cast<size_t>((std::min)(trail.sampleCount, sampleCapacity)));
    if (trail.allocatedMaxSamples > 0 && !trail.samples.empty()) {
        const int keepCount = (std::min)(trail.sampleCount, sampleCapacity);
        const int discardCount = trail.sampleCount - keepCount;
        for (int sampleIndex = 0; sampleIndex < discardCount; ++sampleIndex) {
            ReleaseSampleResources(ReadMeshTrailSample(trail, sampleIndex));
        }
        for (int sampleIndex = discardCount; sampleIndex < trail.sampleCount; ++sampleIndex) {
            retainedSamples.push_back(std::move(ReadMeshTrailSample(trail, sampleIndex)));
        }
    }

    trail.samples.clear();
    trail.samples.resize(static_cast<size_t>(sampleCapacity));
    for (size_t sampleIndex = 0; sampleIndex < retainedSamples.size(); ++sampleIndex) {
        trail.samples[sampleIndex] = std::move(retainedSamples[sampleIndex]);
    }
    trail.sampleHead = 0;
    trail.sampleCount = static_cast<int>(retainedSamples.size());
    trail.allocatedMaxSamples = sampleCapacity;
}

void ExpireSamples(MeshTrailComponent& trail, float currentTime)
{
    const float duration = (std::max)(trail.duration, 0.01f);
    const float earliestTimestamp = currentTime - duration;

    while (trail.sampleCount > 0 && ReadMeshTrailSample(trail, 0).timestamp < earliestTimestamp) {
        auto& oldest = ReadMeshTrailSample(trail, 0);
        ReleaseSampleResources(oldest);
        oldest = {};
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
        --trail.sampleCount;
    }

    trail.maxSamples = (std::max)(trail.maxSamples, 1);
    while (trail.sampleCount > trail.maxSamples) {
        auto& oldest = ReadMeshTrailSample(trail, 0);
        ReleaseSampleResources(oldest);
        oldest = {};
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
        --trail.sampleCount;
    }
}

bool ShouldSample(const MeshTrailComponent& trail, const math::Vector3& position, float currentTime)
{
    const bool firstSample = trail.lastSampleTime < 0.0f;
    const bool timeReady = firstSample || currentTime - trail.lastSampleTime >= trail.sampleInterval;
    if (!timeReady) {
        return false;
    }

    if (trail.sampleCount == 0) {
        return true;
    }

    const float minimumDistance = (std::max)(trail.minVertexDist, 0.0f);
    const int lastSampleIndex = (trail.sampleHead + trail.sampleCount - 1) % trail.allocatedMaxSamples;
    return DistanceSq(position, trail.samples[static_cast<size_t>(lastSampleIndex)].position)
        >= minimumDistance * minimumDistance;
}

void CaptureSample(GameObject& owner, MeshTrailComponent& trail, float currentTime)
{
    MeshTrailSample sample{};
    sample.timestamp = currentTime;
    sample.position = owner.transform.worldPosition;
    sample.world = owner.transform.GetWorldMatrix();

    // 子 SkinnedMeshRenderer は親 GameObject の Animator を共有する。自 GO だけを見ると
    // bone palette が空になり、武器残像が bind pose で描画される。
    if (auto* animator = FindAnimator(owner)) {
        sample.boneMatrices = animator->boneMatrices;
        if (sample.boneMatrices.size() > asset::MAX_SKINNING_BONES) {
            sample.boneMatrices.resize(asset::MAX_SKINNING_BONES);
        }
    }

    if (trail.allocatedMaxSamples <= 0) {
        return;
    }

    if (trail.sampleCount == trail.allocatedMaxSamples) {
        MeshTrailSample& writeSlot = trail.samples[static_cast<size_t>(trail.sampleHead)];
        ReleaseSampleResources(writeSlot);
        writeSlot = std::move(sample);
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
    } else {
        const int writeIndex = (trail.sampleHead + trail.sampleCount) % trail.allocatedMaxSamples;
        trail.samples[static_cast<size_t>(writeIndex)] = std::move(sample);
        ++trail.sampleCount;
    }
    trail.lastSampleTime = currentTime;
}

} // namespace

void ClearMeshTrailSamples(MeshTrailComponent& trail)
{
    for (int sampleIndex = 0; sampleIndex < trail.sampleCount; ++sampleIndex) {
        auto& sample = ReadMeshTrailSample(trail, sampleIndex);
        ReleaseSampleResources(sample);
        sample = {};
    }
    trail.sampleHead = 0;
    trail.sampleCount = 0;
    trail.lastSampleTime = -1.0f;
}

void PrepareMeshTrailSampleStorage(MeshTrailComponent& trail)
{
    trail.duration = (std::max)(trail.duration, 0.01f);
    trail.sampleInterval = (std::max)(trail.sampleInterval, 0.0f);
    trail.minVertexDist = (std::max)(trail.minVertexDist, 0.0f);
    trail.maxSamples = (std::max)(trail.maxSamples, 1);
    EnsureSampleStorage(trail);
}

void UpdateMeshTrailSamples(GameObject& owner, MeshTrailComponent& trail, float currentTime)
{
    ExpireSamples(trail, currentTime);
    if (trail.enabled && ShouldSample(trail, owner.transform.worldPosition, currentTime)) {
        CaptureSample(owner, trail, currentTime);
    }
    ExpireSamples(trail, currentTime);
}

} // namespace fbzz::scene::mesh_trail_extraction_detail
