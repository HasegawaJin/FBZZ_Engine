// FBZZ Engine
// MeshTrailRenderSystem.cpp | fbzz::scene
// MeshTrailComponent の過去姿勢サンプリング、Skinned bone palette 保存、半透明 DrawCall 発行
#include <Engine/Scene/Systems/MeshTrailRenderSystem.hpp>

#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/MathUtils.hpp>
#include "RenderPasses/GeometryPasses.hpp"
#include <algorithm>
#include <vector>

namespace fbzz::scene {

namespace {

// MeshTrailCB — MeshTrailConstants(cbuffer b2) の C++ ミラー。
struct MeshTrailCB {
    math::Vector4 trailColor;
};

// SkinningCB — Common/Constants.hlsli の SkinningConstants と同じレイアウト。
// WHAT: AnimatorComponent の現在 boneMatrices をサンプル時点で固定し、過去姿勢の SkinnedMesh を再描画する。
struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

static_assert(sizeof(MeshTrailCB) == 16, "MeshTrailCB layout mismatch");

float DistanceSq(const math::Vector3& a, const math::Vector3& b)
{
    return (a - b).LengthSq();
}

void ReleaseSampleResources(MeshTrailSample& sample, renderer::ResourceManager& resources)
{
    if (sample.skinningCB.IsValid()) {
        resources.Release(sample.skinningCB);
        sample.skinningCB = {};
    }
}

void ClearSamples(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    for (auto& sample : trail.samples)
        ReleaseSampleResources(sample, resources);
    trail.samples.clear();
    trail.lastSampleTime = -1.0f;
}

void ExpireSamples(MeshTrailComponent& trail, renderer::ResourceManager& resources, float currentTime)
{
    const float duration = (std::max)(trail.duration, 0.01f);
    const float oldestAllowed = currentTime - duration;

    while (!trail.samples.empty() && trail.samples.front().timestamp < oldestAllowed) {
        ReleaseSampleResources(trail.samples.front(), resources);
        trail.samples.erase(trail.samples.begin());
    }

    trail.maxSamples = (std::max)(trail.maxSamples, 1);
    while (static_cast<int>(trail.samples.size()) > trail.maxSamples) {
        ReleaseSampleResources(trail.samples.front(), resources);
        trail.samples.erase(trail.samples.begin());
    }
}

bool ShouldSample(const MeshTrailComponent& trail, const math::Vector3& position, float currentTime)
{
    const bool firstSample = trail.lastSampleTime < 0.0f;
    const bool timeReady = firstSample || currentTime - trail.lastSampleTime >= trail.sampleInterval;
    if (!timeReady)
        return false;

    if (trail.samples.empty())
        return true;

    const float minDist = (std::max)(trail.minVertexDist, 0.0f);
    return DistanceSq(position, trail.samples.back().position) >= minDist * minDist;
}

void CaptureSample(GameObject& go, MeshTrailComponent& trail, float currentTime)
{
    MeshTrailSample sample{};
    sample.timestamp = currentTime;
    sample.position = go.transform.position;
    sample.world = go.transform.GetWorldMatrix();

    if (auto* animator = go.GetComponent<AnimatorComponent>()) {
        sample.boneMatrices = animator->boneMatrices;
        if (sample.boneMatrices.size() > asset::MAX_SKINNING_BONES)
            sample.boneMatrices.resize(asset::MAX_SKINNING_BONES);
    }

    trail.samples.push_back(std::move(sample));
    trail.lastSampleTime = currentTime;
}

void EnsureComponentResources(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    trail.duration = (std::max)(trail.duration, 0.01f);
    trail.sampleInterval = (std::max)(trail.sampleInterval, 0.0f);
    trail.minVertexDist = (std::max)(trail.minVertexDist, 0.0f);
    trail.maxSamples = (std::max)(trail.maxSamples, 1);

    if (!trail.meshTrailCB.IsValid())
        trail.meshTrailCB = resources.CreateConstantBuffer(sizeof(MeshTrailCB));
}

renderer::ResourceHandle<renderer::ConstantBufferTag> EnsureSampleSkinningCB(
    MeshTrailSample& sample,
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseCB)
{
    if (sample.boneMatrices.empty())
        return bindPoseCB;

    if (!sample.skinningCB.IsValid())
        sample.skinningCB = resources.CreateConstantBuffer(sizeof(SkinningCB));

    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();
    for (size_t i = 0; i < sample.boneMatrices.size() && i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = sample.boneMatrices[i];
    resources.Update(sample.skinningCB, &cb, sizeof(cb));
    return sample.skinningCB;
}

math::Vector4 SampleColor(const MeshTrailComponent& trail, const MeshTrailSample& sample, float currentTime)
{
    const float duration = (std::max)(trail.duration, 0.01f);
    const float newestWeight = math::Clamp01(1.0f - (currentTime - sample.timestamp) / duration);
    return trail.colorEnd + (trail.colorStart - trail.colorEnd) * newestWeight;
}

void DrawStaticMeshSample(
    MeshTrailComponent& trail,
    MeshTrailSample& sample,
    MeshRenderer& mr,
    RenderPassContext& ctx,
    float currentTime)
{
    if (!mr.enabled || !mr.mesh || mr.mesh->isSkinned)
        return;
    if (!mr.mesh->vertexBuffer.IsValid() || !mr.mesh->indexBuffer.IsValid())
        return;

    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    PerObjectCB objData{};
    objData.world = sample.world;
    objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
    resources.Update(h.objectCB, &objData, sizeof(objData));

    MeshTrailCB cb{};
    cb.trailColor = SampleColor(trail, sample, currentTime);
    resources.Update(trail.meshTrailCB, &cb, sizeof(cb));

    renderer::DrawCall dc;
    dc.vertexBuffer = mr.mesh->vertexBuffer;
    dc.indexBuffer = mr.mesh->indexBuffer;
    dc.indexCount = mr.mesh->indexCount;
    dc.vertexCount = mr.mesh->vertexCount;
    dc.shader = h.meshTrailShader;
    dc.pipelineState = trail.doubleSided ? h.meshTrailDoubleSidedPSO : h.meshTrailPSO;
    dc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[1] = h.objectCB;
    dc.constantBuffers[2] = trail.meshTrailCB;
    ctx.renderer.Submit(dc, resources);

    ++ctx.statsDrawCalls;
    ctx.statsVertexCount += static_cast<int>(mr.mesh->vertexCount);
    ctx.statsTriangleCount += static_cast<int>(mr.mesh->indexCount / 3u);
}

void DrawSkinnedMeshSample(
    MeshTrailComponent& trail,
    MeshTrailSample& sample,
    SkinnedMeshRenderer& smr,
    RenderPassContext& ctx,
    float currentTime)
{
    if (!smr.enabled || !smr.model)
        return;

    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    PerObjectCB objData{};
    objData.world = sample.world;
    objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
    resources.Update(h.objectCB, &objData, sizeof(objData));

    MeshTrailCB cb{};
    cb.trailColor = SampleColor(trail, sample, currentTime);
    resources.Update(trail.meshTrailCB, &cb, sizeof(cb));

    const auto skinCB = EnsureSampleSkinningCB(sample, resources, h.bindPoseSkinningCB);

    for (const auto& meshPtr : smr.model->meshes) {
        if (!meshPtr)
            continue;
        if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid())
            continue;

        renderer::DrawCall dc;
        dc.vertexBuffer = meshPtr->vertexBuffer;
        dc.indexBuffer = meshPtr->indexBuffer;
        dc.indexCount = meshPtr->indexCount;
        dc.vertexCount = meshPtr->vertexCount;
        dc.shader = h.skinnedMeshTrailShader;
        dc.pipelineState = trail.doubleSided ? h.meshTrailDoubleSidedPSO : h.meshTrailPSO;
        dc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        dc.constantBuffers[2] = trail.meshTrailCB;
        dc.constantBuffers[7] = skinCB;
        ctx.renderer.Submit(dc, resources);

        ++ctx.statsDrawCalls;
        ctx.statsVertexCount += static_cast<int>(meshPtr->vertexCount);
        ctx.statsTriangleCount += static_cast<int>(meshPtr->indexCount / 3u);
    }
}

} // namespace

void ExecuteMeshTrailPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    if (!h.meshTrailShader.IsValid() || !h.skinnedMeshTrailShader.IsValid())
        return;
    if (!h.meshTrailPSO.IsValid() || !h.meshTrailDoubleSidedPSO.IsValid())
        return;

    ctx.renderer.SetRenderTarget(h.hdrRT, resources);

    const float currentTime = core::Time::TotalTime();

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask))
            continue;

        auto* trail = go.GetComponent<MeshTrailComponent>();
        if (!trail)
            continue;

        if (!trail->enabled) {
            ClearSamples(*trail, resources);
            trail->clearRequested = false;
            continue;
        }

        if (trail->clearRequested) {
            ClearSamples(*trail, resources);
            trail->clearRequested = false;
        }

        const bool hasStaticMesh = go.GetComponent<MeshRenderer>() != nullptr;
        const bool hasSkinnedMesh = go.GetComponent<SkinnedMeshRenderer>() != nullptr;
        if (!hasStaticMesh && !hasSkinnedMesh)
            continue;

        EnsureComponentResources(*trail, resources);
        ExpireSamples(*trail, resources, currentTime);

        if (ShouldSample(*trail, go.transform.position, currentTime))
            CaptureSample(go, *trail, currentTime);

        ExpireSamples(*trail, resources, currentTime);

        for (auto& sample : trail->samples) {
            if (auto* mr = go.GetComponent<MeshRenderer>())
                DrawStaticMeshSample(*trail, sample, *mr, ctx, currentTime);
            if (auto* smr = go.GetComponent<SkinnedMeshRenderer>())
                DrawSkinnedMeshSample(*trail, sample, *smr, ctx, currentTime);
        }
    }
}

} // namespace fbzz::scene
