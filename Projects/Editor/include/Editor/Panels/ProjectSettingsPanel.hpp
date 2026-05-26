// FBZZ Engine
// ProjectSettingsPanel.hpp | fbzz::editor
// タグ・レイヤー名の編集パネル
#pragma once
#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

class ProjectSettingsPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Project Settings"; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    char m_newTag[64] = {};
};

} // namespace fbzz::editor
