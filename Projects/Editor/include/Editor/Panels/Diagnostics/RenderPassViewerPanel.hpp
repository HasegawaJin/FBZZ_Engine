/// @file    RenderPassViewerPanel.hpp
/// @brief   ビューごとのパス実行情報・途中画像・依存グラフ・リソース一覧を調べるドッキングパネル。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#pragma once

#include <Editor/Panels/IPanel.hpp>
#include <Graphics/Pipeline/Capture/RenderPassCapture.hpp>

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
    void OnRegisterOperators(OperatorRegistry& registry) override;

    /// @brief ビューの描画より前に毎フレーム呼ぶ。
    /// @note 閉じていれば専用 RT を解放する。
    void PrepareFrame();
    /// @return このビューを捕捉させるときだけ非 null。
    renderer::RenderPassCapture* CaptureForView(bool gameView);

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    using Capture = renderer::RenderPassCapture;

    enum class Tab { NONE, PREVIEW, GRAPH, RESOURCES };

    struct PassKey {
        std::string name; ///< 描画パスの名前。
        std::size_t occurrence = 0; ///< 同じ名前で実行されるパスを区別する出現順。
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
    /// @param[in] overlay 画像に重ねる注記。nullptr なら出さない。
    void DrawImageCanvas(ImTextureID texture, float width, float height, const char* overlay);
    void DrawGraph(EditorContext& ctx);
    void DrawGallery(EditorContext& ctx);
    /// @param[in] inlineRow true なら直前の項目に続けて横へ流し、幅が尽きたら折り返す。
    void DrawViewSettings(Capture::ViewSettings& view, bool inlineRow);
    /// @brief パス 1 本の上書き (有効・カリング・追加 Read)。
    /// @note 一覧の行・図のノード・詳細帯・«無効化して一覧から消えたパス» の復帰行から共通で呼ぶ。
    void DrawPassOverride(EditorContext& ctx, const std::string& passName);
    void DrawPassTooltip(const Capture::PassInfo& pass) const;
    /// @brief リソースの生産者・消費者へ飛ぶメニュー項目。
    /// @param[in] passIndex 基準にするパス (m_passes 上)。NPOS ならフレーム末尾基準。
    void DrawResourceLinks(const std::string& resource, std::size_t passIndex);

    /// @brief 選択を差し替える。出力の固定と表示設定はここで持ち越す。
    /// @param[in] record 戻る / 進むの履歴へ積むか。履歴移動そのものでは積まない。
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

    Capture m_capture; ///< 現在描画中のビューからパスとリソースを受け取り、プレビューへ渡す捕捉状態。
    renderer::ResourceManager* m_resources = nullptr; ///< 捕捉した画像を解決・解放するときに借りる ResourceManager。

    std::vector<Capture::PassInfo> m_passes; ///< 最後に中身があったフレームのパス一覧。実行パスが先、カリング分が末尾。
    bool m_live = false; ///< m_passes が今フレームの捕捉か。
    std::size_t m_executedCount = 0; ///< 控えた一覧のうち、実行された描画パスの件数。
    double m_totalCpuMs = 0.0; ///< 控えた描画パスの CPU 所要時間の合計 [ms]。
    double m_totalGpuMs = 0.0; ///< 控えた描画パスの GPU 所要時間の合計 [ms]。
    double m_slowestMs = 0.0; ///< パスの色分けに使う、控えた一覧で最も長い所要時間 [ms]。

    std::vector<PassKey> m_visibleOrder; ///< 表示順に並んだ «いま行になっているパス»。↑↓ の送り先。
    std::vector<PassKey> m_history; ///< 戻る・進む操作に使う選択パスの履歴。
    std::size_t m_historyPos = 0; ///< m_history 内で現在表示している項目の添字。
    Tab m_tabRequest = Tab::NONE; ///< 次の描画で選択するタブ。NONE なら切り替えなし。
    bool m_scrollToSelected = false; ///< マウスで選んだ行は動かさない。キー送りや他タブから飛んだときだけ立てる。
    bool m_scrollToGraphNode = false; ///< 別の UI から選択したパスをグラフ上で表示位置へ送る要求。
    /// @brief 出力名の固定。空なら各パスの主出力。
    /// @note 深度や G-Buffer を見たままパスを送るため。持たないパスでは主出力へ落ち、持つパスで復帰する。
    std::string m_pinnedOutput; ///< 別のパスに移動しても優先したい出力リソースの名前。空なら主出力。

    ImGuiTextFilter m_filter; ///< 描画パスの一覧に対する検索条件。
    ImGuiTextFilter m_galleryFilter; ///< 捕捉画像ギャラリーに対する検索条件。
    bool m_gameView = false; ///< 捕捉と表示の対象を Game View にするか。false なら Scene View。
    bool m_capturedGameView = false; ///< 最新の捕捉データが Game View に対応するか。
    bool m_snapshotGameView = false; ///< 表示用として保存したスナップショットが Game View に対応するか。
    bool m_captureActive = false; ///< パネル表示または API 要求により捕捉中か。
    bool m_apiCapture = false; ///< Editor API がパネル非表示中も捕捉を必要としているか。
    bool m_apiGallery = false; ///< Editor API がギャラリー用の画像も必要としているか。
    bool m_showCulled = true; ///< 実行されなかった描画パスも一覧に表示するか。
    bool m_galleryIssuesOnly = false; ///< ギャラリーの表示対象を診断のある画像に絞るか。
    float m_tileZoom = 1.0f; ///< ギャラリーのサムネイルの表示倍率。

    Capture::Request m_lastPreview; ///< 最後に画像が取れた要求。捕捉が途切れても同じパスの間は絵を残す。
    bool m_lastPreviewValid = false; ///< 前回のプレビューが、現在も表示候補として利用できるか。
    bool m_lastPreviewGameView = false; ///< 前回表示したプレビューが Game View に対応するか。
    bool m_fit = true; ///< プレビュー画像全体をキャンバスの範囲へ合わせて表示するか。
    float m_zoom = 1.0f; ///< 手動調整したプレビュー画像の表示倍率。
    float m_panX = 0.0f; ///< プレビュー画像の横方向の表示オフセット。
    float m_panY = 0.0f; ///< プレビュー画像の縦方向の表示オフセット。

    bool m_graphHeat = true; ///< 依存グラフを描画パスの計測時間で色分けするか。
    bool m_graphNeedsFit = true; ///< 次の描画で依存グラフ全体をビューへ収める要求。
    float m_graphZoom = 1.0f; ///< 描画パスの依存グラフの表示倍率。
    float m_graphPanX = 0.0f; ///< 依存グラフの横方向の表示オフセット。
    float m_graphPanY = 0.0f; ///< 依存グラフの縦方向の表示オフセット。
    std::string m_graphMenuPass; ///< 図の右クリックメニューの対象。
};

} // namespace fbzz::editor
