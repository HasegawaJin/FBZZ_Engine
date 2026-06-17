// FBZZ Engine
// SceneHierarchyPanel.hpp | fbzz::editor
// シーン内 GameObject をツリー表示し選択状態を EditorContext に書き込む
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Scene/Entity.hpp>
#include <vector>

namespace fbzz::editor {

class SceneHierarchyPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Scene Hierarchy"; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    // SetParent 成功後、次フレームで強制 open するノードの EntityID
    scene::EntityID m_pendingExpand;
    scene::EntityID m_lastClickedEntity;               // Shift+クリック範囲選択のアンカー
    scene::EntityID m_renamingId;                      // F2 リネーム対象
    char            m_renameBuffer[256] = {};
    std::vector<scene::EntityID> m_visibleOrder;       // 前フレームの描画順 (Shift+クリック用)

    char m_searchFilter[128] = {};
};

} // namespace fbzz::editor
