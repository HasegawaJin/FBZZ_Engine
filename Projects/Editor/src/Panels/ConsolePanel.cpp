// FBZZ Engine
// ConsolePanel.cpp | fbzz::editor
// ログエントリをフィルタ・検索・表示するコンソールパネル
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/SourceOpen.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace fbzz::editor {

namespace {

// ログレベルごとの表示色を返す。
// WHY: Console では重大度を視線だけで判別できることが重要なため、本文全体をレベル色で描画する。
ImVec4 LogLevelColor(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::DEBUG:     return EditorTheme::Color(ThemeColor::Secondary);
    case core::LogLevel::INFO:      return EditorTheme::Color(ThemeColor::Text);
    case core::LogLevel::WARNING:   return EditorTheme::Color(ThemeColor::Warning);
    case core::LogLevel::LOG_ERROR: return EditorTheme::Color(ThemeColor::Danger);
    default:                        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

// 一覧では 1 行に収めたいので、改行以降を省略した 1 行表現を作る。
// WHY: 複数行のログ (スタックダンプ等) がそのまま並ぶとリストの行高が不揃いになり、
//      ImGuiListClipper の等高前提も崩れる。全文は詳細ペインで読ませる。
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
    // GetEntries()[i] の通し番号は oldest + i。リングバッファから押し出された行は
    // ここより小さい番号になるので、選択から落とす判定にも使える。
    const std::uint64_t oldest = m_sink.GetOldestSequence();

    m_rows.clear();
    m_visibleLogText.clear();
    m_warnCount  = 0;
    m_errorCount = 0;

    // Collapse 時の集約先を引くための索引。key はレベルと本文の組。
    // WHY: Unity の Collapse は連続した重複だけでなく、離れて出た同一メッセージも
    //      1 行へまとめて件数を出す。ループ中に前方の行を引き直す必要があるため索引を持つ。
    std::unordered_map<std::string, std::size_t> collapseIndex;

    const auto& entries = m_sink.GetEntries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const core::LogEntry& entry = entries[i];

        // バッジ用の総数はフィルタと無関係に数える (フィルタで隠しても件数は把握したい)。
        if (entry.level == core::LogLevel::WARNING)   ++m_warnCount;
        if (entry.level == core::LogLevel::LOG_ERROR) ++m_errorCount;

        if (entry.level == core::LogLevel::DEBUG     && !m_showDebug) continue;
        if (entry.level == core::LogLevel::INFO      && !m_showInfo)  continue;
        if (entry.level == core::LogLevel::WARNING   && !m_showWarn)  continue;
        if (entry.level == core::LogLevel::LOG_ERROR && !m_showError) continue;
        // WHY: 他の検索欄 (Add Component / Asset Browser) は ContainsCI で
        //      大文字小文字を無視する。Console だけ区別すると挙動が食い違う。
        if (!filter.empty() && !util::StringUtils::ContainsCI(entry.message, filter)) continue;

        if (m_collapse) {
            std::string key;
            key.reserve(entry.message.size() + 2);
            key += static_cast<char>('0' + static_cast<int>(entry.level));
            key += '\x1f';   // レベルと本文の区切り (本文に現れない制御文字)
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

    // WHY フィルタで消えた行を選択から外さないか: レベルトグルを切り替えて戻したときに
    //      選択が生き残っていてほしい。捨てるのは実体がバッファから消えた行だけにする。
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
        // WHY 起点を動かさないか: 動かすと Shift クリックのたびに範囲が「そこから」に
        //     なり、行き過ぎたぶんを戻して選び直せない。起点は素のクリックだけが決める。
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
    // m_rows の順で拾う。選択は集合なので、そのまま回すと画面と並びが変わる。
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

    // Logger はベース名しか残さないため、プロジェクトとエンジンのソースツリーから引き直す。
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

    std::string file;
    int         line = 0;
    std::size_t bodyOffset = 0;
    const bool hasLocation = ParseLogLocationPrefix(entry.message, file, line, bodyOffset);

    if (hasLocation) {
        char label[320];
        std::snprintf(label, sizeof(label), "Open %s:%d", file.c_str(), line);
        if (ImGui::SmallButton(label)) JumpToSource(ctx, entry.message);
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Copy##detail"))
        ImGui::SetClipboardText(entry.message.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(hasLocation ? "(double-click a line to jump)"
                                    : "(no source location in this message)");

    ImGui::Separator();
    // WHY: 折り返して全文を出す。一覧側は 1 行に切り詰めているため、
    //      長いメッセージを読む唯一の場所がここになる。
    ImGui::PushStyleColor(ImGuiCol_Text, LogLevelColor(entry.level));
    ImGui::TextWrapped("%s", entry.message.c_str());
    ImGui::PopStyleColor();
}

void ConsolePanel::OnRenderContent(EditorContext& ctx)
{
    // --- Clear on Play: 編集中 → Play へ遷移した瞬間だけ消す ---
    // WHY: Play 中に出たログだけを見たい場面が多い。毎フレーム状態を比較して
    //      エッジでのみ実行し、Play 中の再入で消え続けないようにする。
    const bool inEditor = (ctx.playMode == nullptr) || ctx.playMode->IsInEditor();
    if (m_clearOnPlay && m_wasInEditor && !inEditor) {
        m_sink.Clear();
        m_selection.clear();
        m_anchorSequence = 0;
        m_detailSequence = 0;
    }
    m_wasInEditor = inEditor;

    // --- ツールバー 1 段目: レベルトグルと件数バッジ ---
    // WHY: DEBUG チェックボックスは表示フィルタであると同時に、Logger の収集レベルも
    //      上げ下げする必要がある (収集していないものは表示しようがない)。ただし
    //      毎フレーム SetMinLevel を呼ぶとグローバル状態を常時上書きしてしまうため、
    //      チェックが実際に変わったフレームだけ反映する。
    if (ImGui::Checkbox("DEBUG", &m_showDebug))
        core::Logger::SetMinLevel(m_showDebug ? core::LogLevel::DEBUG : core::LogLevel::INFO);
    ImGui::SameLine();
    ImGui::Checkbox("INFO",  &m_showInfo);  ImGui::SameLine();
    ImGui::Checkbox("WARN",  &m_showWarn);  ImGui::SameLine();
    ImGui::Checkbox("ERROR", &m_showError); ImGui::SameLine();

    ImGui::TextDisabled("|"); ImGui::SameLine();
    ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "%d W", m_warnCount);
    ImGui::SameLine();
    ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger),  "%d E", m_errorCount);
    ImGui::SameLine();

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##console_filter", "Search logs...", m_filterBuf.data(), m_filterBuf.size());

    // --- ツールバー 2 段目: 表示オプションと操作 ---
    ImGui::Checkbox("Collapse", &m_collapse);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Merge identical messages into one row with a count");
    ImGui::SameLine();
    ImGui::Checkbox("Clear on Play", &m_clearOnPlay); ImGui::SameLine();
    ImGui::Checkbox("Auto Scroll", &m_autoScroll);    ImGui::SameLine();
    ImGui::Checkbox("Details", &m_showDetail);        ImGui::SameLine();

    const int selectedCount = static_cast<int>(m_selection.size());
    ImGui::BeginDisabled(selectedCount == 0);
    // "###" 以降が ID。件数で表示は変わっても ID は動かさない
    // (ラベルがそのまま ID だと、件数が変わった瞬間に別ボタン扱いになる)。
    char copyLabel[64];
    std::snprintf(copyLabel, sizeof(copyLabel), "Copy Selected (%d)###console_copy_selected",
                  selectedCount);
    const bool copySelectedRequested = ImGui::Button(copyLabel);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Click a line, Shift+click (or drag) for a range, Ctrl+click to add.\n"
                          "Ctrl+A selects every visible line, Ctrl+C copies the selection.");
    ImGui::SameLine();
    const bool copyVisibleRequested = ImGui::Button("Copy Visible");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_sink.Clear();
        m_selection.clear();
        m_anchorSequence = 0;
        m_detailSequence = 0;
    }
    ImGui::Separator();

    // フィルタ条件かログ内容が変わったときだけ、表示行を作り直す。
    // WHY: logChanged は Auto Scroll の追従判定にも使う。行数比較では、リングバッファが
    //      満杯で「古い行が落ちて新しい行が入る」状況を新着として検出できない。
    const bool logChanged = (m_cachedRevision != m_sink.GetRevision());
    if (logChanged || FiltersChanged())
        RebuildRows();

    if (copySelectedRequested)
        CopySelection();
    if (copyVisibleRequested)
        ImGui::SetClipboardText(m_visibleLogText.c_str());

    // --- ログ一覧 ---
    // WHY: ImVec2(0, 0) は「残り全部」を意味するが、詳細ペインを出すぶんの高さを
    //      先に引いておかないと一覧が下端まで伸びて詳細が押し出される。
    const float detailH = m_showDetail ? ImGui::GetContentRegionAvail().y * 0.30f : 0.0f;
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

        // WHY: 512 行でも毎フレーム全行を ImGui へ積むと、フィルタ解除時に
        //      無駄な文字列処理とジオメトリ生成が走る。可視範囲だけ描く。
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const Row& r = m_rows[static_cast<std::size_t>(row)];
                if (r.entryIndex >= entries.size()) continue;
                const core::LogEntry& entry = entries[r.entryIndex];

                ImGui::PushID(row);

                // WHY: メッセージ本文をそのまま Selectable のラベルにすると、"##" を含む
                //      ログ (シェーダー診断など) で以降が表示されなくなる。行の当たり判定は
                //      ID だけの Selectable に任せ、本文は自前で重ね描きする。
                const ImVec2 textPos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", m_selection.contains(r.sequence),
                                      ImGuiSelectableFlags_AllowDoubleClick))
                    ApplyRowClick(row);

                // WHY 押した瞬間に起点を取るか: Selectable が true を返すのは離した時なので、
                //     そこまで待つとドラッグの開始行が分からず、なぞって範囲を作れない。
                const ImGuiIO& io = ImGui::GetIO();
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && !io.KeyShift && !io.KeyCtrl)
                    m_anchorSequence = r.sequence;

                // 押したままなぞって範囲を伸ばす。飛ばした行も起点からの範囲で埋まるため、
                // クリッパーが間引いた行が選択から抜け落ちることはない。
                // WHY AllowWhenBlockedByActiveItem が要るか: ドラッグ中は押した行が
                //     ActiveId を握っており、素の IsItemHovered は他の行で false を返す。
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                    && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    const int anchor = RowIndexOf(m_anchorSequence);
                    if (anchor >= 0) {
                        SelectRange(anchor, row, false);
                        m_detailSequence = r.sequence;
                    }
                }

                // 右クリックは選択外の行なら選び直す。選択内ならまとめて扱いたいので触らない。
                // WHY ポップアップの中で選択を触らないか: 中身は開いている間ずっと毎フレーム
                //     走るため、そこで選び直すと複数選択が毎フレーム 1 行へ潰れる。
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    if (!m_selection.contains(r.sequence)) {
                        m_selection.clear();
                        m_selection.insert(r.sequence);
                        m_anchorSequence = r.sequence;
                    }
                    m_detailSequence = r.sequence;
                }

                const std::string label = FirstLineOf(entry.message);
                ImGui::GetWindowDrawList()->AddText(
                    textPos,
                    ImGui::ColorConvertFloat4ToU32(LogLevelColor(r.level)),
                    label.c_str(), label.c_str() + label.size());

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_detailSequence = r.sequence;
                    JumpToSource(ctx, entry.message);
                }

                // Collapse 件数バッジを行の右端へ重ねる。
                if (r.count > 1) {
                    char badge[16];
                    std::snprintf(badge, sizeof(badge), "%d", r.count);
                    const ImVec2 badgeSize = ImGui::CalcTextSize(badge);
                    const ImVec2 rowMin = ImGui::GetItemRectMin();
                    const ImVec2 rowMax = ImGui::GetItemRectMax();
                    const ImVec2 pos = { rowMax.x - badgeSize.x - 12.0f, rowMin.y };
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled({ pos.x - 4.0f, pos.y },
                                      { pos.x + badgeSize.x + 4.0f, pos.y + badgeSize.y },
                                      IM_COL32(90, 90, 90, 200), 3.0f);
                    dl->AddText(pos, IM_COL32(230, 230, 230, 255), badge);
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
                    std::string f; int ln = 0; std::size_t off = 0;
                    const bool hasLoc = ParseLogLocationPrefix(entry.message, f, ln, off);
                    if (ImGui::MenuItem("Open Source", nullptr, false, hasLoc))
                        JumpToSource(ctx, entry.message);
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }
        }
        clipper.End();
    }

    // Ctrl+C / Ctrl+A は Console にフォーカスがある間だけ拾う。
    // WHY 自前で見るか: HotkeyManager の edit.copy / select.all は Scene View と
    //     Hierarchy のスコープに閉じており、Console では発火しない。取り合いにならない。
    // WHY WantTextInput で降りるか: 検索欄を編集中の Ctrl+C は入力欄のコピーであって
    //     ログのコピーではない。文字入力を待っているフレームは横取りしない。
    // WHY Shortcut() ではなく素の IsKeyPressed か: Shortcut のルーティングは
    //     「今の子ウィンドウが focused か」で決まるため、ツールバーを触った直後の
    //     ように一覧側へフォーカスが入っていないフレームで黙って落ちる。
    //     フォーカス判定はここで明示しているので、キーはそのまま見れば足りる。
    const ImGuiIO& shortcutIo = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !shortcutIo.WantTextInput && shortcutIo.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            CopySelection();
        if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !m_rows.empty())
            SelectRange(0, static_cast<int>(m_rows.size()) - 1, false);
    }

    // Auto Scroll: 最下部に居るときだけ追従する。
    // WHY: 無条件に SetScrollHereY(1.0f) を呼ぶと、ユーザーが過去ログを読もうと
    //      上へスクロールしても毎フレーム下端へ引き戻されてしまう。逆に自分で
    //      下端まで戻せば追従が再開する (Unity / VSCode の出力ペインと同じ挙動)。
    if (m_autoScroll && logChanged && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    // --- 詳細ペイン ---
    if (m_showDetail) {
        ImGui::BeginChild("##log_detail", ImVec2(0, 0), true);
        DrawDetailPane(ctx);
        ImGui::EndChild();
    }
}

} // namespace fbzz::editor
