/// @file    ParticleEditWidgets.hpp
/// @brief   ParticleCurve / ParticleGradient のドラッグ編集ウィジェット。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// WHY: 旧 UI は Time/Value の DragFloat 羅列で、カーブの形が編集中に想像できなかった。
/// Unity の Curve / Gradient フィールド相当のキャンバス操作 (キーをドラッグ、
/// ダブルクリックで追加、右クリックで削除) を VFX Editor と Inspector の両方から共用する。
#pragma once
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <string>

namespace fbzz::editor::widgets {

// projectRoot を渡すと、キャンバスの下に «Load / Save As» の 1 行が付き、
// .curve / .gradient として形を使い回せるようになる。
//
// WHY 曲線をハンドルにしないか: アセット参照にすると «1 本直したら共有している全員が
//   変わる» が既定になる。粒子の曲線はエミッターごとに微調整するほうが普通なので、
//   保持はインラインのままにして、共有したい形だけを明示的に出し入れする。
//   (Unity の ParticleSystem がカーブをアセット化していないのも同じ理由)

// 折れ線カーブエディタ。キャンバス上でキーをドラッグして編集する。
//   maxValue : 縦軸の最大値 (Size カーブ = 1.0, Velocity カーブ = 10.0 など)
//   空きスペースをダブルクリックでキー追加 (最大 kMaxParticleCurveKeys)、
//   キーを右クリックで削除 (最小2)。上部のコンボで補間モード (Linear/Step/Smooth) を選ぶ。
//   曲線・バーはキーを直線で結ぶのではなく Evaluate() をサンプルして描くため、
//   補間モードを増やしても表示は自動で追従する。
// @return true if curve was changed
bool CurveEditor(const char* label, scene::ParticleCurve& curve,
                 float maxValue, float height = 96.0f,
                 const std::string* projectRoot = nullptr);

// グラデーションエディタ。カラーバー + 下部のキーマーカーで編集する。
//   マーカーをドラッグで時刻変更、クリックで選択して色編集、
//   バーをダブルクリックでキー追加 (最大 kMaxParticleCurveKeys)、マーカーを右クリックで削除 (最小2)。
// @return true if gradient was changed
bool GradientEditor(const char* label, scene::ParticleGradient& gradient,
                    const std::string* projectRoot = nullptr);

} // namespace fbzz::editor::widgets
