// FBZZ Engine
// ParticleEditWidgets.hpp | fbzz::editor
// ParticleCurve / ParticleGradient のドラッグ編集ウィジェット
// WHY: 旧 UI は Time/Value の DragFloat 羅列で、カーブの形が編集中に想像できなかった。
//      Unity の Curve / Gradient フィールド相当のキャンバス操作 (キーをドラッグ、
//      ダブルクリックで追加、右クリックで削除) を VFX Editor と Inspector の両方から共用する。
#pragma once
#include <Engine/Scene/Components/ParticleEmitter.hpp>

namespace fbzz::editor::widgets {

// 折れ線カーブエディタ。キャンバス上でキーをドラッグして編集する。
//   maxValue : 縦軸の最大値 (Size カーブ = 1.0, Velocity カーブ = 10.0 など)
//   空きスペースをダブルクリックでキー追加 (最大 kMaxParticleCurveKeys)、
//   キーを右クリックで削除 (最小2)。上部のコンボで補間モード (Linear/Step/Smooth) を選ぶ。
//   曲線・バーはキーを直線で結ぶのではなく Evaluate() をサンプルして描くため、
//   補間モードを増やしても表示は自動で追従する。
// @return true if curve was changed
bool CurveEditor(const char* label, scene::ParticleCurve& curve,
                 float maxValue, float height = 96.0f);

// グラデーションエディタ。カラーバー + 下部のキーマーカーで編集する。
//   マーカーをドラッグで時刻変更、クリックで選択して色編集、
//   バーをダブルクリックでキー追加 (最大 kMaxParticleCurveKeys)、マーカーを右クリックで削除 (最小2)。
// @return true if gradient was changed
bool GradientEditor(const char* label, scene::ParticleGradient& gradient);

} // namespace fbzz::editor::widgets
