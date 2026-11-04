/// @file    SceneRenderResources.cpp
/// @brief   登録済み Component の GPU 実行状態と Scene 描画キャッシュを終了する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Engine/Scene/SceneRenderResources.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Engine/Scene/Systems/RenderMeshTrailExtractor.hpp>
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>

namespace fbzz::scene {
void ReleaseParticleSimulationGpuResources(ParticleEmitter& component)
{
    auto& runtime = component.runtime;
    runtime.gpuParticleBuffer.Reset();
    runtime.gpuEmitterCB.Reset();
    runtime.gpuSortBuffer.Reset();
    runtime.gpuSortCB.Reset();
    runtime.gpuSpawnBuffer = {};
    runtime.gpuForceBuffer = {};
    runtime.gpuSortCapacity = 0;
    runtime.gpuCapacity = 0;
    runtime.gpuInitialized = false;
    runtime.gpuWriteHead = 0;
    runtime.gpuSpawnCount = 0;
    runtime.lastGpuSimulationFrame = UINT64_MAX;
}
void UnlinkRuntimeMesh(Scene& scene, EntityID id, const DoubleBufferedMesh& target)
{
    auto* renderer = scene.GetComponent<MeshRenderer>(id);
    if (!renderer) return;
    for (const auto& slot : target.slots)
        if (slot && renderer->mesh == slot.get()) renderer->mesh = nullptr;
}
void ReleaseComponentGpuResources(Scene& scene, EntityID id, TerrainComponent&)
{ ReleaseTerrainRenderResources(scene, id); }
void ReleaseComponentGpuResources(Scene& scene, EntityID id, WaterComponent&)
{ ReleaseWaterRenderResources(scene, id); }

void ReleaseSceneOwnedGpuResources(Scene& scene)
{
    ReleaseParticleRenderResources(scene);
    ReleaseMeshTrailRenderResources(scene);
    ReleaseWaterRenderResources(scene);
    ReleaseTerrainRenderResources(scene);
    ForEachRegisteredComponent([&]<class T, class Registration>() {
        for (const auto id : scene.GetEntities<T>()) {
            auto* component = scene.GetComponent<T>(id);
            if (component) ReleaseComponentGpuResources(scene, id, *component);
        }
    });
}
void ReleaseEntityOwnedGpuResources(Scene& scene, EntityID id)
{
    ForEachRegisteredComponent([&]<class T, class Registration>() {
        if (auto* component = scene.GetComponent<T>(id)) ReleaseComponentGpuResources(scene, id, *component);
    });
}
} /// @note namespace fbzz::scene
