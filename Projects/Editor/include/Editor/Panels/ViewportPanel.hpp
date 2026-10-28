/// @file    ViewportPanel.hpp
/// @brief   IRenderTarget をテクスチャとして表示しカメラ操作を受け付ける。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Tools/ClothPinBrush.hpp>
#include <Editor/Tools/FiberPaintTool.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <string>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Entity.hpp>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::editor {

/// @note Scene View への .mat ドラッグ中プレビュー状態。
/// @note Unity と同じく「カーソルが乗ったオブジェクトへ即座に仮適用し、外れたら元へ戻す。
/// @note 左ボタンを離した位置で確定」という操作のため、仮適用先と元の値を保持する。
/// @note ドラッグがビューポート外へ出た / キャンセルされた場合も、この情報で必ず巻き戻す。
struct MaterialDragPreviewState {
    scene::EntityID target       = scene::EntityID::INVALID; ///< @note 仮適用中の GameObject
    std::string     materialPath;                            ///< @note ドラッグ中の .mat
    std::string     previousPath;                            ///< @note 仮適用前の materialPath
    bool            hadComponent = false;                    ///< @note 仮適用前に MaterialComponent があったか
    bool            applied      = false;                    ///< @note 現在どれかに仮適用中か
};

class ViewportPanel : public IPanel {
public:
    enum class Kind {
        Scene,
        Game,
        UI
    };

    explicit ViewportPanel(Kind kind = Kind::Scene);

    const char* GetWindowName() const override { return m_windowName.c_str(); }
    /// @note キーの文脈を持つのは Scene ビューだけ。Game / UI ビューは自前のキーを持たない。
    HotkeyScope GetHotkeyScope() const override
    {
        return m_kind == Kind::Scene ? HotkeyScope::SceneViewport : HotkeyScope::None;
    }

    /// @note EditorApp がパネル生成後に直接セットするためpublic。これらが有効でない間はプレースホルダを描画する
    renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    renderer::IRenderer* renderer = nullptr;
    renderer::ResourceManager* resources = nullptr;

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnAfterBegin(EditorContext& ctx) override;
    ImGuiWindowFlags GetWindowFlags() const override;

private:
    Kind m_kind = Kind::Scene;
    std::string m_windowName;

    /// @note ゲームがカーソルを取り上げている間だけ立つ。OnBeforeBegin が更新し、
    /// @note 直後の GetWindowFlags() が読む (Begin の引数評価はフックの後)。
    bool m_pinWindowRect = false;

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

    /// @note Scene View の矩形 (ドラッグ) 選択状態
    bool   m_rectSelecting = false;
    ImVec2 m_rectStart = {};

    /// @note Scene View への .mat ドラッグ中プレビュー
    MaterialDragPreviewState m_materialDrag;
    ClothPinBrush m_clothPinBrush;
    FiberPaintTool m_fiberPaintTool;
    ControlPointViewportTool m_controlPoints;
    ImDrawListSplitter m_controlPointLayers;

    /// @note 前フレームでビューポート上のオーバーレイ UI (ツールバー・ブックマーク等) を
    /// @note ホバーしていたか。true の間はクリックピッキング / 矩形選択開始を抑制する。
    bool   m_prevOverlayHovered = false;
};

} /// @note namespace fbzz::editor
