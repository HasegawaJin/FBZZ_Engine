/// @file    RenderParticleExtractor.cpp
/// @brief   描画順と可視性を決め、粒子を CPU または GPU の描画入力へ渡す。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include "RenderParticleExtraction.hpp"
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include "ParticleSimulationHelpers.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Core/Time.hpp>
#include "RenderExtractionPhases.hpp"
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <utility>

namespace fbzz::scene {

namespace {

using namespace particle_extraction_detail;
using namespace particle_simulation_detail;

std::vector<GameObject*> CollectParticleEmitters(const RenderPassContext& context)
{
    /// @note 描画順を renderPriority → カメラ距離 (遠い順) で確定させる。GameObject の並び順の
    /// @note ままだと「炎の手前に煙」が保証されず、シーンを編集しただけで前後が入れ替わる。
    /// @note 距離はバウンズ更新前なので Transform 位置で近似する (順序決定には十分)。
    std::vector<GameObject*> sortedEmitters;
    {
        /// @note 収集中はリソースやゲーム側のコールバックを呼ばず、同順位の要素にはシーンの順序を保つ。
        const auto emitters = context.scene.GetEntities<ParticleEmitter>();
        std::vector<EntityID> candidates(emitters.begin(), emitters.end());
        context.scene.SortEntitiesBySceneOrder(candidates);
        for (EntityID entity : candidates) {
            auto* owner = context.scene.GetGameObject(entity);
            if (!owner) continue;
            auto& candidate = *owner;
            if (!ShouldRenderGameObject(candidate, context.cullingMask)) continue;
            auto* candidateEmitter = candidate.GetComponent<ParticleEmitter>();
            if (candidateEmitter == nullptr || !candidateEmitter->settings.enabled) continue;
            sortedEmitters.push_back(&candidate);
        }
    }
    const math::Vector3 cameraPosition = context.camera.m_position;
    /// @note GetComponent は非 const 版しかないため、比較関数も非 const ポインタで受ける。
    std::stable_sort(sortedEmitters.begin(), sortedEmitters.end(),
        [cameraPosition](GameObject* leftOwner, GameObject* rightOwner) {
            const int leftPriority = leftOwner->GetComponent<ParticleEmitter>()->settings.renderPriority;
            const int rightPriority = rightOwner->GetComponent<ParticleEmitter>()->settings.renderPriority;
            if (leftPriority != rightPriority) return leftPriority < rightPriority;
            const math::Vector3 leftOffset = leftOwner->transform.worldPosition - cameraPosition;
            const math::Vector3 rightOffset = rightOwner->transform.worldPosition - cameraPosition;
            return math::Vector3::Dot(leftOffset, leftOffset) > math::Vector3::Dot(rightOffset, rightOffset);
        });

    return sortedEmitters;
}

bool UpdateParticleVisibility(ParticleEmitter& emitter, const Transform& transform, RenderPassContext& context)
{
    UpdateParticleBounds(emitter, transform);
    const float cameraDistance = (emitter.runtime.boundsCenter - context.camera.m_position).Length();
    const float coverage = emitter.runtime.boundsRadius / (std::max)(cameraDistance, 0.001f);
    const bool frustumVisible = !context.frustumCullingEnabled || !context.cameraFrustum
        || context.cameraFrustum->IntersectsSphere(emitter.runtime.boundsCenter, emitter.runtime.boundsRadius);
    const bool coverageVisible = emitter.settings.culling.screenCoverageThreshold <= 0.0f
        || coverage >= emitter.settings.culling.screenCoverageThreshold;
    emitter.runtime.isCulledThisFrame = emitter.settings.culling.cullingEnabled && (!frustumVisible || !coverageVisible);
    if (emitter.runtime.isCulledThisFrame) {
        ++context.statsParticleCulled;
        emitter.runtime.visibleParticleCount = 0;
        return false;
    }
    if (emitter.settings.culling.lodEnabled && emitter.settings.culling.lodFarDistance > emitter.settings.culling.lodNearDistance) {
        const float alpha = Clamp01((cameraDistance - emitter.settings.culling.lodNearDistance)
            / (emitter.settings.culling.lodFarDistance - emitter.settings.culling.lodNearDistance));
        emitter.runtime.lodRateScale = emitter.settings.culling.lodNearRateScale
            + (emitter.settings.culling.lodFarRateScale - emitter.settings.culling.lodNearRateScale) * alpha;
    } else {
        emitter.runtime.lodRateScale = 1.0f;
    }
    return true;
}

void PublishParticleRenderResources(ParticleEmitter& emitter, renderer::ResourceManager& resources,
    renderer::RenderParticleInput& input)
{
    if (emitter.settings.trail.trailRibbon && !resources.Get(emitter.runtime.trailRibbonCB)) {
        renderer::ReleaseOwnedResource(emitter.runtime.trailRibbonCB);
        emitter.runtime.trailRibbonCB = resources.CreateConstantBuffer(sizeof(TrailCB));
    }
    input.runtime.texture = emitter.runtime.texture;
    input.runtime.distortionTexture = emitter.runtime.distortionTexture;
    input.runtime.motionVectorTexture = emitter.runtime.motionVectorTexture;
    input.runtime.sixWayNegativeTexture = emitter.runtime.sixWayNegativeTexture;
    input.runtime.sixWayAlbedoColorTexture = emitter.runtime.sixWayAlbedoColorTexture;
    input.runtime.sixWayEmissionColorTexture = emitter.runtime.sixWayEmissionColorTexture;
    input.runtime.customShader = emitter.runtime.customShader;
    input.runtime.renderCB = emitter.runtime.renderCB;
    input.runtime.materialParamsCB = emitter.runtime.materialParamsCB;
    input.runtime.trailRibbonCB = emitter.runtime.trailRibbonCB;
    input.runtime.gpuSortCB = emitter.runtime.gpuSortCB;
    input.runtime.gpuSortBuffer = emitter.runtime.gpuSortBuffer;
    input.runtime.gpuSortCapacity = emitter.runtime.gpuSortCapacity;
    input.runtime.resolvedBlend = emitter.runtime.resolvedBlend;
    input.runtime.textureIsSrgb = emitter.runtime.textureIsSrgb;
    input.runtime.material.distortion = emitter.runtime.material.distortion;
    input.runtime.material.punctualLighting = emitter.runtime.material.punctualLighting;
    input.runtime.material.selfShadowStrength = emitter.runtime.material.selfShadowStrength;
}

}

void ExtractRayParticleSources(const Scene& scene, renderer::RenderScene& output,
    bool experimentalRayTracingEnabled)
{
    if (!experimentalRayTracingEnabled) return;
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        auto* ownerObject = scene.GetGameObject(id);
        const auto* emitter = ownerObject ? ownerObject->GetComponent<ParticleEmitter>() : nullptr;
        if (!ownerObject || !ownerObject->activeInHierarchy() || !emitter || !emitter->settings.enabled) continue;
        const bool usesGpuSimulation = CanUseGpuSimulation(emitter->settings, &emitter->runtime.material);
        /// @note CPU メッシュ粒子の除外と個数ゼロの扱いをラスタライズと揃える。カメラ、LOD、予算、可視粒子数は発生源の有無を示さない。
        if (usesGpuSimulation ? emitter->settings.maxParticles <= 0
                : !emitter->settings.meshParticlePath.empty() || emitter->runtime.particles.empty()) continue;
        const uint32_t layerMask = ownerObject->layer >= 0 && ownerObject->layer < 32 ? uint32_t{1} << ownerObject->layer : 0;
        output.rayUnsupportedEffects.push_back({id.index, id.generation, layerMask,
            renderer::RenderRayUnsupportedEffect::PARTICLE});
    }
}

void ExtractRenderParticles(RenderPassContext& context, renderer::RenderScene& output)
{
    auto& resources = context.resources;
    auto& handles = context.handles;
    auto& cache = BeginParticleMaterialPass(context.scene, resources);
    if (!handles.particleShader.IsValid() || !handles.particleIB.IsValid()) return;
    const float deltaTime = Time::deltaTime, time = Time::time;
    const auto flowSnapshot = context.scene.FlowFrame().fields;
    const std::vector<ActiveFlowField>& flowFields = *flowSnapshot;
    /// @note 設定値が正の場合だけ予算を適用する。残数がゼロになった後も制限は有効。
    const bool budgetLimited = context.settings.particleBudgetEnabled && context.settings.particleBudget > 0;
    int remainingBudget = context.settings.particleBudget;

    detail::RenderExtractionPhases phases("RenderSceneExtractor::Particles");
    phases.Begin("RenderSceneExtractor::Particles::CollectAndSort");
    const auto sortedEmitters = CollectParticleEmitters(context);

    phases.Begin("RenderSceneExtractor::Particles::EmitterInputsAndResources");
    for (GameObject* emitterObject : sortedEmitters) {
        GameObject& ownerObject = *emitterObject;
        auto* emitter = ownerObject.GetComponent<ParticleEmitter>();
        const auto* animator = FindParticleAnimator(ownerObject);
        const Transform& transform = ownerObject.transform;
        ++context.statsParticleEmitters;

        /// @note VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速)。ゲーム実行時は常に 1.0。
        const float emitterDeltaTime = deltaTime * emitter->GetEditorTimeScale(Time::frameCount);

        if (!UpdateParticleVisibility(*emitter, transform, context)) continue;
        PrepareParticleAppearance(*emitter, resources, cache);
        /// @note Physics Query、Local Space、厳密な透過ソートはCPU側で解決し、見た目の正しさを優先する。
        const bool isGpuMode = CanUseGpuSimulation(emitter->settings, &emitter->runtime.material);

        const bool canEmit = emitter->runtime.emitThisFrame;

        renderer::RenderParticleInput input;
        input.layer = static_cast<uint32_t>(ownerObject.layer); input.position = transform.worldPosition;
        input.boundsCenter = emitter->runtime.boundsCenter;
        input.boundsRadius = emitter->runtime.boundsRadius;
        input.priority = emitter->settings.renderPriority; input.gpu = isGpuMode;
        input.settings.sortMode = emitter->settings.sortMode;
        input.settings.trail = emitter->settings.trail;
        input.settings.colorStart = emitter->settings.colorStart; input.settings.colorEnd = emitter->settings.colorEnd;
        input.settings.meshParticle = !emitter->settings.meshParticlePath.empty();
        input.settings.maxParticles = emitter->settings.maxParticles;
        if (!isGpuMode && input.settings.meshParticle) {
            emitter->runtime.visibleParticleCount = 0;
            continue;
        }
        const auto availableCount = isGpuMode
            ? static_cast<std::size_t>((std::max)(input.settings.maxParticles, 0)) : emitter->runtime.particles.size();
        const auto drawCapacity = isGpuMode
            ? availableCount : (std::min)(availableCount, static_cast<std::size_t>(kMaxParticleDraw));
        const int drawCount = static_cast<int>(budgetLimited
            ? (std::min)(drawCapacity, static_cast<std::size_t>(remainingBudget)) : drawCapacity);
        input.drawCount = drawCount;
        emitter->runtime.visibleParticleCount = drawCount;
        if (budgetLimited) remainingBudget -= drawCount;
        if (availableCount > static_cast<std::size_t>(drawCount)) ++context.statsParticleBudgetDropped;
        context.statsParticleVisible += drawCount;

        if (isGpuMode) {
            if (availableCount == 0) continue;
            /// @note ソート後の参照先は GPU プール全域に及ぶ。定数の上限は描画数と分ける。
            if (drawCount > 0)
                input.constants = BuildParticleRenderConstants(*emitter, resources, input.settings.maxParticles,
                    context.width, context.height, ShouldSortGpuParticles(*emitter, handles));
            const auto* meshRenderer = input.settings.meshParticle && drawCount > 0
                ? ownerObject.GetComponent<MeshRenderer>() : nullptr;
            PrepareGpuEmitter(*emitter, transform, animator, emitterDeltaTime, time, canEmit, flowFields, meshRenderer, context, input);
            if (drawCount == 0 && !input.simulate) continue;
        } else {
            if (drawCount == 0) continue;
            input.constants = BuildParticleRenderConstants(*emitter, resources, drawCount, context.width, context.height);
            /// @note ソートする入力は別カメラでの再評価に全候補が必要。入力順の描画は予算分だけ保持する。
            const auto& particles = emitter->runtime.particles;
            const auto copyCount = emitter->settings.sortMode == ParticleSortMode::BackToFront
                ? particles.size() : static_cast<std::size_t>(drawCount);
            input.runtime.particles.assign(particles.begin(), particles.begin() + copyCount);
            if (emitter->settings.simulationSpace == ParticleSimulationSpace::Local) {
                for (auto& particle : input.runtime.particles) {
                    particle.position = TransformEmitterPoint(transform, particle.position);
                    particle.velocity = TransformEmitterVector(transform, particle.velocity);
                    const auto historyCount = (std::min)(particle.trailPoints.size(),
                        static_cast<std::size_t>(particle.trailCount));
                    for (std::size_t index = 0; index < historyCount; ++index)
                        particle.trailPoints[index] = TransformEmitterPoint(transform, particle.trailPoints[index]);
                }
            }
        }
        if (drawCount > 0) PublishParticleRenderResources(*emitter, resources, input);
        output.particles.push_back(std::move(input));
    }
    phases.End();
    ExtractRayParticleSources(context.scene, output, context.experimentalRayTracingEnabled);
}

}
