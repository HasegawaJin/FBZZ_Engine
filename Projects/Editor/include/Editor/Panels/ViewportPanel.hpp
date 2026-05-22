// FBZZ Engine
// ViewportPanel.hpp | fbzz::editor
// IRenderTarget をテクスチャとして表示しカメラ操作を受け付ける
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <memory>

namespace fbzz::renderer { class IRenderTarget; }

namespace fbzz::editor {

class ViewportPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Viewport"; }

    std::shared_ptr<renderer::IRenderTarget> hdrRT;

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnAfterBegin(EditorContext& ctx) override;
};

} // namespace fbzz::editor
