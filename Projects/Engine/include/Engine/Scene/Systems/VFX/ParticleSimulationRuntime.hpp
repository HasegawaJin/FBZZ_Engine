/// @file    ParticleSimulationRuntime.hpp
/// @brief   ParticleのCPU更新をRendererから分離してSystemSchedulerから実行する公開境界。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Engine/Api.hpp>

#include <span>

namespace fbzz::physics { class World; }

namespace fbzz::scene {

class Scene;
class GameObject;
struct ParticleEmitter;
struct Transform;

/// @brief 実際の CPU 粒子更新を固定 FPS で進めたループの可視性。
struct ParticleLoopInspection {
    int fps = 0;
    int sampledFrames = 0;
    int emptyFrames = 0;
    int boundaryEmptyFrames = 0;
    int firstEmptyFrame = -1;
};

/// @note CPUシミュレーション対象の全Emitterを1フレーム進める。
/// @note GPUで完結できない機能を選択したEmitterも同じ経路で更新する。
FBZZ_ENGINE_API void UpdateParticleCpuSimulation(Scene& scene, physics::World& world, float deltaTime, float time);

/// @note 再生状態と自身のワールド速度を 1 フレーム進め、このフレームに発生させてよいかを返す。
/// @note 1 フレームに 2 度呼んでも進まない (lastPlaybackFrame で自衛)。カリングされた
/// @note エミッターも «時間だけは進める» 必要があるため、System だけでなく描画パスからも呼ぶ。
/// @note 実装を 1 本に保つ目的で公開: システムとパスに別々の実装を置くと更新漏れで食い違う。
[[nodiscard]] FBZZ_ENGINE_API bool AdvanceParticleEmitterPlayback(ParticleEmitter& emitter,
                                                  const Transform& transform, float deltaTime);

/// @brief 一時シーンの Emitter を指定 FPS・周期数だけ再生し、粒子と Atlas の可視範囲が空になるフレームを数える。
/// @param frameCoverage Atlas 各コマの可視画素率。空なら粒子の有無だけを検査する。
/// @pre scene と emitter はこの検査専用で、実シーンの再生状態を渡さない。
[[nodiscard]] FBZZ_ENGINE_API bool InspectParticleEmitterLoop(Scene& scene, physics::World& world,
                                              GameObject& gameObject, ParticleEmitter& emitter,
                                              int fps, int cycles, std::span<const float> frameCoverage,
                                              ParticleLoopInspection& out);

/// @note VFX Editor のタイムラインスクラブ用: 単一 Emitter を ResetPlayback() で巻き戻した後、
/// @note 固定ステップ (1/60s) で targetTime まで決定論的に再シミュレートする。
/// @note randomSeed ベースの乱数のため、同じ targetTime へのスクラブは常に同じ見た目になる。
/// @note GPU シミュレーション中の Emitter は CS の粒子履歴を巻き戻せないためリスタートのみ行う。
FBZZ_ENGINE_API void ScrubParticleEmitterForEditor(Scene& scene, physics::World& world,
                                   GameObject& gameObject, ParticleEmitter& emitter,
                                   float targetTime, float time);

} /// @note namespace fbzz::scene
