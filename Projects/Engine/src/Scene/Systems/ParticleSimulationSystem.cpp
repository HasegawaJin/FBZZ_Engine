// FBZZ Engine
// ParticleSimulationSystem.cpp | fbzz::scene
// Particle再生、距離Emission、時刻Burst、Prewarmを描画前に一度だけ評価する。
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
    if (emitter.randomState == 0) emitter.randomState = emitter.randomSeed != 0 ? emitter.randomSeed : 1;
    emitter.randomState = emitter.randomState * 1664525u + 1013904223u;
    return static_cast<float>(emitter.randomState) * (1.0f / 4294967295.0f);
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
        if (!emitter || !gameObject || !emitter->enabled) continue;

        // VFX Editor のタイムラインスクラブ要求。決定論的な再シミュレーションで状態を作り直すため、
        // このフレームの通常再生はスキップする (フレームガードはスクラブ側が立てる)。
        if (emitter->editorScrubTime >= 0.0f) {
            const float target = emitter->editorScrubTime;
            emitter->editorScrubTime = -1.0f;
            ScrubParticleEmitterForEditor(ctx.scene, ctx.world, *gameObject, *emitter, target, Time::time);
            continue;
        }

        if (emitter->pauseWhenCulled && emitter->isCulledThisFrame) continue;
        if (emitter->lastPlaybackFrame == Time::frameCount) continue;
        emitter->lastPlaybackFrame = Time::frameCount;

        // VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速)。ゲーム実行時は常に 1.0。
        const float dt = ctx.dt * emitter->GetEditorTimeScale(Time::frameCount);

        bool canEmit = emitter->playing;
        if (canEmit && emitter->delayTime < emitter->startDelay) {
            emitter->delayTime += dt;
            canEmit = false;
        }
        if (canEmit && emitter->duration > 0.0f) {
            emitter->playTime += dt;
            if (emitter->playTime >= emitter->duration) {
                if (emitter->loop) {
                    emitter->playTime = 0.0f;
                    emitter->delayTime = 0.0f;
                    emitter->burstCyclesFired.clear();
                } else {
                    emitter->playing = false;
                    canEmit = false;
                    if (emitter->clearOnStop) {
                        emitter->particles.clear();
                        emitter->gpuClearPending = true;
                    }
                }
            }
        }
        emitter->emitThisFrame = canEmit;

        const math::Vector3 currentPosition = gameObject->transform.worldPosition;
        // エミッター自身の移動速度。inheritVelocity がスポーン初速へ足すために使う。
        // dt が 0 (完全停止・スクラブ中) のときは前回値を保つ (0 除算と速度の消失を避ける)。
        if (emitter->hasLastEmitterPosition && dt > 1.0e-6f)
            emitter->emitterVelocity = (currentPosition - emitter->lastEmitterPosition) * (1.0f / dt);
        if (emitter->hasLastEmitterPosition && canEmit && emitter->rateOverDistance > 0.0f) {
            emitter->distanceEmitAccum += (currentPosition - emitter->lastEmitterPosition).Length()
                * emitter->rateOverDistance;
            const int count = static_cast<int>(emitter->distanceEmitAccum);
            emitter->burstPending += count;
            emitter->distanceEmitAccum -= static_cast<float>(count);
        }
        emitter->lastEmitterPosition = currentPosition;
        emitter->hasLastEmitterPosition = true;

        if (emitter->burstCyclesFired.size() != emitter->bursts.size())
            emitter->burstCyclesFired.assign(emitter->bursts.size(), 0);
        for (size_t index = 0; index < emitter->bursts.size(); ++index) {
            const ParticleBurst& burst = emitter->bursts[index];
            int& fired = emitter->burstCyclesFired[index];
            const int cycles = (std::max)(burst.cycles, 1);
            while (fired < cycles
                   && emitter->playTime >= burst.time + burst.interval * static_cast<float>(fired)) {
                if (NextRandom01(*emitter) <= (std::max)(0.0f, (std::min)(burst.probability, 1.0f)))
                    emitter->burstPending += (std::max)(burst.count, 0);
                ++fired;
            }
        }

        if (emitter->prewarm && emitter->loop && !emitter->prewarmed) {
            const float warmDuration = emitter->duration > 0.0f ? emitter->duration : emitter->lifetime;
            const int warmCount = (std::min)(
                static_cast<int>((std::max)(emitter->emitRate, 0.0f) * (std::max)(warmDuration, 0.0f)),
                (std::max)(emitter->maxParticles, 0));
            emitter->burstPending += warmCount;
            emitter->prewarmSpawnPending += warmCount;
            emitter->prewarmed = true;
        }
    }

    // 描画されないScene Viewや非表示Viewportでも寿命・衝突を進めるため、CPU更新はScheduler側で完結させる。
    UpdateParticleCpuSimulation(ctx.scene, ctx.world, ctx.dt, Time::time);
}

} // namespace fbzz::scene
