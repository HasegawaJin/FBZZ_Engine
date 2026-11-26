/// @file    RenderMeshTrailExtractor.cpp
/// @brief   残像サンプルから静的・スキン付き・粒子メッシュの描画入力を発行する。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include "RenderMeshTrailExtraction.hpp"
#include <Engine/Rendering/Extraction/Effects/RenderMeshTrailExtractor.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include "../../Passes/Geometry/GeometryPasses.hpp"
#include <Engine/Asset/Model/Model.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Engine/Scene/Components/Geometry/MeshRenderer.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/Particles/ParticleEmitter.hpp>
#include <Engine/Scene/Components/Particles/ParticleColorSpace.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace fbzz::scene {

namespace {

using namespace mesh_trail_extraction_detail;

/// @note SkinningCB — Common/Constants.hlsli の SkinningConstants と同じレイアウト。
struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

bool IsMeshIndexExcluded(const MeshTrailComponent& trail, int meshIndex)
{
    return std::find(trail.excludedMeshIndices.begin(), trail.excludedMeshIndices.end(), meshIndex)
        != trail.excludedMeshIndices.end();
}

renderer::ResourceHandle<renderer::ConstantBufferTag> EnsureSampleSkinningCB(
    MeshTrailSample& sample,
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseCB)
{
    if (sample.boneMatrices.empty()) {
        return bindPoseCB;
    }

    if (!resources.Get(sample.skinningCB)) {
        sample.skinningCB.Reset();
        sample.skinningCB = resources.CreateConstantBuffer(sizeof(SkinningCB));
        sample.skinningCBDirty = true;
    }

    if (!sample.skinningCBDirty) {
        return sample.skinningCB;
    }

    SkinningCB constants{};
    for (int sampleIndex = 0; sampleIndex < asset::MAX_SKINNING_BONES; ++sampleIndex) {
        constants.boneMatrices[sampleIndex] = math::Matrix4::Identity();
    }
    for (size_t sampleIndex = 0; sampleIndex < sample.boneMatrices.size() && sampleIndex < asset::MAX_SKINNING_BONES; ++sampleIndex) {
        constants.boneMatrices[sampleIndex] = sample.boneMatrices[sampleIndex];
    }
    resources.Update(sample.skinningCB, &constants, sizeof(constants));
    sample.skinningCBDirty = false;
    return sample.skinningCB;
}

math::Vector4 SampleColor(const MeshTrailComponent& trail, const MeshTrailSample& sample, float currentTime)
{
    const float duration = (std::max)(trail.duration, 0.01f);
    const float newestWeight = math::Clamp01(1.0f - (currentTime - sample.timestamp) / duration);
    return trail.colorEnd + (trail.colorStart - trail.colorEnd) * newestWeight;
}
/// @note 正規化された非負の skin weight の結果は、保存した各骨で変換した元境界の凸包内にある。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/mul Linear blend skinning uses weighted affine positions.
void HistoricalSkinBounds(const MeshTrailSample& sample, const renderer::Mesh& mesh, renderer::RenderMeshTrailInput& input)
{
    if (input.shader.IsValid() || !mesh.morphTargets.empty() || mesh.boundsRadius <= 0) {
        return;
    }
    math::Vector3 low{}, high{};
    bool first = true;
    const auto include = [&](const math::Matrix4& bone) {
        const auto world = sample.world * bone;
        const auto center = world * math::Vector4{mesh.boundsCenter, 1};
        const auto extent = [&](int row) {
            return mesh.boundsRadius * (std::abs(world.m[row][0]) + std::abs(world.m[row][1]) + std::abs(world.m[row][2]));
        };
        const math::Vector3 origin{center.x, center.y, center.z}, extents{extent(0), extent(1), extent(2)};
        const auto a = origin - extents, b = origin + extents;
        if (first) { low = a; high = b; first = false; }
        else {
            low = {std::min(low.x, a.x), std::min(low.y, a.y), std::min(low.z, a.z)};
            high = {std::max(high.x, b.x), std::max(high.y, b.y), std::max(high.z, b.z)};
        }
    };
    include(math::Matrix4::Identity());
    for (const auto& bone : sample.boneMatrices) {
        include(bone);
    }
    input.boundsCenter = (low + high) * 0.5f;
    input.boundsRadius = (high - low).Length() * 0.5f;
}
void ExtractStaticSample(MeshTrailComponent& trail, MeshTrailSample& sample, MeshRenderer& meshRenderer,
    RenderPassContext& context, float currentTime, const TrailMaterialBinding* material,
    renderer::RenderScene& output, uint32_t layer) {
    if (!meshRenderer.enabled || !meshRenderer.lodVisible || !meshRenderer.mesh || meshRenderer.mesh->isSkinned || !meshRenderer.mesh->vertexBuffer.IsValid() || !meshRenderer.mesh->indexBuffer.IsValid()) {
        return;
    }
    renderer::RenderMeshTrailInput input;
    input.layer = layer;
    input.object.world = sample.world;
    input.object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(sample.world);
    input.color = SampleColor(trail, sample, currentTime);
    input.vertices = meshRenderer.mesh->vertexBuffer; input.indices = meshRenderer.mesh->indexBuffer;
    input.indexCount = meshRenderer.mesh->indexCount; input.vertexCount = meshRenderer.mesh->vertexCount;
    ApplyMeshTrailMaterial(material, false, input);
    if (!input.shader.IsValid() && meshRenderer.mesh->morphTargets.empty()) {
        const auto bounds = ComputeWorldBounds(sample.world, ComputeWorldBoundsScale(sample.world), *meshRenderer.mesh);
        input.boundsCenter = bounds.center; input.boundsRadius = bounds.radius;
    }
    input.doubleSided = trail.doubleSided; input.colorBuffer = trail.meshTrailCB; input.texture = trail.texture;
    output.meshTrails.push_back(std::move(input));
}
void ExtractSkinnedSample(MeshTrailComponent& trail, MeshTrailSample& sample, SkinnedMeshRenderer& skinnedRenderer,
    RenderPassContext& context, float currentTime, const TrailMaterialBinding* material,
    renderer::RenderScene& output, uint32_t layer) {
    if (!skinnedRenderer.enabled || !skinnedRenderer.lodVisible || !skinnedRenderer.model) {
        return;
    }
    const auto skin = EnsureSampleSkinningCB(sample, context.resources, context.handles.bindPoseSkinningCB);
    for (size_t meshIndex = 0; meshIndex < skinnedRenderer.SubmeshCount(); ++meshIndex) {
        if (IsMeshIndexExcluded(trail, static_cast<int>(meshIndex))) {
            continue;
        }
        const auto* mesh = skinnedRenderer.SubmeshMesh(meshIndex);
        if (!mesh || !mesh->vertexBuffer.IsValid() || !mesh->indexBuffer.IsValid()) {
            continue;
        }
        renderer::RenderMeshTrailInput input;
        input.layer = layer; input.skinned = true; input.skin = skin;
        input.object.world = sample.world;
        input.object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(sample.world);
        input.color = SampleColor(trail, sample, currentTime);
        input.vertices = skinnedRenderer.ResolveSlotVertexBuffer(meshIndex, mesh->vertexBuffer); input.indices = mesh->indexBuffer;
        input.indexCount = mesh->indexCount; input.vertexCount = mesh->vertexCount;
        ApplyMeshTrailMaterial(material, true, input);
        HistoricalSkinBounds(sample, *mesh, input);
        input.doubleSided = trail.doubleSided; input.colorBuffer = trail.meshTrailCB; input.texture = trail.texture;
        output.meshTrails.push_back(std::move(input));
    }
}

void ExtractMeshParticleSamples(GameObject& owner, MeshTrailComponent& trail, const ParticleEmitter& emitter,
    RenderPassContext& context, float currentTime, const TrailMaterialBinding* material, renderer::RenderScene& output)
{
    auto* mesh = owner.GetComponent<MeshRenderer>();
    if (mesh == nullptr) {
        return;
    }
    const math::Vector4 savedStart = trail.colorStart;
    const math::Vector4 savedEnd = trail.colorEnd;
    const std::size_t particleCount = (std::min)(emitter.runtime.particles.size(),
        static_cast<std::size_t>((std::max)(emitter.runtime.visibleParticleCount, 0)));
    for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
        const Particle& particle = emitter.runtime.particles[particleIndex];
        math::Vector3 position = particle.position;
        if (emitter.settings.simulationSpace == ParticleSimulationSpace::Local) {
            const math::Vector3 scaled{
                position.x * owner.transform.worldScale.x,
                position.y * owner.transform.worldScale.y,
                position.z * owner.transform.worldScale.z};
            position = owner.transform.worldPosition + owner.transform.worldRotation * scaled;
        }
        MeshTrailSample sample;
        sample.timestamp = currentTime;
        sample.position = position;
        // Mesh Particle は billboard と違い 3 軸すべてを使えるため、
        // sizeAxisScale の z も反映する (billboard 経路は xy のみ)。
        sample.world = math::Matrix4::TRS(position,
            math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, particle.rotation),
            { particle.size * emitter.settings.sizeAxisScale.x,
              particle.size * emitter.settings.sizeAxisScale.y,
              particle.size * emitter.settings.sizeAxisScale.z });
        // Particle::color はリニア、MeshTrail の色はオーサリング空間 (sRGB) と
        // 規約が違う。境界のここで戻して、MeshTrail 側の扱いは一切変えない。
        const math::Vector4 authored = ParticleLinearToSrgb(particle.color);
        trail.colorStart = authored;
        trail.colorEnd = authored;
        ExtractStaticSample(trail, sample, *mesh, context, currentTime, material, output, static_cast<uint32_t>(owner.layer));
    }
    trail.colorStart = savedStart;
    trail.colorEnd = savedEnd;
}

} // namespace

void ExtractRenderMeshTrails(RenderPassContext& context, renderer::RenderScene& output)
{
    auto& resources = context.resources;
    auto& cache = BeginMeshTrailMaterialPass(context.scene, resources);

    const float currentTime = Time::time;
    if (context.scene.GetEntities<MeshTrailComponent>().empty()) {
        return;
    }

    for (auto& owner : context.scene.GameObjects()) {
        if (!owner.activeInHierarchy()) {
            continue;
        }

        auto* trail = owner.GetComponent<MeshTrailComponent>();
        if (!trail) {
            continue;
        }

        if (!trail->enabled && trail->clearOnDisable) {
            ClearMeshTrailSamples(*trail);
            trail->clearRequested = false;
            continue;
        }

        if (trail->clearRequested) {
            ClearMeshTrailSamples(*trail);
            trail->clearRequested = false;
        }

        const bool hasStaticMesh = owner.GetComponent<MeshRenderer>() != nullptr;
        const bool hasSkinnedMesh = owner.GetComponent<SkinnedMeshRenderer>() != nullptr;
        if (!hasStaticMesh && !hasSkinnedMesh) {
            continue;
        }

        PrepareMeshTrailSampleStorage(*trail);
        PrepareMeshTrailAppearance(*trail, resources);
        const TrailMaterialBinding* material =
            ResolveTrailMaterialBinding(cache, resources, trail->materialPath);
        if (auto* emitter = owner.GetComponent<ParticleEmitter>();
            emitter != nullptr && !emitter->settings.meshParticlePath.empty()) {
            ExtractMeshParticleSamples(owner, *trail, *emitter, context, currentTime, material, output);
            continue;
        }
        UpdateMeshTrailSamples(owner, *trail, currentTime);

        for (int sampleIndex = 0; sampleIndex < trail->sampleCount; ++sampleIndex) {
            auto& sample = ReadMeshTrailSample(*trail, sampleIndex);
            if (auto* meshRenderer = owner.GetComponent<MeshRenderer>()) {
                ExtractStaticSample(*trail, sample, *meshRenderer, context, currentTime, material, output, static_cast<uint32_t>(owner.layer));
            }
            if (auto* skinnedRenderer = owner.GetComponent<SkinnedMeshRenderer>()) {
                ExtractSkinnedSample(*trail, sample, *skinnedRenderer, context, currentTime, material, output, static_cast<uint32_t>(owner.layer));
            }
        }
    }
}

} // namespace fbzz::scene
