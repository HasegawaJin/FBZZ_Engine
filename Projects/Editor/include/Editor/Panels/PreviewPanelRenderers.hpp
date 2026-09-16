/// @file    PreviewPanelRenderers.hpp
/// @brief   PreviewPanel の種別別描画実装と共通パネルを接続する内部 API。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY: IPreviewPanel の公開契約に Animation 専用の UI 状態を漏らさず、
/// Animation / Material の実装ファイルを独立して保守できるようにする。
#pragma once

namespace fbzz::editor {

struct EditorContext;

// Animation Preview が持つ再生操作・ドロップ領域を描画する。
// Material は PreviewPanel.cpp のルーターから直接ウィジェットへ渡す。
void DrawAnimationPreviewPanelContent(EditorContext& ctx);

} // namespace fbzz::editor

