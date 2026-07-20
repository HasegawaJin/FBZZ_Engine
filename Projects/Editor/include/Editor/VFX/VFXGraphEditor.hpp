// FBZZ Engine
// VFXGraphEditor.hpp | fbzz::editor
// VFX GraphのCanvas・Viewport・Inspectorを同時表示するUIオーケストレーター
#pragma once

namespace fbzz::editor {

struct EditorContext;
class VFXEditorPanel;

// Graphドキュメントの画面構成を所有し、Emitter編集PanelからGraph UI責務を分離する。
// WHY: GraphとViewportを常時並列表示しつつ、将来Graph Widgetを独立再利用できる境界を作る。
class VFXGraphEditor {
public:
    // 1フレーム分のGraphドキュメントUIを3列ワークスペースとして描画する。
    void Render(VFXEditorPanel& host, EditorContext& context);
};

} // namespace fbzz::editor
