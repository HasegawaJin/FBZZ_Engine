// FBZZ Engine
// ParticleEmitterModules.hpp | fbzz::editor
// ParticleEmitter の Shuriken 式モジュールスタック UI
// WHY: 約80項目のフラットな編集列は認知負荷が高く、どの機能が有効かも一目で分からない。
//      Unity Particle System と同じ「チェックボックス付き折りたたみモジュール」へ再構成し、
//      VFX Editor と Inspector の両方から同じ実装を呼ぶことで UI の二重管理を解消する。
#pragma once

namespace fbzz::scene { struct ParticleEmitter; }

namespace fbzz::editor {

struct EditorContext;

// モジュールスタック全体を描画する。
// @return true if any field was changed (呼び出し側で markSceneDirty する)
bool DrawParticleEmitterModules(scene::ParticleEmitter& emitter, EditorContext& ctx);

// 再生制御ボタン列 (Play/Pause/Restart/Stop/Clear/Burst)。
// VFX Editor のトランスポートと Inspector の Main モジュールが共用する。
void DrawParticleEmitterPlaybackButtons(scene::ParticleEmitter& emitter);

} // namespace fbzz::editor
