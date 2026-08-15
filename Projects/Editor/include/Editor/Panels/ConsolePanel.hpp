// FBZZ Engine
// ConsolePanel.hpp | fbzz::editor
// ログエントリをフィルタ・検索・表示するコンソールパネル
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {

class ConsolePanel : public IPanel {
public:
    explicit ConsolePanel(ConsoleSink& sink);
    const char* GetWindowName() const override { return "Console"; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;

private:
    // 表示 1 行ぶん。Collapse 有効時は同一メッセージが 1 行へ畳まれ count が増える。
    // WHY: 描画のたびにフィルタと集約をやり直すとログ件数に比例してエディタが重くなる。
    //      Sink の世代とフィルタ条件が変わったときだけ再構築し、あとはこの配列を
    //      ImGuiListClipper で間引き描画する。
    struct Row {
        std::size_t    entryIndex = 0;  // ConsoleSink::GetEntries() 内の代表エントリ
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

    int  m_selectedRow = -1;
    bool m_wasInEditor = true;   // Clear on Play のエッジ検出用
};

} // namespace fbzz::editor
