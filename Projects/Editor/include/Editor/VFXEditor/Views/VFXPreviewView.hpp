// FBZZ Engine
// VFXPreviewView.hpp | fbzz::editor
// プレビュー RT の表示・環境コントロール・参照画像オーバーレイ・連番書き出しダイアログ
// WHY: プレビューは「何を描くか」(VFXPreviewController) と「どう見せるか」(この View) で
//      性質が違う。参照画像の重ね方や書き出しダイアログの開閉は表示専用の状態なので、
//      サービス側へ混ぜず View に閉じる。AI capture 用の World には一切影響しない。
#pragma once

#include <imgui.h>
#include <string>

namespace fbzz::editor {

struct EditorContext;
class VFXEditorSession;
class VFXGraphCanvas;

class VFXPreviewView {
public:
    // NOTE: canvas 参照は、プレビューへドロップされたアセットを Graph ノードとして
    //       追加する導線のためだけに持つ。
    VFXPreviewView(VFXEditorSession& session, VFXGraphCanvas& canvas)
        : m_session(session), m_canvas(canvas) {}

    void DrawViewport(EditorContext& ctx, bool fillAvailable = false);
    // 背景・ライトのプリセット、同時プレビュー数、Overdraw、A/B 比較。
    void DrawEnvironmentControls();
    void DrawReferenceOverlayControls(EditorContext& ctx);
    // 決定論 scrub を使い、代表時刻ではなく全区間を等間隔で PNG 連番にする。
    void DrawSequenceExportDialog(EditorContext& ctx);

    // 書き出しダイアログの開閉。Panel のメニューから開くため公開する。
    bool sequenceDialogOpen = false;

private:
    // 選択ノードの Transform を Preview 上で直接掴んで動かす (W/E/R で Translate/Rotate/Scale)。
    // WHY: 位置合わせを DragFloat3 でやるのは、3 軸を別々に見ながら結果を頭で合成する作業で、
    //      「煙を炎の少し上」のような相対配置ほど時間が溶ける。掴んで動かせれば一瞬で済む。
    // NOTE: 書き換えるのはグラフアセットの node であって、生成済み GameObject ではない。
    //       実体は次のライブ反映で追従する。実体を直接動かすと保存されない編集になる。
    // @return ギズモがマウス入力を消費したか (カメラ操作と取り合わないよう呼び出し側へ返す)
    bool DrawNodeGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);

    // ギズモのドラッグ 1 回を 1 つの Undo にまとめるための進行状態。
    bool m_gizmoDragging = false;

    VFXEditorSession& m_session;
    VFXGraphCanvas& m_canvas;

    // 参照画像オーバーレイ。Preview の上へ ImGui で重ねるだけなので、
    // AI capture 用 World には一切影響しない (評価画が表示設定で変わらない)。
    // テクスチャは ResourceManager がキャッシュするため、ここではパスだけ持つ。
    std::string m_referenceImagePath;
    bool m_referenceImageVisible = false;
    float m_referenceImageOpacity = 0.45f;
    float m_referenceImageScale = 1.0f;
    float m_referenceImageOffsetX = 0.0f;
    float m_referenceImageOffsetY = 0.0f;
};

} // namespace fbzz::editor
