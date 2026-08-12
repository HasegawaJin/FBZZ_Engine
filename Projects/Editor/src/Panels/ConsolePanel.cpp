// FBZZ Engine
// ConsolePanel.cpp | fbzz::editor
// ログエントリをフィルタ・検索・表示するコンソールパネル
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/SourceOpen.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>

#include <cstdio>
#include <unordered_map>

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

        m_rows.push_back(Row{ i, 1, entry.level });
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

    if (m_selectedRow >= static_cast<int>(m_rows.size()))
        m_selectedRow = -1;
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
    if (m_selectedRow < 0 || m_selectedRow >= static_cast<int>(m_rows.size())) {
        ImGui::TextDisabled("Select a log line to see the full message.");
        return;
    }
    const std::size_t entryIndex = m_rows[static_cast<std::size_t>(m_selectedRow)].entryIndex;
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
        m_selectedRow = -1;
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
    const bool copyVisibleRequested = ImGui::Button("Copy Visible");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_sink.Clear();
        m_selectedRow = -1;
    }
    ImGui::Separator();

    // フィルタ条件かログ内容が変わったときだけ、表示行を作り直す。
    // WHY: logChanged は Auto Scroll の追従判定にも使う。行数比較では、リングバッファが
    //      満杯で「古い行が落ちて新しい行が入る」状況を新着として検出できない。
    const bool logChanged = (m_cachedRevision != m_sink.GetRevision());
    if (logChanged || FiltersChanged())
        RebuildRows();

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
                if (ImGui::Selectable("##row", m_selectedRow == row,
                                      ImGuiSelectableFlags_AllowDoubleClick))
                    m_selectedRow = row;

                const std::string label = FirstLineOf(entry.message);
                ImGui::GetWindowDrawList()->AddText(
                    textPos,
                    ImGui::ColorConvertFloat4ToU32(LogLevelColor(r.level)),
                    label.c_str(), label.c_str() + label.size());

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_selectedRow = row;
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
                    m_selectedRow = row;
                    if (ImGui::MenuItem("Copy"))
                        ImGui::SetClipboardText(entry.message.c_str());
                    if (ImGui::MenuItem("Copy All Visible"))
                        ImGui::SetClipboardText(m_visibleLogText.c_str());
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
