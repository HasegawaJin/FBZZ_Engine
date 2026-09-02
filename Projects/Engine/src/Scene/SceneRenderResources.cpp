/// @file    SceneRenderResources.cpp
/// @brief   シーン / エンティティが所有する GPU リソースの返却と、複製時の切り離し。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Engine/Scene/SceneRenderResources.hpp>

#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>

#include <vector>

namespace fbzz::scene {

namespace {

// resources == nullptr は «返さずに手放す» (複製先のように、そもそも所有していない場合)。
template <class Tag>
void Drop(renderer::ResourceManager* resources, renderer::ResourceHandle<Tag>& handle)
{
    if (resources != nullptr && handle.IsValid())
        resources->Release(handle);
    handle = {};
}

template <class Tag>
void Drop(renderer::ResourceManager* resources,
          std::vector<renderer::ResourceHandle<Tag>>& handles)
{
    for (auto& handle : handles)
        Drop(resources, handle);
    handles.clear();
}

// 以下の Drop は «もう一度確保し直せる状態» まで戻す。
// WHY 付随フィールドまで畳むか: 各システムは «確保済みか» を容量やモデルポインタで判定する。
//     ハンドルだけ空にすると «確保済みのつもりで無効ハンドルを使う» 状態が残る。

void Drop(AnimatorComponent& animator, renderer::ResourceManager* resources)
{
    Drop(resources, animator.skinningBuffer);
    Drop(resources, animator.prevSkinningBuffer);
}

void Drop(SkinnedMeshRenderer& smr, renderer::ResourceManager* resources)
{
    Drop(resources, smr.morphVertexBuffers);
    Drop(resources, smr.skinnedVertexBuffers);
    smr.skinnedBufferModel = nullptr;
    smr.appliedMorphWeights.clear();
    smr.gpuSkinnedThisFrame = false;
}

void Drop(TrailComponent& trail, renderer::ResourceManager* resources)
{
    Drop(resources, trail.vertexBuffer);
    Drop(resources, trail.trailCB);
    trail.allocatedMaxPoints = 0;
    trail.allocatedSmoothSubdivisions = 0;
}

void Drop(MeshTrailComponent& trail, renderer::ResourceManager* resources)
{
    Drop(resources, trail.meshTrailCB);
    for (MeshTrailSample& sample : trail.samples)
        Drop(resources, sample.skinningCB);
}

void Drop(ParticleEmitter& emitter, renderer::ResourceManager* resources)
{
    ParticleRuntime& runtime = emitter.runtime;
    Drop(resources, runtime.materialParamsCB);
    Drop(resources, runtime.gpuParticleBuffer);
    Drop(resources, runtime.gpuSpawnBuffer);
    Drop(resources, runtime.gpuEmitterCB);
    Drop(resources, runtime.renderCB);
    Drop(resources, runtime.trailRibbonCB);
    Drop(resources, runtime.gpuSortBuffer);
    Drop(resources, runtime.gpuSortCB);
    runtime.gpuSortCapacity = 0;
    runtime.gpuCapacity = 0;
    runtime.gpuInitialized = false;
}

void Drop(ReflectionProbeComponent& probe, renderer::ResourceManager* resources)
{
    Drop(resources, probe.runtimeCubeRT);
    Drop(resources, probe.runtimeIrradiance);
    Drop(resources, probe.runtimePrefilter);
    probe.runtimeResolution = 0;
    probe.runtimePrefilterMipCount = 0;
}

template <class Component>
void DropAll(Scene& scene, renderer::ResourceManager* resources)
{
    for (Component* component : scene.GetComponents<Component>())
        Drop(*component, resources);
}

template <class Component>
void DropOne(Scene& scene, EntityID id, renderer::ResourceManager* resources)
{
    if (Component* component = scene.GetComponent<Component>(id))
        Drop(*component, resources);
}

} // namespace

void ReleaseSceneOwnedGpuResources(Scene& scene, renderer::ResourceManager& resources)
{
    DropAll<AnimatorComponent>(scene, &resources);
    DropAll<SkinnedMeshRenderer>(scene, &resources);
    DropAll<TrailComponent>(scene, &resources);
    DropAll<MeshTrailComponent>(scene, &resources);
    DropAll<ParticleEmitter>(scene, &resources);
    DropAll<ReflectionProbeComponent>(scene, &resources);
}

void ReleaseEntityOwnedGpuResources(Scene& scene, EntityID id, renderer::ResourceManager& resources)
{
    DropOne<AnimatorComponent>(scene, id, &resources);
    DropOne<SkinnedMeshRenderer>(scene, id, &resources);
    DropOne<TrailComponent>(scene, id, &resources);
    DropOne<MeshTrailComponent>(scene, id, &resources);
    DropOne<ParticleEmitter>(scene, id, &resources);
    DropOne<ReflectionProbeComponent>(scene, id, &resources);
}

void ClearDuplicatedGpuHandles(Scene& scene, EntityID id)
{
    DropOne<AnimatorComponent>(scene, id, nullptr);
    DropOne<SkinnedMeshRenderer>(scene, id, nullptr);
    DropOne<TrailComponent>(scene, id, nullptr);
    DropOne<MeshTrailComponent>(scene, id, nullptr);
    DropOne<ParticleEmitter>(scene, id, nullptr);
    DropOne<ReflectionProbeComponent>(scene, id, nullptr);
}

} // namespace fbzz::scene
