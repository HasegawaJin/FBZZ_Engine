/// @file    ParticleEditWidgets.hpp
/// @brief   ParticleCurve / ParticleGradient のドラッグ編集ウィジェット。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// キャンバス上でキーをドラッグして編集する Unity の Curve/Gradient フィールド相当。
/// VFX Editor と Inspector の両方から共用する。
#pragma once
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <string>

namespace fbzz::editor::widgets {

/// projectRoot を渡すと、キャンバスの下に «Load / Save As» の 1 行が付き、
/// .curve / .gradient として形を使い回せるようになる。
///
/// @note 曲線はアセット参照でなくインラインで持つ。アセット化すると «1 本直したら
///       共有している全員が変わる» が既定になるが、粒子の曲線はエミッターごとの
///       微調整が普通のため (Unity の ParticleSystem も同じ理由でアセット化していない)。

/// 折れ線カーブエディタ。キャンバス上でキーをドラッグして編集する。
///   maxValue : 縦軸の最大値 (Size カーブ = 1.0, Velocity カーブ = 10.0 など)
///   空きスペースをダブルクリックでキー追加 (最大 kMaxParticleCurveKeys)、
///   キーを右クリックで削除 (最小2)。上部のコンボで補間モード (Linear/Step/Smooth) を選ぶ。
///   曲線・バーはキーを直線で結ぶのではなく Evaluate() をサンプルして描くため、
///   補間モードを増やしても表示は自動で追従する。
/// @return true if curve was changed
bool CurveEditor(const char* label, scene::ParticleCurve& curve,
                 float maxValue, float height = 96.0f,
                 const std::string* projectRoot = nullptr);

/// グラデーションエディタ。カラーバー + 下部のキーマーカーで編集する。
///   マーカーをドラッグで時刻変更、クリックで選択して色編集、
///   バーをダブルクリックでキー追加 (最大 kMaxParticleCurveKeys)、マーカーを右クリックで削除 (最小2)。
/// @return true if gradient was changed
bool GradientEditor(const char* label, scene::ParticleGradient& gradient,
                    const std::string* projectRoot = nullptr);

} // namespace fbzz::editor::widgets
