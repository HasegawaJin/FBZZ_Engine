/// @file    RenderTrailSampling.cpp
/// @brief   軌跡点の採取・寿命管理・ビームの組み立て。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderTrailSampling.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <Engine/Scene/Hierarchy/Transform.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>

namespace fbzz::scene::trail_extraction_detail {

void ResetTrailSampleStorage(TrailComponent& trail)
{
    trail.maxPoints = (std::max)(trail.maxPoints, 2);
    trail.pointBuffer.resize(static_cast<size_t>(trail.maxPoints));
    trail.ringHead = 0;
    trail.ringTail = 0;
    trail.ringCount = 0;
    trail.lastSampleTime = -1.0f;
}

namespace {

TrailPoint& RingAt(TrailComponent& trail, int index)
{
    assert(index >= 0 && index < trail.ringCount);
    return trail.pointBuffer[static_cast<size_t>((trail.ringHead + index) % trail.maxPoints)];
}

void RingPushBack(TrailComponent& trail, const TrailPoint& point)
{
    if (trail.pointBuffer.size() != static_cast<size_t>(trail.maxPoints)) {
        ResetTrailSampleStorage(trail);
    }

    if (trail.ringCount == trail.maxPoints) {
        trail.ringHead = (trail.ringHead + 1) % trail.maxPoints;
    } else {
        ++trail.ringCount;
    }

    trail.pointBuffer[static_cast<size_t>(trail.ringTail)] = point;
    trail.ringTail = (trail.ringTail + 1) % trail.maxPoints;
}

void RingExpireOld(TrailComponent& trail, float currentTime)
{
    const float oldestAllowedTime = currentTime - (std::max)(trail.duration, math::EPSILON);
    while (trail.ringCount > 0 && RingAt(trail, 0).timestamp < oldestAllowedTime) {
        trail.ringHead = (trail.ringHead + 1) % trail.maxPoints;
        --trail.ringCount;
    }
}

float DistanceSq(const math::Vector3& a, const math::Vector3& b)
{
    return (a - b).LengthSq();
}

math::Vector3 ResolveTrailSamplePosition(Scene& scene, GameObject& owner, const TrailComponent& trail)
{
    if (!trail.attachBone.empty()) {
        if (auto* smr = owner.GetComponent<SkinnedMeshRenderer>()) {
            if (smr->model && smr->model->skeleton) {
                const auto it = smr->model->skeleton->nodeMap.find(trail.attachBone);
                if (it != smr->model->skeleton->nodeMap.end()) {
                    const int nodeIndex = it->second;
                    if (nodeIndex >= 0 && nodeIndex < static_cast<int>(smr->nodeEntities.size())) {
                        if (auto* boneGo = scene.GetGameObject(smr->nodeEntities[static_cast<size_t>(nodeIndex)]))
        return boneGo->transform.worldPosition +
            boneGo->transform.worldRotation * trail.attachOffset;
                    }
                }
            }
        }
    }

        return owner.transform.worldPosition +
            owner.transform.worldRotation * trail.attachOffset;
}

} // namespace

void ClearTrailSamples(TrailComponent& trail)
{
    trail.ringHead = 0;
    trail.ringTail = 0;
    trail.ringCount = 0;
    trail.lastSampleTime = -1.0f;
}

namespace {

void UpdateTrailPoints(TrailComponent& trail, const math::Vector3& worldPosition, float currentTime)
{
    RingExpireOld(trail, currentTime);

    // 瞬間移動の切断。sampleInterval の «待ち» より前に判定する ─ 跳んだフレームを
    // 待たせると、その 1 フレームのあいだ古い点と新しい点が 1 本の筋でつながる。
    if (trail.ringCount > 0
        && TrailIsDiscontinuous(trail, RingAt(trail, trail.ringCount - 1).position, worldPosition)) {
        ClearTrailSamples(trail);
    }

    const bool firstSample = trail.lastSampleTime < 0.0f;
    const bool timeReady = firstSample || currentTime - trail.lastSampleTime >= trail.sampleInterval;
    if (!timeReady) {
        return;
    }

    const bool distanceReady =
        trail.ringCount == 0 ||
        DistanceSq(worldPosition, RingAt(trail, trail.ringCount - 1).position) >= trail.minVertexDist * trail.minVertexDist;

    if (distanceReady) {
        RingPushBack(trail, { worldPosition, currentTime });
        trail.lastSampleTime = currentTime;
    }
}

} // namespace

const TrailPoint& ReadTrailPoint(const TrailComponent& trail, int logicalIndex)
{
    assert(logicalIndex >= 0 && logicalIndex < trail.ringCount);
    return trail.pointBuffer[static_cast<size_t>((trail.ringHead + logicalIndex) % trail.maxPoints)];
}

void AdvanceTrailSamples(Scene& scene, GameObject& owner, TrailComponent& trail, float currentTime, uint64_t frameStamp)
{
    if (trail.lastExtractionFrame == frameStamp) {
        return;
    }
    trail.lastExtractionFrame = frameStamp;
    if (trail.enabled && trail.beamMode) {
        const auto transformPoint = [&owner, &trail](const math::Vector3& point) {
            if (trail.beamWorldSpace) {
                return point;
            }
            const math::Vector3 scaled{
                point.x * owner.transform.worldScale.x,
                point.y * owner.transform.worldScale.y,
                point.z * owner.transform.worldScale.z };
            return owner.transform.worldPosition + owner.transform.worldRotation * scaled;
        };
        trail.ringHead = 0;
        trail.ringTail = 0;
        trail.ringCount = 0;
        // 折れ線が与えられていればそれを、無ければ従来どおり 2 端点を描く。
        if (trail.beamPoints.size() >= 2) {
            for (const math::Vector3& point : trail.beamPoints) {
                RingPushBack(trail, { transformPoint(point), currentTime });
            }
        } else {
            RingPushBack(trail, { transformPoint(trail.beamStart), currentTime });
            RingPushBack(trail, { transformPoint(trail.beamEnd), currentTime });
        }
        trail.lastSampleTime = currentTime;
    } else if (trail.enabled) {
        const math::Vector3 samplePosition = ResolveTrailSamplePosition(scene, owner, trail);
        UpdateTrailPoints(trail, samplePosition, currentTime);
    }
    else {
        RingExpireOld(trail, currentTime);
    }
}

} // namespace fbzz::scene::trail_extraction_detail
