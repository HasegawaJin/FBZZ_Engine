// FBZZ Engine
// IPanel.hpp | fbzz::editor
// エディターパネルの基底インターフェース
#pragma once

namespace fbzz::editor { struct EditorContext; }

namespace fbzz::editor {

class IPanel {
public:
    virtual ~IPanel() = default;
    virtual void OnRender(EditorContext& ctx) = 0;

    bool visible = true;
};

} // namespace fbzz::editor
