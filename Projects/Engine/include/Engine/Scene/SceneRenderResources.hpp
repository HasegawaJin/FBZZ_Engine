/// @file    SceneRenderResources.hpp
/// @brief   GPU 実行状態の共通リセットと Scene 所有キャッシュの終了境界。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Entity.hpp>
namespace fbzz::scene {
class Scene;
struct DoubleBufferedMesh;
struct ParticleEmitter;
struct TerrainComponent;
struct WaterComponent;
FBZZ_ENGINE_API void ReleaseSceneOwnedGpuResources(Scene& scene);
FBZZ_ENGINE_API void ReleaseEntityOwnedGpuResources(Scene& scene, EntityID id);
/// @note 再生成するシミュレーション資源だけを返し、描画定数と借用プールは維持する。
FBZZ_ENGINE_API void ReleaseParticleSimulationGpuResources(ParticleEmitter& component);
FBZZ_ENGINE_API void ReleaseComponentGpuResources(Scene& scene, EntityID id, TerrainComponent& component);
FBZZ_ENGINE_API void ReleaseComponentGpuResources(Scene& scene, EntityID id, WaterComponent& component);
/// @note 個体専用メッシュを捨てる前に、同じ個体の MeshRenderer が持つ非所有参照を切る。
FBZZ_ENGINE_API void UnlinkRuntimeMesh(Scene& scene, EntityID id, const DoubleBufferedMesh& mesh);
template<class T>
void ReleaseComponentGpuResources(T& component)
{
    if constexpr (requires { component.ResetGpuRuntime(); }) component.ResetGpuRuntime();
    else if constexpr (requires { component.runtime.ResetGpuRuntime(); }) component.runtime.ResetGpuRuntime();
    else if constexpr (requires { component.runtimeMesh.Reset(); }) component.runtimeMesh.Reset();
}
template<class T>
void ReleaseComponentGpuResources(Scene& scene, EntityID id, T& component)
{
    if constexpr (requires { component.runtimeMesh; }) UnlinkRuntimeMesh(scene, id, component.runtimeMesh);
    ReleaseComponentGpuResources(component);
}
} /// @note namespace fbzz::scene
