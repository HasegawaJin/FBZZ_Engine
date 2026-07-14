// FBZZ Engine
// ParticleSimulationRuntime.hpp | fbzz::scene
// ParticleのCPU更新をRendererから分離してSystemSchedulerから実行する公開境界。
#pragma once

namespace fbzz::physics { class World; }

namespace fbzz::scene {

class Scene;
class GameObject;
struct ParticleEmitter;

// CPUシミュレーション対象の全Emitterを1フレーム進める。
// GPUで完結できない機能を選択したEmitterも同じ経路で更新する。
void UpdateParticleCpuSimulation(Scene& scene, physics::World& world, float deltaTime, float time);

// VFX Editor のタイムラインスクラブ用: 単一 Emitter を ResetPlayback() で巻き戻した後、
// 固定ステップ (1/60s) で targetTime まで決定論的に再シミュレートする。
// randomSeed ベースの乱数のため、同じ targetTime へのスクラブは常に同じ見た目になる。
// GPU シミュレーション中の Emitter は CS の粒子履歴を巻き戻せないためリスタートのみ行う。
void ScrubParticleEmitterForEditor(Scene& scene, physics::World& world,
                                   GameObject& gameObject, ParticleEmitter& emitter,
                                   float targetTime, float time);

} // namespace fbzz::scene
