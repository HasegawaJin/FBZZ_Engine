/// @file    ParticlePlayback.hpp
/// @brief   描画・Scheduler・スクラブが共有する粒子再生の1ステップ。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <cstdint>
namespace fbzz::scene {
struct ParticleEmitter;
struct Transform;
namespace particle_runtime_detail {
bool StepParticleEmitterPlayback(ParticleEmitter& emitter, const Transform& transform, float deltaTime);
bool AdvanceParticleEmitterPlaybackAtFrame(ParticleEmitter& emitter, const Transform& transform, float deltaTime, uint64_t frame);
}
} /// @note namespace fbzz::scene
