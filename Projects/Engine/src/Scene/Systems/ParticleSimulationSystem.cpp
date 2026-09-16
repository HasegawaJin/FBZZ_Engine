/// @file    ParticleSimulationSystem.cpp
/// @brief   Particle再生、距離Emission、時刻Burst、Prewarmを描画前に一度だけ評価する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include <Engine/Scene/Systems/ParticleSimulationSystem.hpp>
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>

#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ForceField.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <algorithm>

namespace fbzz::scene {

ComponentAccess ParticleSimulationSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<Transform>()
        .Reads<AnimatorComponent>()
        .Reads<ForceField>()
        .Writes<ParticleEmitter>();
}

OrderingHints ParticleSimulationSystem::GetOrder() const
{
    // MeshSurfaceのスキン姿勢が確定した後、描画パスより前にEmissionを確定する。
    return OrderingHints{}.After<IKSystem>();
}

void ParticleSimulationSystem::Update(SystemContext& ctx)
{
    for (EntityID id : ctx.scene.GetEntities<ParticleEmitter>()) {
        auto* emitter = ctx.scene.GetComponent<ParticleEmitter>(id);
        GameObject* gameObject = ctx.scene.GetGameObject(id);
        if (!emitter || !gameObject || !gameObject->activeInHierarchy() || !emitter->settings.enabled) continue;

        // VFX Editor のタイムラインスクラブ要求。決定論的な再シミュレーションで状態を作り直すため、
        // このフレームの通常再生はスキップする (フレームガードはスクラブ側が立てる)。
        if (emitter->runtime.editorScrubTime >= 0.0f) {
            const float target = emitter->runtime.editorScrubTime;
            emitter->runtime.editorScrubTime = -1.0f;
            ScrubParticleEmitterForEditor(ctx.scene, ctx.world, *gameObject, *emitter, target, Time::time);
            continue;
        }

        if (emitter->settings.culling.pauseWhenCulled && emitter->runtime.isCulledThisFrame) continue;

        // 再生進行の実体は AdvanceParticleEmitterPlayback が持つ (描画パスと共用)。
        // VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速) だけここで dt へ掛ける。
        (void)AdvanceParticleEmitterPlayback(*emitter, gameObject->transform,
                                             ctx.dt * emitter->GetEditorTimeScale(Time::frameCount));
    }

    // 描画されないScene Viewや非表示Viewportでも寿命・衝突を進めるため、CPU更新はScheduler側で完結させる。
    UpdateParticleCpuSimulation(ctx.scene, ctx.world, ctx.dt, Time::time);
}

} // namespace fbzz::scene
