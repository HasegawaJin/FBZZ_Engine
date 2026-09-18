/// @file    ParticleEmitterModules.hpp
/// @brief   ParticleEmitter の Shuriken 式モジュールスタック UI。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// Unity Particle System 式の「チェックボックス付き折りたたみモジュール」。VFX Editor と
/// Inspector の両方から同じ実装を呼び、UI の二重管理を解消する。
#pragma once
#include <vector>

namespace fbzz::scene {
struct ParticleEmitter;
struct ParticleEmitterSettings;
struct FlowFieldSettings;
}

namespace fbzz::editor {

struct EditorContext;

/// モジュールスタック全体を描画する。
/// @return true if any field was changed (呼び出し側で markSceneDirty する)
/// @note Inspector は GameObject に付いた実体を、.vfx プレファブの層はシーンと同じ
///       ParticleEmitter をそのまま編集する。後者に動いている実体は無いので、
///       再生ボタンも再生状態のリセットも意味を持たない。
/// @param liveEmitter 実体を伴う場合のコンポーネント。null なら再生制御を出さず、
///                    リセットは捨てられる (アセット編集モード)。
bool DrawParticleEmitterModules(scene::ParticleEmitterSettings& emitter, EditorContext& ctx,
                                scene::ParticleEmitter* liveEmitter = nullptr);

/// 再生制御ボタン列 (Play/Pause/Restart/Stop/Clear/Burst)。
/// VFX Editor のトランスポートと Inspector の Main モジュールが共用する。
void DrawParticleEmitterPlaybackButtons(scene::ParticleEmitter& emitter);

/// 流れ 1 本の編集 UI。シーンに置いた FlowField と、エミッター内蔵の流れ
/// (ParticleEmitterSettings::localForces) の両方が同じ設定型なので UI も共有する。
/// channels マスクは含まない (内蔵の力では相手が 1 体に決まっていて意味を持たない)。
/// @param showSpace 座標系の選択を出すか。シーンに置いた力場は原点も向きも GameObject の
///        Transform が決めるので false にする (出すと «効かない選択肢» を編集させることになる)。
/// @return true if any field was changed
bool DrawFlowFieldSettings(scene::FlowFieldSettings& force, EditorContext& ctx,
                                    bool showSpace = true);

/// 力のリスト (追加・削除つき)。エミッター内蔵の力 (ParticleEmitter::localForces) と
/// シーンに置いた場 (FlowField::forces) が同じ UI を共有する。
/// @param showSpace 座標系の選択を出すか。シーンの力場は Transform が原点を決めるので false。
/// @return true if any field was changed
bool DrawFlowFieldList(std::vector<scene::FlowFieldSettings>& forces,
                                EditorContext& ctx, bool showSpace = true);

} // namespace fbzz::editor
