// FBZZ Engine
// ViewportPanel.hpp | fbzz::editor
// IRenderTarget をテクスチャとして表示しカメラ操作を受け付ける
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <memory>
#include <string>

namespace fbzz::renderer { class IRenderTarget; }

namespace fbzz::editor {

class ViewportPanel : public IPanel {
public:
    enum class Kind {
        Scene,
        Game
    };

    explicit ViewportPanel(Kind kind = Kind::Scene);

    const char* GetWindowName() const override { return m_windowName.c_str(); }

    std::shared_ptr<renderer::IRenderTarget> hdrRT;

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnAfterBegin(EditorContext& ctx) override;

private:
    Kind m_kind = Kind::Scene;
    std::string m_windowName;
};

} // namespace fbzz::editor
