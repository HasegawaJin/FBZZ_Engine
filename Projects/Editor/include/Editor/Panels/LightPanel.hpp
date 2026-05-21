// FBZZ Engine
// LightPanel.hpp | fbzz::editor
// Directional / Point / Spot ライトをスライダーで編集する
#pragma once
#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

class LightPanel : public IPanel {
public:
    void OnRender(EditorContext& ctx) override;
};

} // namespace fbzz::editor
