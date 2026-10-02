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
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>

#include <memory>
#include <vector>

namespace fbzz::scene {

namespace {

/// @note resources == nullptr は «返さずに手放す» (複製先のように、そもそも所有していない場合)。
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

/// @note 以下の Drop は «もう一度確保し直せる状態» まで戻す。
/// @note 各システムは «確保済みか» を容量やモデルポインタで判定するため、ハンドルだけ空にすると
/// @note «確保済みのつもりで無効ハンドルを使う» 状態が残る。

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
    /// @note 帯の頂点は TrailRenderPass のプールから借りているので、Component には持っていない。
    Drop(resources, trail.trailCB);
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
    /// @note 借りているだけなので返さない (実体は ParticlePass が .mat 単位で 1 本持つ)。
    /// @note 返すと、同じ .mat を使う他のエミッターの b2 まで一緒に死ぬ。
    runtime.materialParamsCB = {};
    Drop(resources, runtime.gpuParticleBuffer);
    /// @note スポーン / 力場は ParticlePass のプールから借りているだけなので返さない。
    runtime.gpuSpawnBuffer = {};
    runtime.gpuForceBuffer = {};
    Drop(resources, runtime.gpuEmitterCB);
    Drop(resources, runtime.renderCB);
    Drop(resources, runtime.trailRibbonCB);
    Drop(resources, runtime.gpuSortBuffer);
    Drop(resources, runtime.gpuSortCB);
    runtime.gpuSortCapacity = 0;
    runtime.gpuCapacity = 0;
    runtime.gpuInitialized = false;
}

/// @note 2 枚のメッシュは «この Component 専用» で、他に持ち主が居ない。
/// @note 次に焼くときは容量から取り直すため空にまで戻す。ハンドルだけ空にすると vertexCapacity が
/// @note «確保済み» のまま残り、無効ハンドルへ書きにいく。
void Drop(DoubleBufferedMesh& target, renderer::ResourceManager* resources)
{
    for (std::unique_ptr<renderer::Mesh>& slot : target.slots) {
        if (!slot) continue;
        if (resources != nullptr)
            static_cast<void>(resources->ReleaseMeshBuffers(*slot));
        slot.reset();
    }
    target.signature = 0;
    target.current = 0;
}

void Drop(SpriteRendererComponent& sprite, renderer::ResourceManager* resources)
{
    Drop(sprite.runtimeMesh, resources);
}

void Drop(LineRendererComponent& line, renderer::ResourceManager* resources)
{
    Drop(line.runtimeMesh, resources);
}

void Drop(ProceduralMeshComponent& procedural, renderer::ResourceManager* resources)
{
    Drop(procedural.runtimeMesh, resources);
    /// @note 次のフレームで焼き直させる。dirty を戻さないと «空のメッシュのまま» になる。
    procedural.dirty = MeshDirty::All;
}

void Drop(ClothComponent& cloth, renderer::ResourceManager* resources)
{
    /// @note MeshRenderer は非所有参照なので、布のバッファを破棄する前に参照を切る。
    if (cloth.runtime.ownerScene) {
        auto* mesh = cloth.runtime.ownerScene->GetComponent<MeshRenderer>(cloth.runtime.owner);
        if (mesh) {
            for (const auto& slot : cloth.runtimeMesh.slots) {
                if (slot && mesh->mesh == slot.get()) {
                    mesh->mesh = nullptr;
                    mesh->enabled = false;
                }
            }
        }
    }
    Drop(cloth.runtimeMesh, resources);
    cloth.runtime.initialized = false;
}

void Drop(ReflectionProbeComponent& probe, renderer::ResourceManager* resources)
{
    Drop(resources, probe.runtimeCubeRT);
    Drop(resources, probe.runtimeIrradiance);
    Drop(resources, probe.runtimePrefilter);
    probe.runtimeResolution = 0;
    probe.runtimePrefilterMipCount = 0;
    probe.runtimePublicationOwner = nullptr;
    probe.runtimePublishedIrradiance = {};
    probe.runtimePublishedPrefilter = {};
    probe.runtimePublicationEpoch = 0;
    probe.runtimeImmutablePublished = false;
}

void Drop(LightProbeVolumeComponent& volume, renderer::ResourceManager* resources)
{
    Drop(resources, volume.runtimeVolume);
    Drop(resources, volume.runtimeRawVolume);
    for (auto& face : volume.runtimeFaces)
        Drop(resources, face);
    for (auto& face : volume.runtimeFacing)
        Drop(resources, face);
    volume.runtimeSettingsKey = 0;
    volume.runtimeGrid = {};
    volume.runtimeBoxMin = {};
    volume.runtimeBoxSize = {};
    volume.runtimeFaceSize = 0;
    volume.runtimeCursor = 0;
    volume.runtimePass = 0;
    volume.runtimeBaking = false;
    volume.runtimeReady = false;
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
    DropAll<LightProbeVolumeComponent>(scene, &resources);
    DropAll<SpriteRendererComponent>(scene, &resources);
    DropAll<LineRendererComponent>(scene, &resources);
    DropAll<ProceduralMeshComponent>(scene, &resources);
    DropAll<ClothComponent>(scene, &resources);
}

void ReleaseEntityOwnedGpuResources(Scene& scene, EntityID id, renderer::ResourceManager& resources)
{
    DropOne<AnimatorComponent>(scene, id, &resources);
    DropOne<SkinnedMeshRenderer>(scene, id, &resources);
    DropOne<TrailComponent>(scene, id, &resources);
    DropOne<MeshTrailComponent>(scene, id, &resources);
    DropOne<ParticleEmitter>(scene, id, &resources);
    DropOne<ReflectionProbeComponent>(scene, id, &resources);
    DropOne<LightProbeVolumeComponent>(scene, id, &resources);
    DropOne<SpriteRendererComponent>(scene, id, &resources);
    DropOne<LineRendererComponent>(scene, id, &resources);
    DropOne<ProceduralMeshComponent>(scene, id, &resources);
    DropOne<ClothComponent>(scene, id, &resources);
}

void ClearDuplicatedGpuHandles(Scene& scene, EntityID id)
{
    DropOne<AnimatorComponent>(scene, id, nullptr);
    DropOne<SkinnedMeshRenderer>(scene, id, nullptr);
    DropOne<TrailComponent>(scene, id, nullptr);
    DropOne<MeshTrailComponent>(scene, id, nullptr);
    DropOne<ParticleEmitter>(scene, id, nullptr);
    DropOne<ReflectionProbeComponent>(scene, id, nullptr);
    DropOne<LightProbeVolumeComponent>(scene, id, nullptr);
    DropOne<SpriteRendererComponent>(scene, id, nullptr);
    DropOne<LineRendererComponent>(scene, id, nullptr);
    DropOne<ProceduralMeshComponent>(scene, id, nullptr);
    DropOne<ClothComponent>(scene, id, nullptr);
}

/// @note Scene::RemoveComponent<T>() から呼ばれる 1 型ぶんの返却。
/// @note 呼び出し側 (Scene.hpp のテンプレート) に Renderer を持ち込まないよう Active() を使う。
namespace {
template <class Component>
void DropToActive(Component& component)
{
    Drop(component, renderer::ResourceManager::Active());
}
} // namespace

void ClearComponentGpuHandles(AnimatorComponent& component)        { Drop(component, nullptr); }
void ClearComponentGpuHandles(SkinnedMeshRenderer& component)      { Drop(component, nullptr); }
void ClearComponentGpuHandles(TrailComponent& component)           { Drop(component, nullptr); }
void ClearComponentGpuHandles(MeshTrailComponent& component)       { Drop(component, nullptr); }
void ClearComponentGpuHandles(ParticleEmitter& component)          { Drop(component, nullptr); }
void ClearComponentGpuHandles(ReflectionProbeComponent& component) { Drop(component, nullptr); }
void ClearComponentGpuHandles(LightProbeVolumeComponent& component) { Drop(component, nullptr); }
void ClearComponentGpuHandles(SpriteRendererComponent& component)  { Drop(component, nullptr); }
void ClearComponentGpuHandles(LineRendererComponent& component)    { Drop(component, nullptr); }
void ClearComponentGpuHandles(ProceduralMeshComponent& component)  { Drop(component, nullptr); }
void ClearComponentGpuHandles(ClothComponent& component)           { Drop(component, nullptr); }

void ReleaseComponentGpuResources(AnimatorComponent& component)        { DropToActive(component); }
void ReleaseComponentGpuResources(SkinnedMeshRenderer& component)      { DropToActive(component); }
void ReleaseComponentGpuResources(TrailComponent& component)           { DropToActive(component); }
void ReleaseComponentGpuResources(MeshTrailComponent& component)       { DropToActive(component); }
void ReleaseComponentGpuResources(ParticleEmitter& component)          { DropToActive(component); }
void ReleaseComponentGpuResources(ReflectionProbeComponent& component) { DropToActive(component); }
void ReleaseComponentGpuResources(LightProbeVolumeComponent& component) { DropToActive(component); }
void ReleaseComponentGpuResources(SpriteRendererComponent& component)  { DropToActive(component); }
void ReleaseComponentGpuResources(LineRendererComponent& component)    { DropToActive(component); }
void ReleaseComponentGpuResources(ProceduralMeshComponent& component)  { DropToActive(component); }
void ReleaseComponentGpuResources(ClothComponent& component)           { DropToActive(component); }

} // namespace fbzz::scene
