/// @file    RenderPassViewerPanel.hpp
/// @brief   ビューごとのパス実行情報と途中画像を調べるドッキングパネル。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#pragma once

#include <Editor/Panels/IPanel.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>

namespace fbzz::editor {

class RenderPassViewerPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Render Pass Viewer"; }
    bool GetDefaultVisibility() const override { return false; }
    const char* GetMenuCategory() const override { return "Debug"; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;

    /// ビューの描画より前に呼ぶ。閉じた場合は専用 RT を解放する。
    void PrepareFrame();
    scene::RenderPassCapture* CaptureForView(bool gameView);

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    void DrawPasses();
    void DrawPreview(EditorContext& ctx);
    scene::RenderPassCapture m_capture;
    renderer::ResourceManager* m_resources = nullptr;
    ImGuiTextFilter m_filter;
    bool m_gameView = false;
    bool m_capturedGameView = false;
    bool m_captureActive = false;
    bool m_showCulled = true;
    bool m_fit = true;
    float m_zoom = 1.0f;
};

} // namespace fbzz::editor
