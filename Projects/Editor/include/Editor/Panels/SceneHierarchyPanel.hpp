// FBZZ Engine
// SceneHierarchyPanel.hpp | fbzz::editor
// シーン内 GameObject をツリー表示し選択状態を EditorContext に書き込む
#pragma once
#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

class SceneHierarchyPanel : public IPanel {
public:
    void OnRender(EditorContext& ctx) override;
};

} // namespace fbzz::editor
