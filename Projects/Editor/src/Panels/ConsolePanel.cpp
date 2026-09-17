/// @file    ConsolePanel.cpp
/// @brief   ログエントリをフィルタ・検索・表示するコンソールパネル。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/SourceOpen.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace fbzz::editor {

namespace {

/// ログレベルを表す色 (左のバー・バッジ・件数トグルが共有する)。
ImVec4 LogLevelColor(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::DEBUG:     return EditorTheme::Color(ThemeColor::TextFaint);
    case core::LogLevel::INFO:      return EditorTheme::Color(ThemeColor::Info);
    case core::LogLevel::WARNING:   return EditorTheme::Color(ThemeColor::Warning);
    case core::LogLevel::LOG_ERROR: return EditorTheme::Color(ThemeColor::Danger);
    default:                        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

/// 本文そのものの色。
/// @note 重大度は行の左バーとバッジで示すため、本文は常に通常色に固定する。
ImVec4 LogMessageColor(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::DEBUG:     return EditorTheme::Color(ThemeColor::TextMuted);
    case core::LogLevel::WARNING:   return EditorTheme::Color(ThemeColor::Warning);
    case core::LogLevel::LOG_ERROR: return EditorTheme::Color(ThemeColor::Danger);
    case core::LogLevel::INFO:
    default:                        return EditorTheme::Color(ThemeColor::Text);
    }
}

/// 行の背景。警告とエラーだけ、レベル色をごく薄く敷いて面として目立たせる。
/// 0 を返す行は背景を描かない。
ImU32 LogRowTint(core::LogLevel level, bool selected)
{
    /// @note 選択色を濁らせない
    if (selected) return 0;
    switch (level) {
    case core::LogLevel::WARNING:   return EditorTheme::ColorU32(ThemeColor::Warning, 0.10f);
    case core::LogLevel::LOG_ERROR: return EditorTheme::ColorU32(ThemeColor::Danger,  0.14f);
    default:                        return 0;
    }
}

/// 一覧 1 行の高さ。
/// @note Selectable の既定行間 0 だと文字が帯状になるため広げる。
/// @note ImGuiListClipper は等高前提で間引くので、この値を Begin へも渡すこと。
float ConsoleRowHeight()
{
    return ImGui::GetTextLineHeight() + 6.0f;
}

/// 色に頼らずレベルを読めるようにするバッジ。記号が使えない環境では 1 文字へ落ちる。
/// @note 形の違いは色より速く目に入るため、色覚に依らない手がかりとして優先する。
const char* LogLevelBadge(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::DEBUG:     return icons::Or(icons::kDetail,  "D");
    case core::LogLevel::INFO:      return icons::Or(icons::kInfo,    "I");
    case core::LogLevel::WARNING:   return icons::Or(icons::kWarning, "W");
    case core::LogLevel::LOG_ERROR: return icons::Or(icons::kError,   "E");
    default:                        return "?";
    }
}

/// バッジ列の幅。記号は 1 文字より広いので、本文の開始 X をここで決める。
float LogLevelBadgeWidth()
{
    return ImGui::CalcTextSize(LogLevelBadge(core::LogLevel::WARNING)).x;
}

/// 詳細ペインの見出しに出すレベル名。
const char* LogLevelName(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::DEBUG:     return "DEBUG";
    case core::LogLevel::INFO:      return "INFO";
    case core::LogLevel::WARNING:   return "WARNING";
    case core::LogLevel::LOG_ERROR: return "ERROR";
    default:                        return "LOG";
    }
}

/// 幅 width に収まるよう改行を挿し込んだ文字列を返す。
/// @note InputTextMultiline は折り返しを持たないため、表示直前に自前で折る。
std::string WrapText(const std::string& text, float width)
{
    if (width <= 1.0f) return text;

    ImFont*     font     = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();

    std::string out;
    out.reserve(text.size() + text.size() / 32);

    const char* cursor = text.c_str();
    const char* end    = cursor + text.size();
    while (cursor < end) {
        const char* lineEnd = std::find(cursor, end, '\n');
        /// @note 論理行 1 本を、収まる幅ごとに切り出す。
        while (cursor < lineEnd) {
            const char* wrap = font->CalcWordWrapPosition(fontSize, cursor, lineEnd, width);
            /// @note 1 文字も置けない幅のときは無限ループになるので、最低 1 文字は進める。
            if (wrap == cursor) ++wrap;
            out.append(cursor, wrap);
            cursor = wrap;
            /// @note 折り返し位置の空白は行頭に残さない。
            while (cursor < lineEnd && *cursor == ' ') ++cursor;
            if (cursor < lineEnd) out += '\n';
        }
        if (lineEnd < end) {
            out += '\n';
            cursor = lineEnd + 1;
        }
    }
    return out;
}

/// 一覧では 1 行に収めたいので、改行以降を省略した 1 行表現を作る。
/// @note 複数行だと ImGuiListClipper の等高前提が崩れるため。全文は詳細ペインで読ませる。
std::string FirstLineOf(const std::string& message)
{
    const std::size_t nl = message.find('\n');
    if (nl == std::string::npos) return message;
    return message.substr(0, nl) + "  ...";
}

} // namespace

ConsolePanel::ConsolePanel(ConsoleSink& sink) : m_sink(sink) {}

void ConsolePanel::OnInit(EditorContext& /*ctx*/)
{
    core::Logger::AddSink(&m_sink);
}

void ConsolePanel::OnShutdown()
{
    core::Logger::RemoveSink(&m_sink);
}

void ConsolePanel::OnLoadSettings(const EditorSettings& settings)
{
    m_showDebug   = settings.consoleShowDebug;
    m_showInfo    = settings.consoleShowInfo;
    m_showWarn    = settings.consoleShowWarn;
    m_showError   = settings.consoleShowError;
    m_autoScroll  = settings.consoleAutoScroll;
    m_collapse    = settings.consoleCollapse;
    m_clearOnPlay = settings.consoleClearOnPlay;
    m_showDetail  = settings.consoleShowDetail;
    m_detailRatio = std::clamp(settings.consoleDetailRatio, 0.10f, 0.80f);
}

void ConsolePanel::OnSaveSettings(EditorSettings& settings) const
{
    settings.consoleShowDebug   = m_showDebug;
    settings.consoleShowInfo    = m_showInfo;
    settings.consoleShowWarn    = m_showWarn;
    settings.consoleShowError   = m_showError;
    settings.consoleAutoScroll  = m_autoScroll;
    settings.consoleCollapse    = m_collapse;
    settings.consoleClearOnPlay = m_clearOnPlay;
    settings.consoleShowDetail  = m_showDetail;
    settings.consoleDetailRatio = m_detailRatio;
}

bool ConsolePanel::FiltersChanged() const
{
    return m_cachedShowDebug != m_showDebug
        || m_cachedShowInfo  != m_showInfo
        || m_cachedShowWarn  != m_showWarn
        || m_cachedShowError != m_showError
        || m_cachedCollapse  != m_collapse
        || m_cachedFilter    != std::string(m_filterBuf.data());
}

void ConsolePanel::RebuildRows()
{
    const std::string filter(m_filterBuf.data());
    /// @note GetEntries()[i] の通し番号は oldest + i。リングバッファから押し出された行は
    ///       ここより小さい番号になるので、選択から落とす判定にも使える。
    const std::uint64_t oldest = m_sink.GetOldestSequence();

    m_rows.clear();
    m_visibleLogText.clear();
    m_levelCounts.fill(0);

    /// @note Collapse 時の集約先を引くための索引。key はレベルと本文の組。
    /// @note 連続していない同一メッセージも 1 行へまとめるため、前方の行を引き直す索引が要る。
    std::unordered_map<std::string, std::size_t> collapseIndex;

    const auto& entries = m_sink.GetEntries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const core::LogEntry& entry = entries[i];

        /// @note 件数はフィルタと無関係に数える (フィルタで隠しても総数は把握したい)。
        const int levelIndex = static_cast<int>(entry.level);
        if (levelIndex >= 0 && levelIndex < static_cast<int>(m_levelCounts.size()))
            ++m_levelCounts[static_cast<std::size_t>(levelIndex)];

        if (entry.level == core::LogLevel::DEBUG     && !m_showDebug) continue;
        if (entry.level == core::LogLevel::INFO      && !m_showInfo)  continue;
        if (entry.level == core::LogLevel::WARNING   && !m_showWarn)  continue;
        if (entry.level == core::LogLevel::LOG_ERROR && !m_showError) continue;
        /// @note 他の検索欄 (Add Component / Asset Browser) と同じく大文字小文字を無視する。
        if (!filter.empty() && !util::StringUtils::ContainsCI(entry.message, filter)) continue;

        if (m_collapse) {
            std::string key;
            key.reserve(entry.message.size() + 2);
            key += static_cast<char>('0' + static_cast<int>(entry.level));
            /// @note レベルと本文の区切り (本文に現れない制御文字)
            key += '\x1f';
            key += entry.message;

            const auto it = collapseIndex.find(key);
            if (it != collapseIndex.end()) {
                ++m_rows[it->second].count;
                continue;
            }
            collapseIndex.emplace(std::move(key), m_rows.size());
        }

        m_rows.push_back(Row{ i, oldest + i, 1, entry.level });
        m_visibleLogText += entry.message;
        m_visibleLogText += '\n';
    }

    m_cachedRevision  = m_sink.GetRevision();
    m_cachedFilter    = filter;
    m_cachedShowDebug = m_showDebug;
    m_cachedShowInfo  = m_showInfo;
    m_cachedShowWarn  = m_showWarn;
    m_cachedShowError = m_showError;
    m_cachedCollapse  = m_collapse;

    /// @note フィルタで隠れただけの行は選択を保つ。捨てるのはバッファから消えた行だけ。
    std::erase_if(m_selection, [oldest](std::uint64_t sequence) { return sequence < oldest; });
    if (m_anchorSequence < oldest) m_anchorSequence = 0;
    if (m_detailSequence < oldest) m_detailSequence = 0;
}

int ConsolePanel::RowIndexOf(std::uint64_t sequence) const
{
    if (sequence == 0) return -1;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].sequence == sequence) return static_cast<int>(i);
    }
    return -1;
}

void ConsolePanel::SelectRange(int fromRow, int toRow, bool additive)
{
    if (!additive) m_selection.clear();
    if (m_rows.empty()) return;
    if (fromRow > toRow) std::swap(fromRow, toRow);
    fromRow = std::max(fromRow, 0);
    toRow   = std::min(toRow, static_cast<int>(m_rows.size()) - 1);
    for (int row = fromRow; row <= toRow; ++row)
        m_selection.insert(m_rows[static_cast<std::size_t>(row)].sequence);
}

void ConsolePanel::ApplyRowClick(int row)
{
    const ImGuiIO& io = ImGui::GetIO();
    const std::uint64_t sequence = m_rows[static_cast<std::size_t>(row)].sequence;
    const int anchor = RowIndexOf(m_anchorSequence);

    if (io.KeyShift && anchor >= 0) {
        /// @note 起点を動かすと Shift クリックのたびに範囲が「そこから」になり選び直せない。
        SelectRange(anchor, row, io.KeyCtrl);
    } else if (io.KeyCtrl) {
        if (!m_selection.insert(sequence).second) m_selection.erase(sequence);
        m_anchorSequence = sequence;
    } else {
        m_selection.clear();
        m_selection.insert(sequence);
        m_anchorSequence = sequence;
    }
    m_detailSequence = sequence;
}

std::string ConsolePanel::SelectedText() const
{
    const auto& entries = m_sink.GetEntries();
    std::string text;
    /// @note m_rows の順で拾う。選択は集合なので、そのまま回すと画面と並びが変わる。
    for (const Row& row : m_rows) {
        if (!m_selection.contains(row.sequence)) continue;
        if (row.entryIndex >= entries.size()) continue;
        text += entries[row.entryIndex].message;
        text += '\n';
    }
    return text;
}

void ConsolePanel::CopySelection() const
{
    const std::string text = SelectedText();
    if (!text.empty()) ImGui::SetClipboardText(text.c_str());
}

void ConsolePanel::JumpToSource(EditorContext& ctx, const std::string& message)
{
    std::string file;
    int         line = 0;
    std::size_t bodyOffset = 0;
    if (!ParseLogLocationPrefix(message, file, line, bodyOffset)) return;

    /// @note Logger はベース名しか残さないため、プロジェクトとエンジンのソースツリーから引き直す。
    const std::vector<std::string> roots = {
        ctx.scriptsSourceDir,
        ctx.projectRoot,
        ctx.engineRoot
    };
    const std::string resolved = ResolveSourceFileByName(file, roots);
    if (resolved.empty()) {
        FBZZ_LOG_WARN("Console: source file not found for '%s'", file.c_str());
        return;
    }
    OpenSourceInExternalEditor(resolved, line);
}

void ConsolePanel::DrawDetailPane(EditorContext& ctx)
{
    const auto& entries = m_sink.GetEntries();
    const int detailRow = RowIndexOf(m_detailSequence);
    if (detailRow < 0) {
        ImGui::TextDisabled("Select a log line to see the full message. "
                            "Shift+click for a range, Ctrl+C to copy.");
        return;
    }
    const std::size_t entryIndex = m_rows[static_cast<std::size_t>(detailRow)].entryIndex;
    if (entryIndex >= entries.size()) {
        ImGui::TextDisabled("This entry has been discarded from the ring buffer.");
        return;
    }

    const core::LogEntry& entry = entries[entryIndex];
    const Row&            r     = m_rows[static_cast<std::size_t>(detailRow)];

    std::string file;
    int         line = 0;
    std::size_t bodyOffset = 0;
    const bool hasLocation = ParseLogLocationPrefix(entry.message, file, line, bodyOffset);

    /// @name 見出し: レベル / 件数 / 発生位置 / 操作
    /// @note 読む前に要る情報 (レベル・件数・発生位置) は本文の外へ出す。
    ImGui::PushStyleColor(ImGuiCol_Text, LogLevelColor(entry.level));
    ImGui::TextUnformatted(LogLevelName(entry.level));
    ImGui::PopStyleColor();
    ImGui::SameLine();

    if (r.count > 1) {
        ImGui::TextDisabled("x%d", r.count);
        ImGui::SameLine();
    }

    if (hasLocation) {
        char label[320];
        std::snprintf(label, sizeof(label), "%s:%d###detail_open", file.c_str(), line);
        if (ImGui::SmallButton(label)) JumpToSource(ctx, entry.message);
    } else {
        ImGui::TextDisabled("(no source location)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##detail"))
        ImGui::SetClipboardText(entry.message.c_str());

    ImGui::Separator();

    /// @name 本文
    /// @note 重大度は見出しが担うため本文は通常色に固定する。
    /// @note TextWrapped は選択・部分コピーができないため、読み取り専用の InputTextMultiline を使う。
    const float avail = ImGui::GetContentRegionAvail().x
                      - ImGui::GetStyle().FramePadding.x * 2.0f
                      - ImGui::GetStyle().ScrollbarSize;
    if (m_detailTextSequence != r.sequence || m_detailTextWidth != avail) {
        const std::string body = hasLocation ? entry.message.substr(bodyOffset) : entry.message;
        m_detailText         = WrapText(body, avail);
        m_detailTextSequence = r.sequence;
        m_detailTextWidth    = avail;
    }

    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Text));
    ImGui::InputTextMultiline("##detail_body", m_detailText.data(), m_detailText.size() + 1,
                              ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor(2);
}

void ConsolePanel::DrawDetailSplitter()
{
    const float thickness = 4.0f;
    ImGui::InvisibleButton("##detail_splitter",
                           ImVec2(ImGui::GetContentRegionAvail().x, thickness));

    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    if (ImGui::IsItemActive()) {
        /// @note 掴んだ量をパネル高で正規化する。パネルを広げても掴み心地が変わらない。
        const float panelH = std::max(ImGui::GetWindowHeight(), 1.0f);
        m_detailRatio = std::clamp(m_detailRatio - ImGui::GetIO().MouseDelta.y / panelH,
                                   0.10f, 0.80f);
    }

    const ImU32 color = (ImGui::IsItemActive() || ImGui::IsItemHovered())
                      ? EditorTheme::ColorU32(ThemeColor::Accent)
                      : EditorTheme::ColorU32(ThemeColor::Border);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled({ min.x, min.y + 1.0f },
                                             { max.x, max.y - 1.0f }, color);
}

void ConsolePanel::DrawLogRow(EditorContext& ctx, int row, const core::LogEntry& entry)
{
    const Row& r = m_rows[static_cast<std::size_t>(row)];
    const bool selected = m_selection.contains(r.sequence);

    ImGui::PushID(row);

    const float rowH = ConsoleRowHeight();

    /// @note 本文をそのまま Selectable のラベルにすると "##" を含むログ (シェーダー診断等) で
    ///       以降が表示されなくなる。当たり判定は ID だけの Selectable に任せ本文は重ね描きする。
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##row", selected,
                          ImGuiSelectableFlags_AllowDoubleClick, { 0.0f, rowH }))
        ApplyRowClick(row);

    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    ImDrawList*  dl     = ImGui::GetWindowDrawList();

    /// @note 警告・エラーだけ面で塗り、走査で拾えるようにする。
    if (const ImU32 tint = LogRowTint(r.level, selected); tint != 0)
        dl->AddRectFilled(rowMin, rowMax, tint);

    /// @note 左端のレベルバー。色が読めない環境でも次のバッジ文字で判別できる。
    const ImVec4 levelColor = LogLevelColor(r.level);
    dl->AddRectFilled(rowMin, { rowMin.x + 3.0f, rowMax.y },
                      ImGui::ColorConvertFloat4ToU32(levelColor));

    /// @name 押下・ドラッグ・右クリックの取り回し (見た目より前に判定を済ませる)
    /// @note Selectable は離した時に true を返すため、それを待たず押下の瞬間に起点を取る。
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !io.KeyShift && !io.KeyCtrl)
        m_anchorSequence = r.sequence;

    /// @note 押したままなぞって範囲を伸ばす。飛ばした行も起点からの範囲で埋まるため、
    ///       クリッパーが間引いた行が選択から抜け落ちることはない。
    /// @note ドラッグ中は押した行が ActiveId を握るため、他の行の IsItemHovered には
    ///       AllowWhenBlockedByActiveItem が要る。
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const int anchor = RowIndexOf(m_anchorSequence);
        if (anchor >= 0) {
            SelectRange(anchor, row, false);
            m_detailSequence = r.sequence;
        }
    }

    /// @note 右クリックは選択外の行なら選び直す。選択内ならまとめて扱いたいので触らない。
    /// @note ポップアップは開いている間ずっと毎フレーム走るため、そこで選び直すと
    ///       複数選択が毎フレーム 1 行へ潰れる。
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (!m_selection.contains(r.sequence)) {
            m_selection.clear();
            m_selection.insert(r.sequence);
            m_anchorSequence = r.sequence;
        }
        m_detailSequence = r.sequence;
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_detailSequence = r.sequence;
        JumpToSource(ctx, entry.message);
    }

    /// @name 本文
    const float textY = origin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
    float x = rowMin.x + 10.0f;

    /// @note レベルバッジ。固定幅で置き、本文の開始位置を揃える。
    const char* badge = LogLevelBadge(r.level);
    dl->AddText({ x, textY }, ImGui::ColorConvertFloat4ToU32(levelColor), badge);
    x += LogLevelBadgeWidth() + 8.0f;

    /// @note "[File.cpp:123]" は本文と分けて淡色で出す。本文と同じ濃さだと毎行読み飛ばす手間がかかる。
    std::string file;
    int         line = 0;
    std::size_t bodyOffset = 0;
    const bool hasLocation = ParseLogLocationPrefix(entry.message, file, line, bodyOffset);
    if (hasLocation) {
        char location[288];
        std::snprintf(location, sizeof(location), "%s:%d", file.c_str(), line);
        dl->AddText({ x, textY }, EditorTheme::ColorU32(ThemeColor::TextFaint), location);
        x += ImGui::CalcTextSize(location).x + 10.0f;
    }

    const std::string body = FirstLineOf(
        hasLocation ? entry.message.substr(bodyOffset) : entry.message);
    dl->AddText({ x, textY }, ImGui::ColorConvertFloat4ToU32(LogMessageColor(r.level)),
                body.c_str(), body.c_str() + body.size());

    /// @note Collapse 件数バッジを行の右端へ重ねる。
    if (r.count > 1) {
        char countText[16];
        std::snprintf(countText, sizeof(countText), "%d", r.count);
        const ImVec2 badgeSize = ImGui::CalcTextSize(countText);
        const ImVec2 pos = { rowMax.x - badgeSize.x - 12.0f, textY };
        dl->AddRectFilled({ pos.x - 5.0f, pos.y - 1.0f },
                          { pos.x + badgeSize.x + 5.0f, pos.y + badgeSize.y + 1.0f },
                          EditorTheme::ColorU32(ThemeColor::SurfaceRaised, 0.95f), 4.0f);
        dl->AddText(pos, EditorTheme::ColorU32(ThemeColor::Text), countText);
    }

    if (ImGui::BeginPopupContextItem("##log_ctx")) {
        char copySelectedLabel[64];
        std::snprintf(copySelectedLabel, sizeof(copySelectedLabel),
                      "Copy Selected (%d)###ctx_copy_selected",
                      static_cast<int>(m_selection.size()));
        if (ImGui::MenuItem(copySelectedLabel, "Ctrl+C", false, !m_selection.empty()))
            CopySelection();
        if (ImGui::MenuItem("Copy This Line"))
            ImGui::SetClipboardText(entry.message.c_str());
        if (ImGui::MenuItem("Copy All Visible"))
            ImGui::SetClipboardText(m_visibleLogText.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, !m_rows.empty()))
            SelectRange(0, static_cast<int>(m_rows.size()) - 1, false);
        if (ImGui::MenuItem("Deselect All", nullptr, false, !m_selection.empty()))
            m_selection.clear();
        ImGui::Separator();
        if (ImGui::MenuItem("Open Source", nullptr, false, hasLocation))
            JumpToSource(ctx, entry.message);
        ImGui::EndPopup();
    }

    ImGui::PopID();
}

void ConsolePanel::DrawLevelToggle(const char* label, core::LogLevel level, bool& enabled)
{
    /// @note 押下状態 = そのレベルを表示中。件数を同じボタンに載せ、件数と表示可否を 1 か所で確認できるようにする。
    const int count = m_levelCounts[static_cast<std::size_t>(level)];
    char text[32];
    std::snprintf(text, sizeof(text), "%s %d", label, count);

    const ImVec4 levelColor = LogLevelColor(level);
    if (enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              { levelColor.x, levelColor.y, levelColor.z, 0.30f });
        ImGui::PushStyleColor(ImGuiCol_Text, levelColor);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Surface));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
    }
    if (ImGui::Button(text)) enabled = !enabled;
    ImGui::PopStyleColor(2);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s: %d 件\nクリックで表示 / 非表示",
                          label, count);
    }
}

bool ConsolePanel::DrawToolbar()
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float sp = st.ItemSpacing.x;
    const auto btnW = [&](const char* s) {
        return ImGui::CalcTextSize(s).x + st.FramePadding.x * 2.0f;
    };

    /// @note レベルのトグル。DEBUG は表示フィルタであると同時に Logger の収集レベルも動かす
    ///       (収集していないものは表示しようがない)。実際に変わったフレームだけ反映する。
    const bool debugBefore = m_showDebug;
    DrawLevelToggle("D", core::LogLevel::DEBUG, m_showDebug);
    if (m_showDebug != debugBefore)
        core::Logger::SetMinLevel(m_showDebug ? core::LogLevel::DEBUG : core::LogLevel::INFO);
    ImGui::SameLine(0.0f, 4.0f);
    DrawLevelToggle("I", core::LogLevel::INFO,      m_showInfo);  ImGui::SameLine(0.0f, 4.0f);
    DrawLevelToggle("W", core::LogLevel::WARNING,   m_showWarn);  ImGui::SameLine(0.0f, 4.0f);
    DrawLevelToggle("E", core::LogLevel::LOG_ERROR, m_showError); ImGui::SameLine(0.0f, sp);

    /// @note 右側のクラスタ幅を実測し、検索欄を残り全部へ伸ばす (固定幅だと長い検索語の全体が見えない)。
    const int selectedCount = static_cast<int>(m_selection.size());
    char copyLabel[64];
    std::snprintf(copyLabel, sizeof(copyLabel), "Copy (%d)###console_copy_selected",
                  selectedCount);
    char copyLabelVisible[64];
    std::snprintf(copyLabelVisible, sizeof(copyLabelVisible), "Copy (%d)", selectedCount);

    float rightW = btnW("Options") + sp + btnW(copyLabelVisible) + sp + btnW("Clear");
    const float searchW = std::max(120.0f, ImGui::GetContentRegionAvail().x - rightW - sp);

    ImGui::SetNextItemWidth(searchW);
    ImGui::InputTextWithHint("##console_filter", "Search logs...",
                             m_filterBuf.data(), m_filterBuf.size());
    ImGui::SameLine(0.0f, sp);

    /// @note 表示オプション 4 つは一度決めたらほとんど触らないため 1 つのポップアップへ畳む。
    if (ImGui::Button("Options")) ImGui::OpenPopup("##console_options");
    if (ImGui::BeginPopup("##console_options")) {
        ImGui::Checkbox("Collapse", &m_collapse);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Merge identical messages into one row with a count");
        ImGui::Checkbox("Clear on Play", &m_clearOnPlay);
        ImGui::Checkbox("Auto Scroll", &m_autoScroll);
        ImGui::Checkbox("Details", &m_showDetail);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, sp);

    ImGui::BeginDisabled(selectedCount == 0);
    /// @note "###" 以降が ID。件数で表示は変わっても ID は動かさない
    ///       (ラベルがそのまま ID だと、件数が変わった瞬間に別ボタン扱いになる)。
    const bool copySelected = ImGui::Button(copyLabel);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Click a line, Shift+click (or drag) for a range, Ctrl+click to add.\n"
                          "Ctrl+A selects every visible line, Ctrl+C copies the selection.\n"
                          "Right-click a line for Copy All Visible.");
    }
    ImGui::SameLine(0.0f, sp);

    if (ImGui::Button("Clear")) {
        m_sink.Clear();
        m_selection.clear();
        m_anchorSequence = 0;
        m_detailSequence = 0;
    }

    return copySelected;
}

void ConsolePanel::OnRenderContent(EditorContext& ctx)
{
    /// @name Clear on Play: 編集中 → Play へ遷移した瞬間だけ消す
    /// @note 毎フレーム状態を比較しエッジでのみ実行する。Play 中の再入で消え続けないようにする。
    const bool inEditor = (ctx.playMode == nullptr) || ctx.playMode->IsInEditor();
    if (m_clearOnPlay && m_wasInEditor && !inEditor) {
        m_sink.Clear();
        m_selection.clear();
        m_anchorSequence = 0;
        m_detailSequence = 0;
    }
    m_wasInEditor = inEditor;

    const bool copySelectedRequested = DrawToolbar();
    ImGui::Separator();

    /// @note フィルタ条件かログ内容が変わったときだけ、表示行を作り直す。
    /// @note logChanged は Auto Scroll の追従判定にも使う。行数比較だと、リングバッファが満杯で
    ///       「古い行が落ちて新しい行が入る」状況を新着として検出できない。
    const bool logChanged = (m_cachedRevision != m_sink.GetRevision());
    if (logChanged || FiltersChanged())
        RebuildRows();

    /// @note ツールバーは RebuildRows より前に描くため、押した瞬間の m_visibleLogText / 選択は
    ///       1 フレーム古い可能性がある。コピーは RebuildRows の後で行う。
    if (copySelectedRequested)
        CopySelection();

    /// @name ログ一覧
    /// @note ImVec2(0, 0) は「残り全部」を意味するため、詳細ペイン分の高さを先に引いておかないと
    ///       一覧が下端まで伸びて詳細が押し出される。
    const float splitterH = m_showDetail ? 4.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f : 0.0f;
    const float detailH   = m_showDetail
                          ? ImGui::GetContentRegionAvail().y * m_detailRatio + splitterH
                          : 0.0f;
    ImVec2 logRegion = ImGui::GetContentRegionAvail();
    logRegion.y -= detailH;
    if (logRegion.x < 1.0f) logRegion.x = 1.0f;
    if (logRegion.y < 1.0f) logRegion.y = 1.0f;

    ImGui::BeginChild("##log", logRegion, true, ImGuiWindowFlags_HorizontalScrollbar);

    if (m_rows.empty()) {
        const bool hasAny = !m_sink.GetEntries().empty();
        ImGui::TextDisabled("%s", hasAny
            ? "No logs match the current filters (level toggles / filter text)."
            : "No logs yet. Engine and script messages will appear here.");
    } else {
        const auto& entries = m_sink.GetEntries();

        /// @note 全行を毎フレーム ImGui へ積むと無駄な文字列処理とジオメトリ生成が走るため、
        ///       可視範囲だけ描く。行間は 1px のみ (既定の 5px を足すと 1 画面の行数が減る)。
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 1.0f));

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()), ConsoleRowHeight() + 1.0f);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const Row& r = m_rows[static_cast<std::size_t>(row)];
                if (r.entryIndex >= entries.size()) continue;
                const core::LogEntry& entry = entries[r.entryIndex];

                DrawLogRow(ctx, row, entry);
            }
        }
        clipper.End();

        ImGui::PopStyleVar();
    }

    /// @note Ctrl+C / Ctrl+A は Console にフォーカスがある間だけ拾う。HotkeyManager の
    ///       edit.copy / select.all は Scene View / Hierarchy に閉じ、Console では発火しない。
    /// @note WantTextInput で降りる: 検索欄編集中の Ctrl+C は入力欄のコピー。
    /// @note Shortcut() でなく素の IsKeyPressed を使う: Shortcut は子ウィンドウの focused 状態で
    ///       決まり、ツールバー操作直後など一覧側へフォーカスが入っていないフレームで落ちるため。
    const ImGuiIO& shortcutIo = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !shortcutIo.WantTextInput && shortcutIo.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            CopySelection();
        if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !m_rows.empty())
            SelectRange(0, static_cast<int>(m_rows.size()) - 1, false);
    }

    /// @note Auto Scroll: 最下部に居るときだけ追従する。無条件だと過去ログを読もうと
    ///       上へスクロールしても毎フレーム引き戻される (Unity / VSCode の出力ペインと同じ挙動)。
    if (m_autoScroll && logChanged && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    /// @name 詳細ペイン
    if (m_showDetail) {
        DrawDetailSplitter();
        ImGui::BeginChild("##log_detail", ImVec2(0, 0), true);
        DrawDetailPane(ctx);
        ImGui::EndChild();
    }
}

} // namespace fbzz::editor
