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
    void OnRender(EditorContext& ctx) override;

    std::shared_ptr<renderer::IRenderTarget> hdrRT;
};

} // namespace fbzz::editor
