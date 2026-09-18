/// @file    RenderPassViewerPanel.hpp
/// @brief   ビューごとのパス実行情報・途中画像・依存グラフ・リソース一覧を調べるドッキングパネル。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#pragma once

#include <Editor/Panels/IPanel.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassCapture.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace fbzz::editor {

/// @brief RenderGraph の実行結果を «パス一覧 + 選択中パスの詳細 + 3 つの見方» で調べる。
/// @note 左の一覧と上の詳細帯はタブの外に置く。どのタブも «いま選んでいるパス» を軸に見るため。
class RenderPassViewerPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Render Pass Viewer"; }
    bool GetDefaultVisibility() const override { return false; }
    const char* GetMenuCategory() const override { return "Debug"; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;

    /// @brief ビューの描画より前に毎フレーム呼ぶ。
    /// @note 閉じていれば専用 RT を解放する。
    void PrepareFrame();
    /// @return このビューを捕捉させるときだけ非 null。
    scene::RenderPassCapture* CaptureForView(bool gameView);

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    using Capture = scene::RenderPassCapture;

    enum class Tab { NONE, PREVIEW, GRAPH, RESOURCES };

    struct PassKey {
        std::string name;
        std::size_t occurrence = 0;
        bool operator==(const PassKey&) const = default;
    };

    static constexpr std::size_t NPOS = static_cast<std::size_t>(-1);

    /// @brief 捕捉結果をパネル側の控えへ写す。
    /// @note ビューが描かれないフレームは捕捉が空になる。控えを残し、一覧や図が点滅して消えるのを防ぐ。
    void SyncSnapshot();

    void DrawToolbar(EditorContext& ctx);
    void DrawOptionsPopup(EditorContext& ctx);
    void DrawPassList(EditorContext& ctx);
    /// @brief 選択中パスの計測値と、読み書きするリソースのチップ列。
    void DrawSelection(EditorContext& ctx);
    void DrawPreview(EditorContext& ctx);
    /// @brief ホイール拡大・ドラッグ移動・ダブルクリックで全体表示を持つ画像キャンバス。
    /// @param overlay 画像に重ねる注記。nullptr なら出さない。
    void DrawImageCanvas(ImTextureID texture, float width, float height, const char* overlay);
    void DrawGraph(EditorContext& ctx);
    void DrawGallery(EditorContext& ctx);
    /// @param inlineRow true なら直前の項目に続けて横へ流し、幅が尽きたら折り返す。
    void DrawViewSettings(Capture::ViewSettings& view, bool inlineRow);
    /// @brief パス 1 本の上書き (有効・カリング・追加 Read)。
    /// @note 一覧の行・図のノード・詳細帯・«無効化して一覧から消えたパス» の復帰行から共通で呼ぶ。
    void DrawPassOverride(EditorContext& ctx, const std::string& passName);
    void DrawPassTooltip(const Capture::PassInfo& pass) const;
    /// @brief リソースの生産者・消費者へ飛ぶメニュー項目。
    /// @param passIndex 基準にするパス (m_passes 上)。NPOS ならフレーム末尾基準。
    void DrawResourceLinks(const std::string& resource, std::size_t passIndex);

    /// @brief 選択を差し替える。出力の固定と表示設定はここで持ち越す。
    /// @param record 戻る / 進むの履歴へ積むか。履歴移動そのものでは積まない。
    void SelectPass(const std::string& passName, std::size_t occurrence, bool record = true);
    /// @brief 選択を差し替え、一覧と図の双方でその位置まで送る。
    void RevealPass(std::size_t passIndex);
    /// @brief 選択中パスのリソースを Preview に映す。
    /// @note 名前は «希望» として渡す。RT は MRT スライス名へ割れるので、解決は SyncSnapshot が前方一致で行う。
    void ShowResource(const std::string& resource);
    /// @brief ↑↓ でパス送り、Alt+←→ / マウス側面ボタンで履歴移動。
    /// @note 表を描く前に呼ぶ。行の描画中に選択が変わると自動スクロールが 1 フレーム遅れる。
    void HandleNavigation();
    void StepHistory(int step);

    [[nodiscard]] std::size_t FindPass(const std::string& name, std::size_t occurrence) const;
    [[nodiscard]] std::size_t SelectedIndex() const;
    /// @return beforeIndex より前で最後にそれを書いた実行パス。無ければ NPOS。
    [[nodiscard]] std::size_t ProducerOf(const std::string& resource, std::size_t beforeIndex) const;
    /// @return afterIndex の書き込みを読む実行パス。次の書き込みで打ち切る。
    [[nodiscard]] std::vector<std::size_t> ConsumersOf(const std::string& resource, std::size_t afterIndex) const;

    Capture m_capture;
    renderer::ResourceManager* m_resources = nullptr;

    /// @brief 最後に中身があったフレームのパス一覧。実行パスが先、カリング分が末尾。
    std::vector<Capture::PassInfo> m_passes;
    /// @brief m_passes が今フレームの捕捉か。
    bool m_live = false;
    std::size_t m_executedCount = 0;
    double m_totalCpuMs = 0.0;
    double m_totalGpuMs = 0.0;
    double m_slowestMs = 0.0;

    /// @brief 表示順に並んだ «いま行になっているパス»。↑↓ の送り先。
    std::vector<PassKey> m_visibleOrder;
    std::vector<PassKey> m_history;
    std::size_t m_historyPos = 0;
    Tab m_tabRequest = Tab::NONE;
    /// @note マウスで選んだ行は動かさない。キー送りや他タブから飛んだときだけ立てる。
    bool m_scrollToSelected = false;
    bool m_scrollToGraphNode = false;
    /// @brief 出力名の固定。空なら各パスの主出力。
    /// @note 深度や G-Buffer を見たままパスを送るため。持たないパスでは主出力へ落ち、持つパスで復帰する。
    std::string m_pinnedOutput;

    ImGuiTextFilter m_filter;
    ImGuiTextFilter m_galleryFilter;
    bool m_gameView = false;
    bool m_capturedGameView = false;
    bool m_snapshotGameView = false;
    bool m_captureActive = false;
    bool m_showCulled = true;
    bool m_galleryIssuesOnly = false;
    float m_tileZoom = 1.0f;

    /// @brief 最後に画像が取れた要求。捕捉が途切れても同じパスの間は絵を残す。
    Capture::Request m_lastPreview;
    bool m_lastPreviewValid = false;
    bool m_lastPreviewGameView = false;
    bool m_fit = true;
    float m_zoom = 1.0f;
    float m_panX = 0.0f;
    float m_panY = 0.0f;

    bool m_graphHeat = true;
    bool m_graphNeedsFit = true;
    float m_graphZoom = 1.0f;
    float m_graphPanX = 0.0f;
    float m_graphPanY = 0.0f;
    /// @brief 図の右クリックメニューの対象。
    std::string m_graphMenuPass;
};

} // namespace fbzz::editor
