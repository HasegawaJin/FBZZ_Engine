/// @file    LogListView.hpp
/// @brief   行単位のログを Console と同じ操作感 (複数選択・コピー・検索・重大度フィルタ) で表示する部品。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note Build Output の診断/生ログ、Build Settings のパッケージングログは元々 TextUnformatted の
///       一枚貼りで範囲選択もコピーも検索もできなかった。Console で固めた選択操作を行データの
///       出所に依らず使い回せるよう、表示と選択だけをここへ切り出す。
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

struct ImVec2;

namespace fbzz::editor {

enum class LogListSeverity : std::uint8_t { Info, Warning, Error };

struct LogListLine {
    /// 行の同一性。SetLines を跨いで同じ行を指す値を呼び出し側が振る (選択はこの値で持つ)。
    std::uint64_t   id       = 0;
    LogListSeverity severity = LogListSeverity::Info;
    /// 本文の前に淡色で出す短い位置表記 (例: "Foo.cpp:12")。空なら出さない。
    std::string     location;
    /// 本文の後ろに淡色で出す補足 (例: 診断コード "C2065")。空なら出さない。
    std::string     tag;
    std::string     text;
    /// クリップボードへ入れる文字列。空なら text を使う。
    std::string     copyText;
    /// ダブルクリック / "Open Source" で開くファイル。空なら開けない行。
    std::string     file;
    int             line = 0;
};

class LogListView {
public:
    explicit LogListView(const char* id);

    /// 行を差し替える。まだ存在する id の選択は残し、消えた id の選択は捨てる。
    void SetLines(std::vector<LogListLine> lines);
    /// 伸びていくログ向けの差分更新。全文の差し替えより安い。
    void AppendLine(LogListLine line);
    void PopBackLine();
    void DropFrontLines(std::size_t count);
    void ClearLines();
    [[nodiscard]] const std::vector<LogListLine>& Lines() const { return m_lines; }

    /// 重大度トグル (件数付き) / 検索欄 / Copy ボタン。trailingWidth ぶん右を空けて返るので、
    /// 呼び出し側は SameLine で同じ行へ自分のボタンを続けられる。
    void DrawToolbar(float trailingWidth = 0.0f);
    /// 一覧本体。size は BeginChild と同じ解釈 (0 = 残り全部)。
    void DrawList(const ImVec2& size);

    /// 次の DrawList でその行を単独選択し、見える位置までスクロールする。
    void Reveal(std::uint64_t id);
    void ClearSelection();

    void SetEmptyText(const char* text) { m_emptyText = text; }
    /// 診断一覧のように Info 行が出ない出所では、常に 0 のトグルを置かない。
    void SetInfoToggleVisible(bool visible) { m_infoToggle = visible; }

    bool autoScroll = true;

private:
    struct Row {
        std::size_t lineIndex = 0;
    };

    void RebuildRows();
    void CountLine(const LogListLine& line, int delta);
    [[nodiscard]] int  RowIndexOf(std::uint64_t id) const;
    void SelectRange(int fromRow, int toRow, bool additive);
    void ApplyRowClick(int row);
    [[nodiscard]] std::string SelectedText() const;
    [[nodiscard]] std::string VisibleText() const;
    void DrawRow(int row);
    void DrawSeverityToggle(const char* label, LogListSeverity severity);

    std::string                       m_id;
    std::vector<LogListLine>          m_lines;
    std::vector<Row>                  m_rows;
    bool                              m_rowsDirty = true;

    bool                              m_infoToggle = true;
    std::array<bool, 3>               m_show   = { true, true, true };
    std::array<int, 3>                m_counts = {};
    std::array<char, 256>             m_filter = {};
    std::string                       m_cachedFilter;
    std::array<bool, 3>               m_cachedShow = { true, true, true };

    std::unordered_set<std::uint64_t> m_selection;
    std::uint64_t                     m_anchorId = 0;
    bool                              m_hasAnchor = false;
    std::uint64_t                     m_revealId = 0;
    bool                              m_hasReveal = false;
    bool                              m_linesChanged = false;

    const char*                       m_emptyText = "No output.";
};

} // namespace fbzz::editor
