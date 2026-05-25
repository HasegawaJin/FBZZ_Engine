// FBZZ Engine
// InspectorPanel.hpp | fbzz::editor
// 選択 Entity のコンポーネントを表示・編集する
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <any>
#include <typeinfo>

namespace fbzz::editor {

class InspectorPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Inspector"; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    std::any m_componentClipboard;
    const std::type_info* m_componentClipboardType = nullptr;
    char m_addComponentFilter[64] = {};
};

} // namespace fbzz::editor
