// FBZZ Engine
// InspectorPanel.hpp | fbzz::editor
// 選択 Entity のコンポーネントを表示・編集する
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Scene/Entity.hpp>
#include <any>
#include <typeinfo>

namespace fbzz::editor {

class InspectorPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Inspector"; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    std::any              m_componentClipboard;
    const std::type_info* m_componentClipboardType = nullptr;
    char                  m_addComponentFilter[64] = {};

    // ロック機能: true のとき m_lockedEntityId のオブジェクトを固定表示する。
    // WHY: IK Solver の設定中など、Hierarchy で別オブジェクトをクリックしても
    //      Inspector の表示を切り替えずに編集を続けられるようにする。
    bool               m_locked         = false;
    scene::EntityID    m_lockedEntityId = {};
};

} // namespace fbzz::editor
