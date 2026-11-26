/// @file    ViewportPanel.hpp
/// @brief   IRenderTarget をテクスチャとして表示しカメラ操作を受け付ける。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Tools/ClothPinBrush.hpp>
#include <Editor/Tools/FiberPaintTool.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <optional>
#include <string>
#include <Graphics/Renderer/Diagnostics/RenderDebugOverlay.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Engine/Scene/References/Entity.hpp>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::editor {

class WorldAuthoringPanel;

/// @note Scene View への .mat ドラッグ中プレビュー状態。
/// @note Unity と同じく「カーソルが乗ったオブジェクトへ即座に仮適用し、外れたら元へ戻す。
/// @note 左ボタンを離した位置で確定」という操作のため、仮適用先と元の値を保持する。
/// @note ドラッグがビューポート外へ出た / キャンセルされた場合も、この情報で必ず巻き戻す。
struct MaterialDragPreviewState {
    scene::EntityID target       = scene::EntityID::INVALID; ///< 仮適用中の GameObject
    std::string     materialPath;                            ///< ドラッグ中の .mat
    std::string     previousPath;                            ///< 仮適用前の materialPath
    bool            hadComponent = false;                    ///< 仮適用前に MaterialComponent があったか
    bool            applied      = false;                    ///< 現在どれかに仮適用中か
};

/// @note The candidate angle is previewed by the arrow and committed once on release.
struct WaterHeadingDrag {
    scene::EntityID owner = scene::EntityID::INVALID; ///< 水の向きを編集中の WaterComponent を持つ EntityID。
    uint64_t generation = 0; ///< ドラッグを始めたシーンの世代番号。別のシーンへ適用しないため照合する。
    uint64_t lifetime = 0; ///< ドラッグ開始時の WaterComponent の寿命識別子。再作成後への書き込みを防ぐ。
    float candidate = 0.0f; ///< ドラッグ中に計算した水の向きの候補値。
    bool moved = false; ///< ドラッグ開始後に水の向きを実際に動かしたか。
};

class ViewportPanel : public IPanel {
public:
    enum class Kind {
        Scene,
        Game,
        UI
    };

    explicit ViewportPanel(Kind kind = Kind::Scene);
    /// @note EditorApp owns both panels for the viewport lifetime.
    void SetWorldAuthoringPanel(const WorldAuthoringPanel& panel) { m_worldAuthoring = &panel; }

    /// @note 呼び出し元はこのビューポートの描画直後、別ビューがスナップショットを更新する前に呼ぶ。
    void CaptureStats();

    const char* GetWindowName() const override { return m_windowName.c_str(); }
    /// @note キーの文脈を持つのは Scene ビューだけ。Game / UI ビューは自前のキーを持たない。
    HotkeyScope GetHotkeyScope() const override
    {
        return m_kind == Kind::Scene ? HotkeyScope::SceneViewport : HotkeyScope::None;
    }

    /// @note EditorApp がパネル生成後に直接セットするためpublic。これらが有効でない間はプレースホルダを描画する
    renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT; ///< 表示する HDR ビュー画像の借用レンダーターゲットハンドル。
    renderer::IRenderer* renderer = nullptr; ///< 現在のビューの描画に使う借用インターフェース。
    renderer::ResourceManager* resources = nullptr; ///< ビュー画像を解決する借用 ResourceManager。

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnAfterBegin(EditorContext& ctx) override;
    ImGuiWindowFlags GetWindowFlags() const override;

private:
    const WorldAuthoringPanel* m_worldAuthoring = nullptr; ///< Scene View にワールド制作の補助表示を描く、EditorApp 所有のパネル。
    Kind m_kind = Kind::Scene; ///< このパネルが表示する Scene、Game、UI のビュー種別。
    std::string m_windowName; ///< ビュー種別に対応する ImGui ウィンドウ名。
    std::optional<uint64_t> m_statsFrame; ///< 保持している描画統計に対応する Application フレーム番号。
    renderer::RenderDebugOverlay::RenderStats m_renderStats; ///< このビューに対応する CPU 側の描画統計の控え。
    renderer::GpuProfilerSnapshot m_gpuStats; ///< このビューに対応する GPU 計測結果の控え。

    /// @note ゲームがカーソルを取り上げている間だけ立つ。OnBeforeBegin が更新し、
    /// @note 直後の GetWindowFlags() が読む (Begin の引数評価はフックの後)。
    bool m_pinWindowRect = false; ///< 最大化した Game View のウィンドウを動かしたりリサイズしたりできないようにするか。

    int m_uiGizmoDrag = -1; ///< ドラッグ中の UI ギズモハンドルの識別番号。ドラッグしていなければ -1。
    ImVec2 m_uiGizmoDragStart = {}; ///< UI ギズモのドラッグを開始したカーソルの画面座標。
    float m_uiGizmoStartX = 0.0f; ///< ドラッグ開始時の UI 要素のローカル X 座標。
    float m_uiGizmoStartY = 0.0f; ///< ドラッグ開始時の UI 要素のローカル Y 座標。
    float m_uiGizmoStartWidth = 0.0f; ///< ドラッグ開始時の UI 要素の幅。
    float m_uiGizmoStartHeight = 0.0f; ///< ドラッグ開始時の UI 要素の高さ。
    float m_uiGizmoStartAngle = 0.0f; ///< ドラッグ開始時に控えた回転角度。
    float m_uiGizmoStartZ = 0.0f; ///< ドラッグ開始時の UI 要素のローカル Z 座標。

    int m_lastGizmoOp = -1; ///< 直前に ImGuizmo に渡した変換操作の種類。
    int m_lastGizmoMode = -1; ///< 直前に ImGuizmo に渡した座標モード。
    bool m_prevGizmoOver = false; ///< 前フレームでカーソルが変換ギズモ上にあったか。
    bool m_prevGizmoUsing = false; ///< 前フレームで変換ギズモを操作中だったか。

    bool   m_rectSelecting = false; ///< Scene View の矩形 (ドラッグ) 選択状態
    ImVec2 m_rectStart = {}; ///< 矩形選択を開始したマウスの画面座標。

    MaterialDragPreviewState m_materialDrag; ///< Scene View への .mat ドラッグ中プレビュー
    ClothPinBrush m_clothPinBrush; ///< このビューが所有する布固定点ペイントのブラシ状態。
    FiberPaintTool m_fiberPaintTool; ///< このビューが所有するファイバーペイントの状態。
    ControlPointViewportTool m_controlPoints; ///< このビューにおける制御点の選択とドラッグの状態。
    WaterHeadingDrag m_waterHeadingDrag; ///< 水の向きをドラッグで変更する間に保持する操作状態。
    ImDrawListSplitter m_controlPointLayers; ///< 制御点の背景と操作ハンドルを重ねる DrawList のレイヤー分割。

    /// @note 前フレームでビューポート上のオーバーレイ UI (ツールバー・ブックマーク等) を
    /// @note ホバーしていたか。true の間はクリックピッキング / 矩形選択開始を抑制する。
    bool   m_prevOverlayHovered = false; ///< 前フレームで別のビューオーバーレイがマウスの上にあったか。
};

} // namespace fbzz::editor
