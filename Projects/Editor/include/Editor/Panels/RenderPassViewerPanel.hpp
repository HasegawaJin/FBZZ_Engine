/// @file    RenderPassViewerPanel.hpp
/// @brief   ビューごとのパス実行情報・途中画像・リソース一覧を調べるドッキングパネル。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#pragma once

#include <Editor/Panels/IPanel.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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
    void DrawPasses(EditorContext& ctx);
    void DrawPreview(EditorContext& ctx);
    /// 選択を差し替える。出力の «固定» を跨いで持ち越すのはここだけ。
    void SelectPass(const std::string& passName, size_t occurrence);
    /// ↑↓ でパスを送る。引数は «いま画面に出ている行» を表示順に並べたもの。
    /// @note 前フレームの並びで判定する。キー処理は表を描く前に済ませないと、
    ///       選択行への自動スクロールが 1 フレーム遅れて «送るたびに枠外» になる。
    void HandlePassNavigation();
    /// パス 1 本の上書き行 (有効・カリング・追加 Read)。表の行からも、
    /// «無効にしたせいで一覧から消えたパス» の復帰行からも呼ぶ。
    void DrawPassOverride(EditorContext& ctx, const std::string& passName);
    /// 「Resources」タブ。フレーム末尾の全リソースをサムネイルで並べる。
    void DrawGallery(EditorContext& ctx);
    /// 「Graph」タブ。実行順と依存辺をノード図で見せる。
    void DrawGraph(EditorContext& ctx);
    /// Preview / Gallery で同じ 5 項目を出すので 1 つにまとめる。
    void DrawViewSettings(scene::RenderPassCapture::ViewSettings& view, bool showRange);

    scene::RenderPassCapture m_capture;
    renderer::ResourceManager* m_resources = nullptr;

    /// 表示順に並んだ «いま行になっているパス» (name, occurrence)。↑↓ の送り先を決める。
    std::vector<std::pair<std::string, std::size_t>> m_visibleOrder;
    /// 選択行を次の描画で視界へ入れる。キーで送ったときだけ立てる
    /// (マウス選択で毎回スクロールすると、掴んだ行が足元で動いて鬱陶しい)。
    bool m_scrollToSelected = false;
    /// Graph タブ側の同等物。表とノードでスクロール先が別なので別に持つ。
    bool m_scrollToGraphNode = false;
    /// 出力名の «固定»。空なら固定なし = 各パスの主出力を自動選択する。
    /// WHY 持つか: 深度や G-Buffer を見続けたままパスを送りたい。選択のたびに
    ///      主出力へ戻されると、送り 1 回ごとに出力を選び直すことになる。
    std::string m_pinnedOutput;

    ImGuiTextFilter m_filter;
    ImGuiTextFilter m_galleryFilter;
    bool m_gameView = false;
    bool m_capturedGameView = false;
    bool m_captureActive = false;
    bool m_showCulled = true;
    bool m_galleryIssuesOnly = false;
    bool m_graphHeat = true;
    float m_graphZoom = 1.0f;
    float m_tileZoom = 1.0f;
    bool m_fit = true;
    float m_zoom = 1.0f;
};

} // namespace fbzz::editor
