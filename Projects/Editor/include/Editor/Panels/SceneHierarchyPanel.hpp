// FBZZ Engine
// SceneHierarchyPanel.hpp | fbzz::editor
// シーン内 GameObject をツリー表示し選択状態を EditorContext に書き込む
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Scene/Entity.hpp>

namespace fbzz::editor {

class SceneHierarchyPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Scene Hierarchy"; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    // SetParent 成功後、次フレームで強制 open するノードの EntityID
    scene::EntityID m_pendingExpand;
};

} // namespace fbzz::editor
