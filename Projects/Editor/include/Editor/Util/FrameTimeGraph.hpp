/// @file   FrameTimeGraph.hpp
/// @brief  フレーム時間の履歴と、その大きな数値表示 / 面グラフ
/// @author Hasegawa Jin
/// @date   2026-08-24
///
/// WHY: ビューポートの Stats HUD と Analysis > Rendering が同じ「今フレームが
///      予算に収まっているか」を見せる。履歴を表示側それぞれに持たせると、
///      どちらを開いていたかでグラフの中身が変わってしまうため 1 本に統一する。
#pragma once
#include <imgui.h>

namespace fbzz::editor::widgets {

/// 目標フレーム時間に対する状態色 (予算内 / 1.5 倍まで / 超過)。
[[nodiscard]] ImVec4 FrameBudgetColor(float ms, float targetMs);

/// 現在のフレーム時間を履歴へ記録する。同一フレーム内で複数回呼んでも 1 回だけ積む。
/// 描画関数が内部で呼ぶため通常は不要。非表示の間も履歴を途切れさせたくない場合に呼ぶ。
void SampleFrameTime();

/// 大きなフレーム時間 + fps を 1 行で描く。width <= 0 なら残り幅いっぱい。
/// @return 数値に使った予算色 (呼び出し側が同じ色で補足を添えられる)
ImVec4 FrameTimeHero(float targetMs, float width = 0.0f);

/// 履歴の面グラフ。予算を横線で示し、超過した区間だけ赤く塗る。
void FrameTimeGraph(float targetMs, float height);

} // namespace fbzz::editor::widgets
