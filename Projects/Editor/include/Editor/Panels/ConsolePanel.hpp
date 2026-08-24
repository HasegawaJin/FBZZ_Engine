// FBZZ Engine
// ConsolePanel.hpp | fbzz::editor
// ログエントリをフィルタ・検索・表示するコンソールパネル
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace fbzz::editor {

class ConsolePanel : public IPanel {
public:
    explicit ConsolePanel(ConsoleSink& sink);
    const char* GetWindowName() const override { return "Console"; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;

private:
    // 表示 1 行ぶん。Collapse 有効時は同一メッセージが 1 行へ畳まれ count が増える。
    // WHY: 描画のたびにフィルタと集約をやり直すとログ件数に比例してエディタが重くなる。
    //      Sink の世代とフィルタ条件が変わったときだけ再構築し、あとはこの配列を
    //      ImGuiListClipper で間引き描画する。
    struct Row {
        std::size_t    entryIndex = 0;  // ConsoleSink::GetEntries() 内の代表エントリ
        // ConsoleSink の通し番号。行の再構築やリングバッファの押し出しを跨いでも
        // 同じログ行を指し続けるため、選択はこの値で持つ (添字だと 1 行ずれる)。
        std::uint64_t  sequence   = 0;
        int            count      = 1;  // 集約された件数 (Collapse OFF なら常に 1)
        core::LogLevel level      = core::LogLevel::INFO;
    };

    void OnRenderContent(EditorContext& ctx) override;

    // 現在のフィルタ条件で m_rows を作り直す。件数バッジ用の集計もここで行う。
    void RebuildRows();
    // フィルタ条件が前回構築時と変わっていれば true。
    [[nodiscard]] bool FiltersChanged() const;
    // 選択行の全文・発生位置・ジャンプボタンを出す下部ペイン。
    void DrawDetailPane(EditorContext& ctx);
    // メッセージから "[File.cpp:123]" を解決して外部エディターで開く。
    void JumpToSource(EditorContext& ctx, const std::string& message);

    // --- 複数選択 ---
    // 通し番号から現在の表示行を引く。フィルタで隠れている場合は -1。
    [[nodiscard]] int RowIndexOf(std::uint64_t sequence) const;
    // fromRow〜toRow を選択する。additive が false なら既存の選択を捨てる。
    void SelectRange(int fromRow, int toRow, bool additive);
    // Shift / Ctrl の有無で選択の広げ方を決める。
    void ApplyRowClick(int row);
    // 選択行の全文を上から順に連結する。
    [[nodiscard]] std::string SelectedText() const;
    void CopySelection() const;

    ConsoleSink& m_sink;

    bool m_showDebug   = false;
    bool m_showInfo    = true;
    bool m_showWarn    = true;
    bool m_showError   = true;
    bool m_autoScroll  = true;
    bool m_collapse    = false;
    bool m_clearOnPlay = false;
    bool m_showDetail  = true;
    std::array<char, 256> m_filterBuf = {};
    std::string m_visibleLogText;

    // --- 表示キャッシュ (RebuildRows が更新する) ---
    std::vector<Row> m_rows;
    std::uint64_t    m_cachedRevision = ~0ull;  // 未構築を表す番兵
    std::string      m_cachedFilter;
    bool             m_cachedShowDebug = false;
    bool             m_cachedShowInfo  = false;
    bool             m_cachedShowWarn  = false;
    bool             m_cachedShowError = false;
    bool             m_cachedCollapse  = false;
    int              m_warnCount  = 0;   // フィルタ前の総数 (ツールバーのバッジ用)
    int              m_errorCount = 0;

    // 選択中の行 (ConsoleSink の通し番号)。0 は「無し」を表す番兵で、
    // ConsoleSink の採番が 1 始まりなので実在の行とぶつからない。
    std::unordered_set<std::uint64_t> m_selection;
    std::uint64_t m_anchorSequence = 0;  // Shift 範囲の起点。Shift クリックでは動かさない
    std::uint64_t m_detailSequence = 0;  // 詳細ペインに出す行 (最後に触った行)
    bool m_wasInEditor = true;   // Clear on Play のエッジ検出用
};

} // namespace fbzz::editor
