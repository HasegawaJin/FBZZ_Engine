/// @file    ParticlePlayback.cpp
/// @brief   粒子の時刻・Burst・Prewarmを単一の更新処理で進める。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ParticlePlayback.hpp"
#include "ParticleRandom.hpp"
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include <Engine/Core/Time.hpp>
#include <cmath>
namespace fbzz::scene {
namespace particle_runtime_detail {
namespace {
float Clamp01(float value) { return (std::max)(0.0f, (std::min)(value, 1.0f)); }
void ClearEmitterRuntime(ParticleEmitter& emitter)
{
    emitter.runtime.particles.clear();
    emitter.runtime.emitAccum = 0.0f;
    emitter.runtime.prewarmSpawnPending = 0;
    emitter.runtime.burstPending = 0;
    emitter.runtime.injectedSpawns.clear();
}

} /// @note namespace
bool StepParticleEmitterPlayback(ParticleEmitter& emitter, const Transform& tf, float dt)
{
    /// @note 黒体モードの焼き込みはここで 1 フレームに 1 回だけ行う。CPU 更新も GPU 定数バッファも
    /// @note この後の runtimeGradient を読むため、両経路の色が原理的にずれない。
    emitter.RefreshRuntimeGradient();

    bool canEmit = emitter.settings.playing;
    if (canEmit && emitter.runtime.delayTime < emitter.settings.startDelay) {
        emitter.runtime.delayTime += dt;
        canEmit = false;
    }
    /// @note duration の有無に関わらず進める (playTime は «再生開始からの経過»)。
    /// @note 止めると duration = 0 のエミッターで時刻指定 Burst が永久に発火しない。
    if (canEmit) emitter.runtime.playTime += dt;
    const float loopTolerance = emitter.settings.loop
        ? (std::min)((std::max)(dt, 0.0f) * 1.0e-3f, emitter.settings.duration * 1.0e-3f)
        : 0.0f;
    if (canEmit && emitter.settings.duration > 0.0f
        && emitter.runtime.playTime + loopTolerance >= emitter.settings.duration) {
        if (emitter.settings.loop) {
            /// @note lifetime と duration が等しい粒子は同じ刻みで死ぬ。float の累積誤差で
            /// @note 次の Burst が 1 刻み遅れると空白が出るので、境界近傍を同時刻とみなす。
            const float remainder = std::fmod(emitter.runtime.playTime, emitter.settings.duration);
            emitter.runtime.playTime = std::abs(emitter.runtime.playTime - emitter.settings.duration)
                <= loopTolerance ? 0.0f : remainder;
            emitter.runtime.delayTime = 0.0f;
            emitter.runtime.burstCyclesFired.clear();
        } else {
            emitter.settings.playing = false;
            canEmit = false;
            if (emitter.settings.clearOnStop) {
                ClearEmitterRuntime(emitter);
                emitter.runtime.gpuClearPending = true;
            }
        }
    }
    emitter.runtime.emitThisFrame = canEmit;

    /// @note 距離Emission: Emitterのワールド移動量を粒子数へ変換する。
    const math::Vector3 emitterWorldPosition = TransformEmitterPoint(tf, emitter.settings.emitPosition);
    /// @note エミッター自身のワールド速度。inheritVelocity がスポーン初速へ足すために使う。
    /// @note dt が 0 (完全停止・スクラブ中) のときは前回値を保つ (0 除算と速度の消失を避ける)。
    if (emitter.runtime.hasLastEmitterPosition && dt > 1.0e-6f) {
        emitter.runtime.emitterVelocity =
            (emitterWorldPosition - emitter.runtime.lastEmitterPosition) * (1.0f / dt);
    }
    if (emitter.runtime.hasLastEmitterPosition && canEmit && emitter.settings.rateOverDistance > 0.0f) {
        emitter.runtime.distanceEmitAccum += (emitterWorldPosition - emitter.runtime.lastEmitterPosition).Length()
            * emitter.settings.rateOverDistance;
        const int distanceCount = static_cast<int>(emitter.runtime.distanceEmitAccum);
        if (distanceCount > 0) {
            emitter.runtime.burstPending += distanceCount;
            emitter.runtime.distanceEmitAccum -= static_cast<float>(distanceCount);
        }
    }
    emitter.runtime.lastEmitterPosition = emitterWorldPosition;
    emitter.runtime.hasLastEmitterPosition = true;

    /// @note 時刻指定Burst。loop時はplayTimeの巻き戻しでcycle状態もリセットされる。
    if (emitter.runtime.burstCyclesFired.size() != emitter.settings.bursts.size())
        emitter.runtime.burstCyclesFired.assign(emitter.settings.bursts.size(), 0);
    for (size_t burstIndex = 0; burstIndex < emitter.settings.bursts.size(); ++burstIndex) {
        const ParticleBurst& burst = emitter.settings.bursts[burstIndex];
        int& fired = emitter.runtime.burstCyclesFired[burstIndex];
        const int cycles = (std::max)(burst.cycles, 1);
        while (fired < cycles
               && emitter.runtime.playTime >= burst.time + burst.interval * static_cast<float>(fired)) {
            if (Random01(emitter) <= Clamp01(burst.probability))
                emitter.Burst(burst.count);
            ++fired;
        }
    }

    /// @note Prewarmは初回描画前に定常個数を投入する。GPU readbackを避けるため履歴位置は近似する。
    if (emitter.settings.prewarm && emitter.settings.loop && !emitter.runtime.prewarmed) {
        const float warmDuration = emitter.settings.duration > 0.0f ? emitter.settings.duration : emitter.settings.lifetime;
        const int warmCount = static_cast<int>((std::max)(emitter.settings.emitRate, 0.0f)
            * (std::max)(warmDuration, 0.0f));
        const int clampedWarmCount = (std::min)(warmCount, (std::max)(emitter.settings.maxParticles, 0));
        emitter.Burst(clampedWarmCount);
        emitter.runtime.prewarmSpawnPending += clampedWarmCount;
        emitter.runtime.prewarmed = true;
    }
    return canEmit;
}

bool AdvanceParticleEmitterPlaybackAtFrame(ParticleEmitter& emitter, const Transform& transform, float deltaTime, uint64_t frame)
{
    if (emitter.runtime.lastPlaybackFrame == frame) return emitter.runtime.emitThisFrame;
    emitter.runtime.lastPlaybackFrame = frame;
    return StepParticleEmitterPlayback(emitter, transform, deltaTime);
}
} /// @note namespace particle_runtime_detail
bool AdvanceParticleEmitterPlayback(ParticleEmitter& emitter, const Transform& transform, float deltaTime)
{
    return particle_runtime_detail::AdvanceParticleEmitterPlaybackAtFrame(emitter, transform, deltaTime, Time::frameCount);
}
} /// @note namespace fbzz::scene
