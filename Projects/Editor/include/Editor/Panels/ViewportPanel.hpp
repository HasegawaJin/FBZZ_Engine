// FBZZ Engine
// ViewportPanel.hpp | fbzz::editor
// IRenderTarget をテクスチャとして表示しカメラ操作を受け付ける
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>
#include <Engine/Renderer/ResourceHandle.hpp>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::editor {

class ViewportPanel : public IPanel {
public:
    enum class Kind {
        Scene,
        Game,
        UI
    };

    explicit ViewportPanel(Kind kind = Kind::Scene);

    const char* GetWindowName() const override { return m_windowName.c_str(); }

    renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    renderer::IRenderer* renderer = nullptr;
    renderer::ResourceManager* resources = nullptr;

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnAfterBegin(EditorContext& ctx) override;

private:
    Kind m_kind = Kind::Scene;
    std::string m_windowName;
};

} // namespace fbzz::editor
