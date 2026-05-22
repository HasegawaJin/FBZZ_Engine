// FBZZ Engine
// SceneHierarchyPanel.hpp | fbzz::editor
// シーン内 GameObject をツリー表示し選択状態を EditorContext に書き込む
#pragma once
#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

class SceneHierarchyPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Scene Hierarchy"; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
};

} // namespace fbzz::editor
