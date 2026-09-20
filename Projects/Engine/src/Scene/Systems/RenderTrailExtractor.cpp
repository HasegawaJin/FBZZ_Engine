/// @file    RenderTrailExtractor.cpp
/// @brief   TrailComponent のリングバッファ更新、Catmull-Rom 補間、リボン頂点生成、DrawCall 発行 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Engine/Scene/Systems/RenderTrailExtractor.hpp>
#include <Graphics/Renderer/RenderScene.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/DynamicBufferPool.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Util/Easing.hpp>
#include <Math/MathUtils.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fbzz::scene {
namespace {
void InitTrailStorage(TrailComponent& trail)
{
    trail.maxPoints = (std::max)(trail.maxPoints, 2);
    trail.pointBuffer.resize(static_cast<size_t>(trail.maxPoints));
    trail.ringHead = 0;
    trail.ringTail = 0;
    trail.ringCount = 0;
    trail.lastSampleTime = -1.0f;
}

TrailPoint& RingAt(TrailComponent& trail, int index)
{
    assert(index >= 0 && index < trail.ringCount);
    return trail.pointBuffer[static_cast<size_t>((trail.ringHead + index) % trail.maxPoints)];
}

const TrailPoint& RingAt(const TrailComponent& trail, int index)
{
    assert(index >= 0 && index < trail.ringCount);
    return trail.pointBuffer[static_cast<size_t>((trail.ringHead + index) % trail.maxPoints)];
}

void RingPushBack(TrailComponent& trail, const TrailPoint& point)
{
    if (trail.pointBuffer.size() != static_cast<size_t>(trail.maxPoints))
        InitTrailStorage(trail);

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
void EnsureResources(TrailComponent& trail, RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    trail.maxPoints = (std::max)(trail.maxPoints, 2);
    trail.duration = (std::max)(trail.duration, 0.01f);
    trail.sampleInterval = (std::max)(trail.sampleInterval, 0.0f);
    trail.minVertexDist = (std::max)(trail.minVertexDist, 0.0f);
    trail.smoothSubdivisions = (std::max)(trail.smoothSubdivisions, 0);
    trail.uvTiling = (std::max)(trail.uvTiling, 0.001f);

    if (trail.pointBuffer.size() != static_cast<size_t>(trail.maxPoints))
        InitTrailStorage(trail);

    if (!trail.trailCB.IsValid())
        trail.trailCB = resources.CreateConstantBuffer(sizeof(TrailCB));

    /// @note materialPath が設定されている場合: .mat の albedo テクスチャを優先する。
    if (!trail.materialPath.empty()) {
        const bool matChanged = (trail.loadedMaterialPath != trail.materialPath);
        if (matChanged) {
            trail.loadedMaterialPath = trail.materialPath;
            trail.loadedTexturePath.clear();
        }
        const auto matHandle = asset::AssetManager::Load<asset::MaterialAsset>(trail.materialPath);
        if (const auto* mat = asset::AssetManager::Get<asset::MaterialAsset>(matHandle)) {
            const auto it = mat->textures.find("albedo");
            const std::string& resolvedTex = (it != mat->textures.end()) ? it->second : std::string{};
            if (!trail.texture.IsValid() || trail.loadedTexturePath != resolvedTex) {
                if (resolvedTex.empty()) {
                    trail.texture = resources.GetWhiteTexture();
                } else {
                    trail.texture = resources.LoadTexture(resolvedTex);
                }
                trail.loadedTexturePath = resolvedTex;
            }
        }
    } else if (!trail.texture.IsValid()) {
        /// @note 共有の 1 枚を借りる。実体ごとに作ると、その実体が畳まれたぶんだけ GPU に残る。
        trail.texture = resources.GetWhiteTexture();
        trail.loadedTexturePath.clear();
    }
}

math::Vector3 ResolveTrailSamplePosition(Scene& scene, GameObject& go, const TrailComponent& trail)
{
    if (!trail.attachBone.empty()) {
        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
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

        return go.transform.worldPosition +
            go.transform.worldRotation * trail.attachOffset;
}

void ClearRing(TrailComponent& trail)
{
    trail.ringHead = 0;
    trail.ringTail = 0;
    trail.ringCount = 0;
    trail.lastSampleTime = -1.0f;
}

void UpdateTrailPoints(TrailComponent& trail, const math::Vector3& currentPos, float currentTime)
{
    RingExpireOld(trail, currentTime);

    /// @note 瞬間移動の切断。sampleInterval の «待ち» より前に判定する ─ 跳んだフレームを
    /// @note 待たせると、その 1 フレームのあいだ古い点と新しい点が 1 本の筋でつながる。
    if (trail.ringCount > 0
        && TrailIsDiscontinuous(trail, RingAt(trail, trail.ringCount - 1).position, currentPos)) {
        ClearRing(trail);
    }

    const bool firstSample = trail.lastSampleTime < 0.0f;
    const bool timeReady = firstSample || currentTime - trail.lastSampleTime >= trail.sampleInterval;
    if (!timeReady)
        return;

    const bool distanceReady =
        trail.ringCount == 0 ||
        DistanceSq(currentPos, RingAt(trail, trail.ringCount - 1).position) >= trail.minVertexDist * trail.minVertexDist;

    if (distanceReady) {
        RingPushBack(trail, { currentPos, currentTime });
        trail.lastSampleTime = currentTime;
    }
}
}
void ExtractRenderTrails(RenderPassContext& ctx, renderer::RenderScene& output) {
    const float currentTime = Time::time;
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy())
            continue;

        auto* trail = go.GetComponent<TrailComponent>();
        if (!trail)
            continue;

        if (!trail->enabled && trail->clearOnDisable) {
            ClearRing(*trail);
            continue;
        }

        EnsureResources(*trail, ctx);

        if (trail->lastExtractionFrame != ctx.resources.FrameStamp()) {
        trail->lastExtractionFrame = ctx.resources.FrameStamp();
        if (trail->enabled && trail->beamMode) {
            const auto transformPoint = [&go, &trail](const math::Vector3& point) {
                if (trail->beamWorldSpace) return point;
                const math::Vector3 scaled{
        point.x * go.transform.worldScale.x,
        point.y * go.transform.worldScale.y,
        point.z * go.transform.worldScale.z };
    return go.transform.worldPosition +
        go.transform.worldRotation * scaled;
            };
            trail->ringHead = 0;
            trail->ringTail = 0;
            trail->ringCount = 0;
            /// @note 折れ線が与えられていればそれを、無ければ従来どおり 2 端点を描く。
            if (trail->beamPoints.size() >= 2) {
                for (const math::Vector3& point : trail->beamPoints)
                    RingPushBack(*trail, { transformPoint(point), currentTime });
            } else {
                RingPushBack(*trail, { transformPoint(trail->beamStart), currentTime });
                RingPushBack(*trail, { transformPoint(trail->beamEnd), currentTime });
            }
            trail->lastSampleTime = currentTime;
        } else if (trail->enabled) {
            const math::Vector3 samplePos = ResolveTrailSamplePosition(ctx.scene, go, *trail);
            UpdateTrailPoints(*trail, samplePos, currentTime);
        }
        else
            RingExpireOld(*trail, currentTime);
        }
        if (trail->ringCount < 2)
            continue;

        renderer::RenderTrailInput input;
        input.layer = static_cast<uint32_t>(go.layer);
        input.duration = trail->duration;
        input.widthStart = trail->widthStart;
        input.widthEnd = trail->widthEnd;
        input.widthCurveEnabled = trail->widthCurveEnabled;
        input.widthCurve = trail->widthCurve;
        input.colorGradientEnabled = trail->colorGradientEnabled;
        input.colorGradient = trail->colorGradient;
        input.widthEasing = trail->widthEasing;
        input.colorStart = trail->colorStart;
        input.colorEnd = trail->colorEnd;
        input.alignment = trail->alignment;
        input.smoothSubdivisions = trail->smoothSubdivisions;
        input.uvMode = trail->uvMode;
        input.uvScrollSpeed = trail->uvScrollSpeed;
        input.uvTiling = trail->uvTiling;
        input.texture = trail->texture;
        input.trailCB = trail->trailCB;
        input.srgbTexture = IsEffectTextureSrgb(trail->loadedTexturePath);
        input.points.reserve(static_cast<size_t>(trail->ringCount));
        for (int i = 0; i < trail->ringCount; ++i) input.points.push_back(RingAt(*trail, i));
        output.trails.push_back(std::move(input));
    }
}
}
