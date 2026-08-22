// FBZZ Engine
// RenderPasses/Geometry/MeshTrailRenderPass.cpp | fbzz::scene
// MeshTrailComponent の過去姿勢サンプリング、Skinned bone palette 保存、半透明 DrawCall 発行 (IRenderPass 実装)
#include "Engine/Scene/Systems/RenderPasses/Geometry/MeshTrailRenderPass.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/SamplerMode.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/MathUtils.hpp>
#include "GeometryPasses.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

namespace {

// MeshTrailCB — MeshTrailConstants(cbuffer b2) の C++ ミラー。
struct MeshTrailCB {
    math::Vector4 trailColor;
};

// SkinningCB — Common/Constants.hlsli の SkinningConstants と同じレイアウト。
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

MeshTrailSample& SampleAt(MeshTrailComponent& trail, int logicalIndex)
{
    const int physicalIndex = (trail.sampleHead + logicalIndex) % trail.allocatedMaxSamples;
    return trail.samples[static_cast<size_t>(physicalIndex)];
}

const MeshTrailSample& SampleAt(const MeshTrailComponent& trail, int logicalIndex)
{
    const int physicalIndex = (trail.sampleHead + logicalIndex) % trail.allocatedMaxSamples;
    return trail.samples[static_cast<size_t>(physicalIndex)];
}

void EnsureSampleStorage(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    const int desiredMaxSamples = (std::max)(trail.maxSamples, 1);
    if (trail.allocatedMaxSamples == desiredMaxSamples &&
        trail.samples.size() == static_cast<size_t>(desiredMaxSamples))
        return;

    std::vector<MeshTrailSample> ordered;
    ordered.reserve(static_cast<size_t>((std::min)(trail.sampleCount, desiredMaxSamples)));
    if (trail.allocatedMaxSamples > 0 && !trail.samples.empty()) {
        const int keepCount = (std::min)(trail.sampleCount, desiredMaxSamples);
        const int discardCount = trail.sampleCount - keepCount;
        for (int i = 0; i < discardCount; ++i)
            ReleaseSampleResources(SampleAt(trail, i), resources);
        for (int i = discardCount; i < trail.sampleCount; ++i)
            ordered.push_back(std::move(SampleAt(trail, i)));
    }

    trail.samples.clear();
    trail.samples.resize(static_cast<size_t>(desiredMaxSamples));
    for (size_t i = 0; i < ordered.size(); ++i)
        trail.samples[i] = std::move(ordered[i]);
    trail.sampleHead = 0;
    trail.sampleCount = static_cast<int>(ordered.size());
    trail.allocatedMaxSamples = desiredMaxSamples;
}

void ClearSamples(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    for (int i = 0; i < trail.sampleCount; ++i) {
        auto& sample = SampleAt(trail, i);
        ReleaseSampleResources(sample, resources);
        sample = {};
    }
    trail.sampleHead = 0;
    trail.sampleCount = 0;
    trail.lastSampleTime = -1.0f;
}

void ExpireSamples(MeshTrailComponent& trail, renderer::ResourceManager& resources, float currentTime)
{
    const float duration = (std::max)(trail.duration, 0.01f);
    const float oldestAllowed = currentTime - duration;

    while (trail.sampleCount > 0 && SampleAt(trail, 0).timestamp < oldestAllowed) {
        auto& oldest = SampleAt(trail, 0);
        ReleaseSampleResources(oldest, resources);
        oldest = {};
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
        --trail.sampleCount;
    }

    trail.maxSamples = (std::max)(trail.maxSamples, 1);
    while (trail.sampleCount > trail.maxSamples) {
        auto& oldest = SampleAt(trail, 0);
        ReleaseSampleResources(oldest, resources);
        oldest = {};
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
        --trail.sampleCount;
    }
}

bool ShouldSample(const MeshTrailComponent& trail, const math::Vector3& position, float currentTime)
{
    const bool firstSample = trail.lastSampleTime < 0.0f;
    const bool timeReady = firstSample || currentTime - trail.lastSampleTime >= trail.sampleInterval;
    if (!timeReady)
        return false;

    if (trail.sampleCount == 0)
        return true;

    const float minDist = (std::max)(trail.minVertexDist, 0.0f);
    return DistanceSq(position, SampleAt(trail, trail.sampleCount - 1).position) >= minDist * minDist;
}

bool IsMeshIndexExcluded(const MeshTrailComponent& trail, int meshIndex)
{
    return std::find(trail.excludedMeshIndices.begin(), trail.excludedMeshIndices.end(), meshIndex)
        != trail.excludedMeshIndices.end();
}

void CaptureSample(GameObject& go, MeshTrailComponent& trail, renderer::ResourceManager& resources, float currentTime)
{
    MeshTrailSample sample{};
    sample.timestamp = currentTime;
            sample.position = go.transform.worldPosition;
            sample.world = go.transform.GetWorldMatrix();

    // 子SkinnedMeshRendererは親GameObjectのAnimatorを共有する。
    // WHY: 自GOだけを見るとbone paletteが空になり、武器残像がbind poseで描画されるため。
    if (auto* animator = FindAnimator(go)) {
        sample.boneMatrices = animator->boneMatrices;
        if (sample.boneMatrices.size() > asset::MAX_SKINNING_BONES)
            sample.boneMatrices.resize(asset::MAX_SKINNING_BONES);
    }

    if (trail.allocatedMaxSamples <= 0)
        return;

    if (trail.sampleCount == trail.allocatedMaxSamples) {
        MeshTrailSample& writeSlot = trail.samples[static_cast<size_t>(trail.sampleHead)];
        ReleaseSampleResources(writeSlot, resources);
        writeSlot = std::move(sample);
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
    } else {
        const int writeIndex = (trail.sampleHead + trail.sampleCount) % trail.allocatedMaxSamples;
        trail.samples[static_cast<size_t>(writeIndex)] = std::move(sample);
        ++trail.sampleCount;
    }
    trail.lastSampleTime = currentTime;
}

void EnsureComponentResources(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    trail.duration = (std::max)(trail.duration, 0.01f);
    trail.sampleInterval = (std::max)(trail.sampleInterval, 0.0f);
    trail.minVertexDist = (std::max)(trail.minVertexDist, 0.0f);
    trail.maxSamples = (std::max)(trail.maxSamples, 1);
    EnsureSampleStorage(trail, resources);

    if (!trail.meshTrailCB.IsValid())
        trail.meshTrailCB = resources.CreateConstantBuffer(sizeof(MeshTrailCB));

    // materialPath が設定されている場合: .mat の albedo テクスチャと doubleSided を優先する。
    if (!trail.materialPath.empty()) {
        const bool matChanged = (trail.loadedMaterialPath != trail.materialPath);
        if (matChanged) {
            trail.loadedMaterialPath = trail.materialPath;
            trail.loadedTexturePath.clear();
        }
        const auto matHandle = asset::AssetManager::LoadMaterial(trail.materialPath);
        if (const auto* mat = asset::AssetManager::GetMaterial(matHandle)) {
            const auto it = mat->textures.find("albedo");
            const std::string& resolvedTex = (it != mat->textures.end()) ? it->second : std::string{};
            if (!trail.texture.IsValid() || trail.loadedTexturePath != resolvedTex) {
                if (resolvedTex.empty()) {
                    static const uint8_t white[4] = { 255, 255, 255, 255 };
                    trail.texture = resources.CreateTexture(white, 1, 1);
                } else {
                    trail.texture = resources.LoadTexture(resolvedTex);
                }
                trail.loadedTexturePath = resolvedTex;
            }
            trail.doubleSided = mat->doubleSided;
        }
    } else if (!trail.texture.IsValid()) {
        static const uint8_t white[4] = { 255, 255, 255, 255 };
        trail.texture = resources.CreateTexture(white, 1, 1);
        trail.loadedTexturePath.clear();
    }
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

    if (!sample.skinningCBDirty)
        return sample.skinningCB;

    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();
    for (size_t i = 0; i < sample.boneMatrices.size() && i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = sample.boneMatrices[i];
    resources.Update(sample.skinningCB, &cb, sizeof(cb));
    sample.skinningCBDirty = false;
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
    if (!mr.enabled || !mr.lodVisible || !mr.mesh || mr.mesh->isSkinned)
        return;
    if (!mr.mesh->vertexBuffer.IsValid() || !mr.mesh->indexBuffer.IsValid())
        return;

    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    PerObjectCB objData{};
    objData.world = sample.world;
    objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
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
    dc.textures[0] = trail.texture;
    SubmitCounted(ctx, dc);
}

void DrawSkinnedMeshSample(
    MeshTrailComponent& trail,
    MeshTrailSample& sample,
    SkinnedMeshRenderer& smr,
    RenderPassContext& ctx,
    float currentTime)
{
    if (!smr.enabled || !smr.lodVisible || !smr.model)
        return;

    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    PerObjectCB objData{};
    objData.world = sample.world;
    objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
    resources.Update(h.objectCB, &objData, sizeof(objData));

    MeshTrailCB cb{};
    cb.trailColor = SampleColor(trail, sample, currentTime);
    resources.Update(trail.meshTrailCB, &cb, sizeof(cb));

    const auto skinCB = EnsureSampleSkinningCB(sample, resources, h.bindPoseSkinningCB);

    for (size_t meshIndex = 0; meshIndex < smr.SubmeshCount(); ++meshIndex) {
        // 対象の絞り込みは MeshTrailComponent::excludedMeshIndices で行う。
        // WHY 除外番号がローカルスロット番号か: 剣だけを残像化する、といった指定は
        //     「この Renderer の何番目か」で書くのが自然で、モデル全体の submesh 番号を
        //     知る必要がない。ノードごとに子 GO へ分けた構成では、剣の Renderer が
        //     持つ submesh は 1 個だけになり excludedMeshIndices すら不要になる。
        if (IsMeshIndexExcluded(trail, static_cast<int>(meshIndex)))
            continue;
        const renderer::Mesh* meshPtr = smr.SubmeshMesh(meshIndex);
        if (!meshPtr)
            continue;
        if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid())
            continue;

        renderer::DrawCall dc;
        dc.vertexBuffer = smr.ResolveSlotVertexBuffer(meshIndex, meshPtr->vertexBuffer);
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
        dc.textures[0] = trail.texture;
        SubmitCounted(ctx, dc);
    }
}

} // namespace

// ─── IRenderPass ──────────────────────────────────────────────────────────────

std::string_view MeshTrailRenderPass::Name() const { return "MeshTrail"; }

std::vector<renderer::RenderGraph::ResourceAccess> MeshTrailRenderPass::DeclareAccesses(
    const RenderPassContext&) const
{
    return { { "HDR", renderer::RenderGraph::ResourceUsage::ReadWrite } };
}

void MeshTrailRenderPass::Execute(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    if (!h.meshTrailShader.IsValid() || !h.skinnedMeshTrailShader.IsValid())
        return;
    if (!h.meshTrailPSO.IsValid() || !h.meshTrailDoubleSidedPSO.IsValid())
        return;

    ctx.renderer.SetRenderTarget(h.hdrRT, resources);
    ctx.renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);

    const float currentTime = Time::time;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask))
            continue;

        auto* trail = go.GetComponent<MeshTrailComponent>();
        if (!trail)
            continue;

        if (!trail->enabled && trail->clearOnDisable) {
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
        if (auto* emitter = go.GetComponent<ParticleEmitter>();
            emitter != nullptr && !emitter->meshParticlePath.empty()) {
            auto* mesh = go.GetComponent<MeshRenderer>();
            if (mesh == nullptr) continue;
            const math::Vector4 savedStart = trail->colorStart;
            const math::Vector4 savedEnd = trail->colorEnd;
            const std::size_t particleCount = (std::min)(emitter->particles.size(),
                static_cast<std::size_t>((std::max)(emitter->visibleParticleCount, 0)));
            for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
                const Particle& particle = emitter->particles[particleIndex];
                math::Vector3 position = particle.position;
                if (emitter->simulationSpace == ParticleSimulationSpace::Local) {
                    const math::Vector3 scaled{
        position.x * go.transform.worldScale.x,
        position.y * go.transform.worldScale.y,
        position.z * go.transform.worldScale.z };
    position = go.transform.worldPosition +
        go.transform.worldRotation * scaled;
                }
                MeshTrailSample sample;
                sample.timestamp = currentTime;
                sample.position = position;
                // Mesh Particle は billboard と違い 3 軸すべてを使えるため、
                // sizeAxisScale の z も反映する (billboard 経路は xy のみ)。
                sample.world = math::Matrix4::TRS(position,
                    math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, particle.rotation),
                    { particle.size * emitter->sizeAxisScale.x,
                      particle.size * emitter->sizeAxisScale.y,
                      particle.size * emitter->sizeAxisScale.z });
                // Particle::color はリニア、MeshTrail の色はオーサリング空間 (sRGB) と
                // 規約が違う。境界のここで戻して、MeshTrail 側の扱いは一切変えない。
                const math::Vector4 authored = ParticleLinearToSrgb(particle.color);
                trail->colorStart = authored;
                trail->colorEnd = authored;
                DrawStaticMeshSample(*trail, sample, *mesh, ctx, currentTime);
            }
            trail->colorStart = savedStart;
            trail->colorEnd = savedEnd;
            continue;
        }
        ExpireSamples(*trail, resources, currentTime);

        if (trail->enabled && ShouldSample(
            *trail, go.transform.worldPosition, currentTime))
            CaptureSample(go, *trail, resources, currentTime);

        ExpireSamples(*trail, resources, currentTime);

        for (int i = 0; i < trail->sampleCount; ++i) {
            auto& sample = SampleAt(*trail, i);
            if (auto* mr = go.GetComponent<MeshRenderer>())
                DrawStaticMeshSample(*trail, sample, *mr, ctx, currentTime);
            if (auto* smr = go.GetComponent<SkinnedMeshRenderer>())
                DrawSkinnedMeshSample(*trail, sample, *smr, ctx, currentTime);
        }
    }
}

} // namespace fbzz::scene
