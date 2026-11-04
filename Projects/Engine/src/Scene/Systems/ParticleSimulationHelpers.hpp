/// @file    ParticleSimulationHelpers.hpp
/// @brief   CPU 更新と GPU 投入で共有する粒子の発生・境界計算。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
namespace fbzz::scene { class GameObject; struct AnimatorComponent; struct Transform; }
namespace fbzz::scene::particle_simulation_detail {
/// @note 材質の CPU 設定と入力の正規化を描画前に解決する。GPU を確保せず時刻を進めない。
void PrepareParticleSimulationInputs(ParticleEmitter& emitter);
float Clamp01(float value);
float EmitRateCurveScale(const ParticleEmitter& emitter);
const AnimatorComponent* FindParticleAnimator(GameObject& object);
void InitGpuSpawnEntry(GpuSpawnEntry& entry, ParticleEmitter& emitter, const Transform& transform,
    const AnimatorComponent* animator);
void UpdateParticleBounds(ParticleEmitter& emitter, const Transform& transform);
}
