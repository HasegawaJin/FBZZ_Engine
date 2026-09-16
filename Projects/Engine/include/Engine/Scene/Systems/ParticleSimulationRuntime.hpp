/// @file    ParticleSimulationRuntime.hpp
/// @brief   ParticleのCPU更新をRendererから分離してSystemSchedulerから実行する公開境界。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

namespace fbzz::physics { class World; }

namespace fbzz::scene {

class Scene;
class GameObject;
struct ParticleEmitter;
struct Transform;

// CPUシミュレーション対象の全Emitterを1フレーム進める。
// GPUで完結できない機能を選択したEmitterも同じ経路で更新する。
void UpdateParticleCpuSimulation(Scene& scene, physics::World& world, float deltaTime, float time);

// 再生状態 (delay / duration / loop / 距離 Emission / 時刻 Burst / Prewarm) と
// エミッター自身のワールド速度を 1 フレーム分進め、このフレームに発生させてよいかを返す。
//
// 1 フレームに 2 度呼ばれても進まない (lastPlaybackFrame で自衛する)。通常は
// ParticleSimulationSystem が先に進め、描画パスは早期 return する ─ カリングされた
// エミッターでも «時間だけは進める» 必要があるため、描画パスからも呼べる形にしている。
//
// WHY 公開境界に置くか: 実装を 1 本に保つため。システムとパスに «ほぼ同じ» 実装を
//     並べると、片方だけ直した差 (基準点・速度の更新漏れ) が «どちらが先に走ったか»
//     に依存する再現しにくいズレになる。
[[nodiscard]] bool AdvanceParticleEmitterPlayback(ParticleEmitter& emitter,
                                                  const Transform& transform, float deltaTime);

// VFX Editor のタイムラインスクラブ用: 単一 Emitter を ResetPlayback() で巻き戻した後、
// 固定ステップ (1/60s) で targetTime まで決定論的に再シミュレートする。
// randomSeed ベースの乱数のため、同じ targetTime へのスクラブは常に同じ見た目になる。
// GPU シミュレーション中の Emitter は CS の粒子履歴を巻き戻せないためリスタートのみ行う。
void ScrubParticleEmitterForEditor(Scene& scene, physics::World& world,
                                   GameObject& gameObject, ParticleEmitter& emitter,
                                   float targetTime, float time);

} // namespace fbzz::scene
