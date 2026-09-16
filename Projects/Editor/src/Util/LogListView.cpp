/// @file    LogListView.cpp
/// @brief   行単位ログの複数選択・コピー・検索付き一覧の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Editor/Util/LogListView.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/SourceOpen.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace fbzz::editor {

namespace {

// WHY 名前に LogList を冠するか: Editor は unity build で、Console 側の匿名名前空間にも
//     同じ役割の関数 (LogLevelColor 等) がある。同じバッチに入ると再定義になる。
std::size_t LogListSeverityIndex(LogListSeverity severity)
{
    return static_cast<std::size_t>(severity);
}

// 左端のバーとバッジの色。Info はビルドログの大半を占めるので、色を付けると全行が
// 塗られて警告・エラーが浮かなくなる。淡色に落とす。
ImVec4 LogListSeverityColor(LogListSeverity severity)
{
    switch (severity) {
    case LogListSeverity::Warning: return EditorTheme::Color(ThemeColor::Warning);
    case LogListSeverity::Error:   return EditorTheme::Color(ThemeColor::Danger);
    case LogListSeverity::Info:
    default:                       return EditorTheme::Color(ThemeColor::TextFaint);
    }
}

ImVec4 LogListTextColor(LogListSeverity severity)
{
    switch (severity) {
    case LogListSeverity::Warning: return EditorTheme::Color(ThemeColor::Warning);
    case LogListSeverity::Error:   return EditorTheme::Color(ThemeColor::Danger);
    case LogListSeverity::Info:
    default:                       return EditorTheme::Color(ThemeColor::Text);
    }
}

ImU32 LogListRowTint(LogListSeverity severity, bool selected)
{
    if (selected) return 0;
    switch (severity) {
    case LogListSeverity::Warning: return EditorTheme::ColorU32(ThemeColor::Warning, 0.10f);
    case LogListSeverity::Error:   return EditorTheme::ColorU32(ThemeColor::Danger,  0.14f);
    default:                       return 0;
    }
}

const char* LogListBadge(LogListSeverity severity)
{
    switch (severity) {
    case LogListSeverity::Warning: return "W";
    case LogListSeverity::Error:   return "E";
    case LogListSeverity::Info:
    default:                       return "I";
    }
}

// ImGuiListClipper は等高前提なので、行高はこの 1 か所で決めて Begin にも渡す。
float LogListRowHeight()
{
    return ImGui::GetTextLineHeight() + 6.0f;
}

const std::string& LogListCopyTextOf(const LogListLine& line)
{
    return line.copyText.empty() ? line.text : line.copyText;
}

} // namespace

LogListView::LogListView(const char* id) : m_id(id) {}

void LogListView::CountLine(const LogListLine& line, int delta)
{
    m_counts[LogListSeverityIndex(line.severity)] += delta;
}

void LogListView::SetLines(std::vector<LogListLine> lines)
{
    m_lines = std::move(lines);
    m_counts.fill(0);
    std::unordered_set<std::uint64_t> alive;
    alive.reserve(m_lines.size());
    for (const LogListLine& line : m_lines) {
        CountLine(line, +1);
        alive.insert(line.id);
    }
    std::erase_if(m_selection, [&](std::uint64_t id) { return !alive.contains(id); });
    if (m_hasAnchor && !alive.contains(m_anchorId)) m_hasAnchor = false;
    m_rowsDirty    = true;
    m_linesChanged = true;
}

void LogListView::AppendLine(LogListLine line)
{
    CountLine(line, +1);
    m_lines.push_back(std::move(line));
    m_rowsDirty    = true;
    m_linesChanged = true;
}

void LogListView::PopBackLine()
{
    if (m_lines.empty()) return;
    CountLine(m_lines.back(), -1);
    // 選択は落とさない。呼び出し側は同じ id で行を積み直す (書きかけの末尾行の更新)。
    m_lines.pop_back();
    m_rowsDirty = true;
}

void LogListView::DropFrontLines(std::size_t count)
{
    count = (std::min)(count, m_lines.size());
    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) {
        CountLine(m_lines[i], -1);
        m_selection.erase(m_lines[i].id);
        if (m_hasAnchor && m_anchorId == m_lines[i].id) m_hasAnchor = false;
    }
    m_lines.erase(m_lines.begin(), m_lines.begin() + static_cast<std::ptrdiff_t>(count));
    m_rowsDirty = true;
}

void LogListView::ClearLines()
{
    m_lines.clear();
    m_counts.fill(0);
    ClearSelection();
    m_rowsDirty    = true;
    m_linesChanged = true;
}

void LogListView::ClearSelection()
{
    m_selection.clear();
    m_hasAnchor = false;
}

void LogListView::Reveal(std::uint64_t id)
{
    m_revealId  = id;
    m_hasReveal = true;
}

void LogListView::RebuildRows()
{
    const std::string filter(m_filter.data());
    m_rows.clear();
    m_rows.reserve(m_lines.size());
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        const LogListLine& line = m_lines[i];
        if (!m_show[LogListSeverityIndex(line.severity)]) continue;
        if (!filter.empty()
            && !util::StringUtils::ContainsCI(line.text, filter)
            && !util::StringUtils::ContainsCI(line.location, filter)
            && !util::StringUtils::ContainsCI(line.tag, filter))
            continue;
        m_rows.push_back(Row{ i });
    }
    m_cachedFilter = filter;
    m_cachedShow   = m_show;
    m_rowsDirty    = false;
}

int LogListView::RowIndexOf(std::uint64_t id) const
{
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_lines[m_rows[i].lineIndex].id == id) return static_cast<int>(i);
    }
    return -1;
}

void LogListView::SelectRange(int fromRow, int toRow, bool additive)
{
    if (!additive) m_selection.clear();
    if (m_rows.empty()) return;
    if (fromRow > toRow) std::swap(fromRow, toRow);
    fromRow = (std::max)(fromRow, 0);
    toRow   = (std::min)(toRow, static_cast<int>(m_rows.size()) - 1);
    for (int row = fromRow; row <= toRow; ++row)
        m_selection.insert(m_lines[m_rows[static_cast<std::size_t>(row)].lineIndex].id);
}

void LogListView::ApplyRowClick(int row)
{
    const ImGuiIO&      io     = ImGui::GetIO();
    const std::uint64_t id     = m_lines[m_rows[static_cast<std::size_t>(row)].lineIndex].id;
    const int           anchor = m_hasAnchor ? RowIndexOf(m_anchorId) : -1;

    if (io.KeyShift && anchor >= 0) {
        // 起点は素のクリックだけが動かす (Console と同じ。行き過ぎを戻して選び直せるように)。
        SelectRange(anchor, row, io.KeyCtrl);
    } else if (io.KeyCtrl) {
        if (!m_selection.insert(id).second) m_selection.erase(id);
        m_anchorId  = id;
        m_hasAnchor = true;
    } else {
        m_selection.clear();
        m_selection.insert(id);
        m_anchorId  = id;
        m_hasAnchor = true;
    }
}

std::string LogListView::SelectedText() const
{
    std::string text;
    // 画面の並びで拾う。選択は集合なので、そのまま回すと順序が崩れる。
    for (const Row& row : m_rows) {
        const LogListLine& line = m_lines[row.lineIndex];
        if (!m_selection.contains(line.id)) continue;
        text += LogListCopyTextOf(line);
        text += '\n';
    }
    return text;
}

std::string LogListView::VisibleText() const
{
    std::string text;
    for (const Row& row : m_rows) {
        text += LogListCopyTextOf(m_lines[row.lineIndex]);
        text += '\n';
    }
    return text;
}

void LogListView::DrawSeverityToggle(const char* label, LogListSeverity severity)
{
    bool&     enabled = m_show[LogListSeverityIndex(severity)];
    const int count   = m_counts[LogListSeverityIndex(severity)];
    char text[32];
    std::snprintf(text, sizeof(text), "%s %d###sev_%s", label, count, label);

    // Info のバー色は淡色だが、トグルは押下状態を読ませたいので Info 色で塗る。
    const ImVec4 color = (severity == LogListSeverity::Info)
                       ? EditorTheme::Color(ThemeColor::Info)
                       : LogListSeverityColor(severity);
    if (enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, { color.x, color.y, color.z, 0.30f });
        ImGui::PushStyleColor(ImGuiCol_Text, color);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Surface));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
    }
    if (ImGui::Button(text)) enabled = !enabled;
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%d lines\nClick to show / hide", count);
}

void LogListView::DrawToolbar(float trailingWidth)
{
    ImGui::PushID(m_id.c_str());

    const ImGuiStyle& st = ImGui::GetStyle();
    const float       sp = st.ItemSpacing.x;

    DrawSeverityToggle("E", LogListSeverity::Error);   ImGui::SameLine(0.0f, 4.0f);
    DrawSeverityToggle("W", LogListSeverity::Warning);
    if (m_infoToggle) {
        ImGui::SameLine(0.0f, 4.0f);
        DrawSeverityToggle("I", LogListSeverity::Info);
    }
    ImGui::SameLine(0.0f, sp);

    const int selectedCount = static_cast<int>(m_selection.size());
    char copyLabel[64];
    std::snprintf(copyLabel, sizeof(copyLabel), "Copy (%d)###copy_selected", selectedCount);
    char copyLabelVisible[32];
    std::snprintf(copyLabelVisible, sizeof(copyLabelVisible), "Copy (%d)", selectedCount);
    const float copyW = ImGui::CalcTextSize(copyLabelVisible).x + st.FramePadding.x * 2.0f;

    const float trailing = trailingWidth > 0.0f ? trailingWidth + sp : 0.0f;
    const float searchW  = (std::max)(100.0f,
                                      ImGui::GetContentRegionAvail().x - copyW - sp - trailing);
    ImGui::SetNextItemWidth(searchW);
    ImGui::InputTextWithHint("##filter", "Search...", m_filter.data(), m_filter.size());
    ImGui::SameLine(0.0f, sp);

    ImGui::BeginDisabled(selectedCount == 0);
    if (ImGui::Button(copyLabel)) {
        // 検索語を今フレームで変えた直後でも、画面と同じ並びでコピーさせる。
        if (m_rowsDirty || m_cachedFilter != m_filter.data() || m_cachedShow != m_show)
            RebuildRows();
        const std::string text = SelectedText();
        if (!text.empty()) ImGui::SetClipboardText(text.c_str());
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Click a line, Shift+click (or drag) for a range, Ctrl+click to add.\n"
                          "Ctrl+A selects every visible line, Ctrl+C copies the selection.\n"
                          "Right-click a line for Copy All Visible / Open Source.");
    }
    if (trailingWidth > 0.0f) ImGui::SameLine(0.0f, sp);

    ImGui::PopID();
}

void LogListView::DrawRow(int row)
{
    const LogListLine& line     = m_lines[m_rows[static_cast<std::size_t>(row)].lineIndex];
    const bool         selected = m_selection.contains(line.id);
    const float        rowH     = LogListRowHeight();

    ImGui::PushID(row);

    // 本文をラベルにしないのは Console と同じ理由 ("##" を含む行が途中で切れる)。
    if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick, { 0.0f, rowH }))
        ApplyRowClick(row);

    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    ImDrawList*  dl     = ImGui::GetWindowDrawList();

    if (const ImU32 tint = LogListRowTint(line.severity, selected); tint != 0)
        dl->AddRectFilled(rowMin, rowMax, tint);
    const ImU32 barColor = ImGui::ColorConvertFloat4ToU32(LogListSeverityColor(line.severity));
    dl->AddRectFilled(rowMin, { rowMin.x + 3.0f, rowMax.y }, barColor);

    // WHY 押した瞬間に起点を取るか: Selectable が true を返すのは離した時で、
    //     そこまで待つとドラッグの開始行が分からない。
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !io.KeyShift && !io.KeyCtrl) {
        m_anchorId  = line.id;
        m_hasAnchor = true;
    }
    // ドラッグ中は押した行が ActiveId を握るので、他の行の hover は BlockedByActiveItem で拾う。
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && m_hasAnchor) {
        if (const int anchor = RowIndexOf(m_anchorId); anchor >= 0)
            SelectRange(anchor, row, false);
    }
    // 選択の外を右クリックしたら選び直す。中ならまとめてコピーしたいので触らない。
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && !m_selection.contains(line.id)) {
        m_selection.clear();
        m_selection.insert(line.id);
        m_anchorId  = line.id;
        m_hasAnchor = true;
    }
    const bool canOpen = !line.file.empty();
    if (canOpen && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        OpenSourceInExternalEditor(line.file, line.line);

    const float textY = rowMin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
    float       x     = rowMin.x + 10.0f;

    dl->AddText({ x, textY }, barColor, LogListBadge(line.severity));
    x += ImGui::CalcTextSize("W").x + 8.0f;

    const ImU32 faint = EditorTheme::ColorU32(ThemeColor::TextFaint);
    if (!line.location.empty()) {
        dl->AddText({ x, textY }, faint, line.location.c_str(),
                    line.location.c_str() + line.location.size());
        x += ImGui::CalcTextSize(line.location.c_str(),
                                 line.location.c_str() + line.location.size()).x + 10.0f;
    }

    // 一覧は 1 行に揃える (等高前提のクリッパーのため)。全文はコピーで取る。
    const char* bodyBegin = line.text.c_str();
    const char* bodyEnd   = bodyBegin + line.text.size();
    if (const char* nl = std::find(bodyBegin, bodyEnd, '\n'); nl != bodyEnd) bodyEnd = nl;
    dl->AddText({ x, textY }, ImGui::ColorConvertFloat4ToU32(LogListTextColor(line.severity)),
                bodyBegin, bodyEnd);

    if (!line.tag.empty()) {
        x += ImGui::CalcTextSize(bodyBegin, bodyEnd).x + 10.0f;
        dl->AddText({ x, textY }, faint, line.tag.c_str(), line.tag.c_str() + line.tag.size());
    }

    if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)
        && !line.copyText.empty() && line.copyText != line.text) {
        ImGui::SetItemTooltip("%s", line.copyText.c_str());
    }

    if (ImGui::BeginPopupContextItem("##ctx")) {
        char copySelectedLabel[64];
        std::snprintf(copySelectedLabel, sizeof(copySelectedLabel),
                      "Copy Selected (%d)###ctx_copy_selected",
                      static_cast<int>(m_selection.size()));
        if (ImGui::MenuItem(copySelectedLabel, "Ctrl+C", false, !m_selection.empty()))
            ImGui::SetClipboardText(SelectedText().c_str());
        if (ImGui::MenuItem("Copy This Line"))
            ImGui::SetClipboardText(LogListCopyTextOf(line).c_str());
        if (ImGui::MenuItem("Copy All Visible", nullptr, false, !m_rows.empty()))
            ImGui::SetClipboardText(VisibleText().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, !m_rows.empty()))
            SelectRange(0, static_cast<int>(m_rows.size()) - 1, false);
        if (ImGui::MenuItem("Deselect All", nullptr, false, !m_selection.empty()))
            m_selection.clear();
        ImGui::Separator();
        if (ImGui::MenuItem("Open Source", "Double-click", false, canOpen))
            OpenSourceInExternalEditor(line.file, line.line);
        ImGui::EndPopup();
    }

    ImGui::PopID();
}

void LogListView::DrawList(const ImVec2& size)
{
    ImGui::PushID(m_id.c_str());

    if (m_rowsDirty || m_cachedFilter != m_filter.data() || m_cachedShow != m_show)
        RebuildRows();
    const bool linesChanged = std::exchange(m_linesChanged, false);

    ImGui::BeginChild("##list", size, true, ImGuiWindowFlags_HorizontalScrollbar);

    if (m_rows.empty()) {
        ImGui::TextDisabled("%s", m_lines.empty() ? m_emptyText
                                                  : "No lines match the current filters.");
    } else {
        const float rowStep = LogListRowHeight() + 1.0f;

        if (m_hasReveal) {
            m_hasReveal = false;
            if (const int row = RowIndexOf(m_revealId); row >= 0) {
                m_selection.clear();
                m_selection.insert(m_revealId);
                m_anchorId  = m_revealId;
                m_hasAnchor = true;
                ImGui::SetScrollY((std::max)(0.0f, static_cast<float>(row) * rowStep
                                                   - ImGui::GetWindowHeight() * 0.4f));
            }
        }

        // 行高は LogListRowHeight が持っているので、既定の行送りは 1px に詰める。
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 1.0f));
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()), rowStep);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                DrawRow(row);
        }
        clipper.End();
        ImGui::PopStyleVar();
    }

    // WHY この子ウィンドウのフォーカスで見るか: 1 つのパネルに一覧が 2 つ並ぶので、
    //     パネル全体で拾うとどちらの選択をコピーするのか決まらない。
    // 検索欄を編集中の Ctrl+C は入力欄のコピーなので横取りしない。
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !m_selection.empty())
            ImGui::SetClipboardText(SelectedText().c_str());
        if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !m_rows.empty())
            SelectRange(0, static_cast<int>(m_rows.size()) - 1, false);
    }

    // 下端に居るときだけ追従する。上へ読みに行った人を引き戻さない。
    if (autoScroll && linesChanged && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopID();
}

} // namespace fbzz::editor
