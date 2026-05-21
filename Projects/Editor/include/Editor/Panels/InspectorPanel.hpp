// FBZZ Engine
// InspectorPanel.hpp | fbzz::editor
// 選択 Entity のコンポーネントを表示・編集する
#pragma once
#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

class InspectorPanel : public IPanel {
public:
    void OnRender(EditorContext& ctx) override;
};

} // namespace fbzz::editor
