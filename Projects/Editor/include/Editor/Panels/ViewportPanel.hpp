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

    // EditorApp がパネル生成後に直接セットするためpublic。これらが有効でない間はプレースホルダを描画する
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

    int m_uiGizmoDrag = -1;
    ImVec2 m_uiGizmoDragStart = {};
    float m_uiGizmoStartX = 0.0f;
    float m_uiGizmoStartY = 0.0f;
    float m_uiGizmoStartWidth = 0.0f;
    float m_uiGizmoStartHeight = 0.0f;
    float m_uiGizmoStartAngle = 0.0f;
    float m_uiGizmoStartZ = 0.0f;

    int m_lastGizmoOp = -1;
    int m_lastGizmoMode = -1;
    bool m_prevGizmoOver = false;
    bool m_prevGizmoUsing = false;

    // Scene View の矩形 (ドラッグ) 選択状態
    bool   m_rectSelecting = false;
    ImVec2 m_rectStart = {};

    // 前フレームでビューポート上のオーバーレイ UI (ツールバー・ブックマーク等) を
    // ホバーしていたか。true の間はクリックピッキング / 矩形選択開始を抑制する。
    bool   m_prevOverlayHovered = false;
};

} // namespace fbzz::editor
