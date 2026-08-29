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
#include <Engine/Scene/Components/ParticleForceField.hpp>
#include <Engine/Scene/Components/WindZoneComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <algorithm>

namespace fbzz::scene {
namespace {

float NextRandom01(ParticleEmitter& emitter)
{
    if (emitter.runtime.randomState == 0) emitter.runtime.randomState = emitter.settings.randomSeed != 0 ? emitter.settings.randomSeed : 1;
    emitter.runtime.randomState = emitter.runtime.randomState * 1664525u + 1013904223u;
    return static_cast<float>(emitter.runtime.randomState) * (1.0f / 4294967295.0f);
}

} // namespace

ComponentAccess ParticleSimulationSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<Transform>()
        .Reads<AnimatorComponent>()
        .Reads<ParticleForceField>()
        .Reads<WindZoneComponent>()
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

        if (emitter->settings.pauseWhenCulled && emitter->runtime.isCulledThisFrame) continue;
        if (emitter->runtime.lastPlaybackFrame == Time::frameCount) continue;
        emitter->runtime.lastPlaybackFrame = Time::frameCount;
        // 黒体モードの焼き込みはここで 1 フレームに 1 回だけ行う。CPU 更新も GPU 定数バッファも
        // この後の runtimeGradient を読むため、両経路の色が原理的にずれない。
        emitter->RefreshRuntimeGradient();

        // VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速)。ゲーム実行時は常に 1.0。
        const float dt = ctx.dt * emitter->GetEditorTimeScale(Time::frameCount);

        bool canEmit = emitter->settings.playing;
        if (canEmit && emitter->runtime.delayTime < emitter->settings.startDelay) {
            emitter->runtime.delayTime += dt;
            canEmit = false;
        }
        // WHY duration の有無に関わらず進めるか: playTime は «再生開始からの経過» であって
        //     «duration の中での位置» ではない。時刻指定 Burst と GetPlayTime() が読むため、
        //     duration = 0 (無期限) のときに止めてしまうと Burst が永久に発火しない。
        if (canEmit) emitter->runtime.playTime += dt;
        if (canEmit && emitter->settings.duration > 0.0f
            && emitter->runtime.playTime >= emitter->settings.duration) {
            if (emitter->settings.loop) {
                emitter->runtime.playTime = 0.0f;
                emitter->runtime.delayTime = 0.0f;
                emitter->runtime.burstCyclesFired.clear();
            } else {
                emitter->settings.playing = false;
                canEmit = false;
                if (emitter->settings.clearOnStop) {
                    emitter->runtime.particles.clear();
                    emitter->runtime.gpuClearPending = true;
                }
            }
        }
        emitter->runtime.emitThisFrame = canEmit;

        const math::Vector3 currentPosition = gameObject->transform.worldPosition;
        // エミッター自身の移動速度。inheritVelocity がスポーン初速へ足すために使う。
        // dt が 0 (完全停止・スクラブ中) のときは前回値を保つ (0 除算と速度の消失を避ける)。
        if (emitter->runtime.hasLastEmitterPosition && dt > 1.0e-6f)
            emitter->runtime.emitterVelocity = (currentPosition - emitter->runtime.lastEmitterPosition) * (1.0f / dt);
        if (emitter->runtime.hasLastEmitterPosition && canEmit && emitter->settings.rateOverDistance > 0.0f) {
            emitter->runtime.distanceEmitAccum += (currentPosition - emitter->runtime.lastEmitterPosition).Length()
                * emitter->settings.rateOverDistance;
            const int count = static_cast<int>(emitter->runtime.distanceEmitAccum);
            emitter->runtime.burstPending += count;
            emitter->runtime.distanceEmitAccum -= static_cast<float>(count);
        }
        emitter->runtime.lastEmitterPosition = currentPosition;
        emitter->runtime.hasLastEmitterPosition = true;

        if (emitter->runtime.burstCyclesFired.size() != emitter->settings.bursts.size())
            emitter->runtime.burstCyclesFired.assign(emitter->settings.bursts.size(), 0);
        for (size_t index = 0; index < emitter->settings.bursts.size(); ++index) {
            const ParticleBurst& burst = emitter->settings.bursts[index];
            int& fired = emitter->runtime.burstCyclesFired[index];
            const int cycles = (std::max)(burst.cycles, 1);
            while (fired < cycles
                   && emitter->runtime.playTime >= burst.time + burst.interval * static_cast<float>(fired)) {
                if (NextRandom01(*emitter) <= (std::max)(0.0f, (std::min)(burst.probability, 1.0f)))
                    emitter->runtime.burstPending += (std::max)(burst.count, 0);
                ++fired;
            }
        }

        if (emitter->settings.prewarm && emitter->settings.loop && !emitter->runtime.prewarmed) {
            const float warmDuration = emitter->settings.duration > 0.0f ? emitter->settings.duration : emitter->settings.lifetime;
            const int warmCount = (std::min)(
                static_cast<int>((std::max)(emitter->settings.emitRate, 0.0f) * (std::max)(warmDuration, 0.0f)),
                (std::max)(emitter->settings.maxParticles, 0));
            emitter->runtime.burstPending += warmCount;
            emitter->runtime.prewarmSpawnPending += warmCount;
            emitter->runtime.prewarmed = true;
        }
    }

    // 描画されないScene Viewや非表示Viewportでも寿命・衝突を進めるため、CPU更新はScheduler側で完結させる。
    UpdateParticleCpuSimulation(ctx.scene, ctx.world, ctx.dt, Time::time);
}

} // namespace fbzz::scene
