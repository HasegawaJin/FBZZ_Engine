/// @file    BuildOutputPanel.cpp
/// @brief   ビルド診断・ライブログ・履歴パネルの実装。
/// @author  Hasegawa Jin
/// @date    2026-07-19
#include <Editor/Panels/BuildOutputPanel.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/EditorTheme.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

// パスからファイル名部分だけを取り出す (一覧を短く保つため)。
std::string FileNameOnly(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

const char* KindLabel(BuildRecord::Kind kind)
{
    return kind == BuildRecord::Kind::Script ? "Script" : "HLSL";
}

// 履歴コンボに出す 1 行ラベル (例: "14:32:05  Script  FAIL  3E 1W  (2.3s)")。
std::string RecordLabel(const BuildRecord& rec)
{
    const char* mark = "...";
    switch (rec.result) {
        case BuildRecord::Result::Success:   mark = "OK";     break;
        case BuildRecord::Result::Failed:    mark = "FAIL";   break;
        case BuildRecord::Result::Cancelled: mark = "CANCEL"; break;
        case BuildRecord::Result::Building:  mark = "...";    break;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s  %-6s  %s  %dE %dW  (%.1fs)",
                  rec.startClock.c_str(), KindLabel(rec.kind), mark,
                  rec.errorCount, rec.warnCount, rec.durationSec);
    return buf;
}

LogListLine MakeDiagnosticLine(const BuildDiagnostic& d, std::uint64_t id)
{
    LogListLine line;
    line.id       = id;
    line.severity = (d.severity == BuildDiagnostic::Severity::Error) ? LogListSeverity::Error
                                                                     : LogListSeverity::Warning;
    line.location = FileNameOnly(d.file);
    if (d.line > 0) {
        line.location += ':';
        line.location += std::to_string(d.line);
    }
    line.tag      = d.code;
    line.text     = d.message;
    // コピーは元の 1 行 (フルパス込み) にする。貼った先でそのまま場所が分かるように。
    line.copyText = d.raw;
    line.file     = d.file;
    line.line     = d.line;
    return line;
}

} // namespace

void BuildOutputPanel::SyncDiagnostics(const BuildRecord* rec)
{
    // 別レコードへ切り替えたら選択は持ち越さない (id は診断の添字なので別物を指してしまう)。
    const std::string clock = rec ? rec->startClock : std::string();
    if (rec != m_diagRecord || clock != m_diagRecordClock) {
        m_diagRecord      = rec;
        m_diagRecordClock = clock;
        m_diagCount       = ~static_cast<std::size_t>(0);
        m_diagView.ClearSelection();
    }

    if (rec == nullptr) {
        m_diagView.SetEmptyText("No builds yet.");
        if (!m_diagView.Lines().empty()) m_diagView.SetLines({});
        m_diagCount = 0;
        return;
    }

    switch (rec->result) {
        case BuildRecord::Result::Building: m_diagView.SetEmptyText("No diagnostics yet."); break;
        case BuildRecord::Result::Success:  m_diagView.SetEmptyText("Build succeeded - no diagnostics."); break;
        default:                            m_diagView.SetEmptyText("No diagnostics."); break;
    }

    if (rec->diagnostics.size() == m_diagCount) return;

    std::vector<LogListLine> lines;
    lines.reserve(rec->diagnostics.size());
    for (std::size_t i = 0; i < rec->diagnostics.size(); ++i)
        lines.push_back(MakeDiagnosticLine(rec->diagnostics[i], i));
    m_diagView.SetLines(std::move(lines));
    m_diagCount = rec->diagnostics.size();
}

void BuildOutputPanel::DrawHeader(EditorContext& ctx, BuildConsole& console, const BuildRecord* rec)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float       sp = st.ItemSpacing.x;
    const auto btnW = [&](const char* s) { return ImGui::CalcTextSize(s).x + st.FramePadding.x * 2.0f; };

    if (console.IsBuilding()) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "Building...");
        if (!console.CurrentFile().empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("compiling %s", console.CurrentFile().c_str());
        }
    } else if (rec) {
        const int e = rec->errorCount;
        const int w = rec->warnCount;
        ImGui::TextColored(EditorTheme::Color(e > 0 ? ThemeColor::Danger : ThemeColor::Success),
                           "%d error%s", e, e == 1 ? "" : "s");
        ImGui::SameLine();
        ImGui::TextColored(EditorTheme::Color(w > 0 ? ThemeColor::Warning : ThemeColor::TextMuted),
                           "%d warning%s", w, w == 1 ? "" : "s");
        ImGui::SameLine();
        ImGui::TextDisabled("%s  %.1fs", KindLabel(rec->kind), rec->durationSec);
    } else {
        ImGui::TextDisabled("No builds yet.");
    }

    // 右側: 履歴 / Rebuild / Clear。幅が足りなければ次の行へ回す。
    const auto& history = console.History();
    const float comboW  = (std::min)(320.0f, (std::max)(160.0f, ImGui::GetContentRegionAvail().x * 0.4f));
    const float rightW  = (history.empty() ? 0.0f : comboW + sp) + btnW("Rebuild") + sp + btnW("Clear");
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > rightW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - rightW);
    else                ImGui::NewLine();

    if (!history.empty()) {
        ImGui::SetNextItemWidth(comboW);
        const bool viewingLatest = m_viewHistoryIndex < 0 || m_viewHistoryIndex >= static_cast<int>(history.size());
        const std::string current = viewingLatest
            ? std::string("Latest: ") + RecordLabel(history.back())
            : RecordLabel(history[static_cast<size_t>(m_viewHistoryIndex)]);
        if (ImGui::BeginCombo("##BuildHistory", current.c_str())) {
            if (ImGui::Selectable("Latest (follow new builds)", viewingLatest))
                m_viewHistoryIndex = -1;
            ImGui::Separator();
            for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(RecordLabel(history[static_cast<size_t>(i)]).c_str(), m_viewHistoryIndex == i))
                    m_viewHistoryIndex = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, sp);
    }

    // Rebuild は script.reload operator を通す。
    // WHY 直接 requestScriptReload を立てないか: その operator は「コンパイル中は
    //     開始できない」条件 (poll) を持っており、フラグを直に立てるとその条件を
    //     素通りする。console.IsBuilding() だけを見ていると、HLSL リロードや
    //     ホットリロード監視が走らせたビルドの最中でも押せてしまう。
    const bool canRebuild = CanInvokeOperator(ctx, "script.reload") && !console.IsBuilding();
    ImGui::BeginDisabled(!canRebuild);
    if (ImGui::Button("Rebuild")) InvokeOperator(ctx, "script.reload");
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, sp);
    if (ImGui::Button("Clear")) {
        console.ClearHistory();
        m_viewHistoryIndex = -1;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Clear the build history (a running build is kept)");
}

void BuildOutputPanel::DrawSplitter()
{
    constexpr float kThickness = 4.0f;
    ImGui::InvisibleButton("##build_splitter", ImVec2(ImGui::GetContentRegionAvail().x, kThickness));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) {
        const float panelH = (std::max)(ImGui::GetWindowHeight(), 1.0f);
        m_splitRatio = std::clamp(m_splitRatio + ImGui::GetIO().MouseDelta.y / panelH, 0.10f, 0.85f);
    }
    const ImU32 color = (ImGui::IsItemActive() || ImGui::IsItemHovered())
                      ? EditorTheme::ColorU32(ThemeColor::Accent)
                      : EditorTheme::ColorU32(ThemeColor::Border);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled({ min.x, min.y + 1.0f }, { max.x, max.y - 1.0f }, color);
}

void BuildOutputPanel::OnRenderContent(EditorContext& ctx)
{
    BuildConsole* console = ctx.buildConsole;
    if (!console) {
        ImGui::TextDisabled("Build console is not available.");
        return;
    }

    const auto& history = console->History();
    const BuildRecord* rec = nullptr;
    if (m_viewHistoryIndex >= 0 && m_viewHistoryIndex < static_cast<int>(history.size()))
        rec = &history[static_cast<size_t>(m_viewHistoryIndex)];
    else
        rec = console->Latest();

    DrawHeader(ctx, *console, rec);
    // Clear で履歴が消えた直後は rec が宙に浮くので引き直す。
    if (m_viewHistoryIndex < 0 || m_viewHistoryIndex >= static_cast<int>(history.size()))
        rec = console->Latest();
    ImGui::Separator();

    SyncDiagnostics(rec);
    m_diagView.SetInfoToggleVisible(false);
    m_rawView.SetEmptyText("No output yet.");
    m_rawFeed.Sync(console->LiveLog(), console->LiveLogGeneration(), console->LiveLogFirstLine(), m_rawView);

    if (m_focusFirstError && rec) {
        for (std::size_t i = 0; i < rec->diagnostics.size(); ++i) {
            if (rec->diagnostics[i].severity == BuildDiagnostic::Severity::Error) {
                m_diagView.Reveal(i);
                break;
            }
        }
        m_focusFirstError = false;
    }

    // WHY 高さを先に割り振るか: 以前は診断を 55% の固定高で置き、生ログは残り全部を
    //     取っていたため、パネルを縮めると生ログのツールバーごと見えなくなっていた。
    //     2 つのツールバーとスプリッタの高さを先に引き、残りを比率で分ける。
    const ImGuiStyle& st        = ImGui::GetStyle();
    const float       toolbarH  = ImGui::GetFrameHeightWithSpacing();
    const float       splitterH = 4.0f + st.ItemSpacing.y * 2.0f;
    const float       listsH    = (std::max)(0.0f, ImGui::GetContentRegionAvail().y - toolbarH * 2.0f - splitterH);
    const float       diagH     = (std::max)(ImGui::GetFrameHeight() * 2.0f, listsH * m_splitRatio);

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Diagnostics");
    ImGui::SameLine();
    m_diagView.DrawToolbar();
    m_diagView.DrawList({ 0.0f, diagH });

    DrawSplitter();

    ImGui::AlignTextToFramePadding();
    // WHY 過去レコードでも最新の出力を出すか: レコードは生ログ全文を持たない (メモリ節約)。
    //     何を見ているのか取り違えないよう、ラベルで明示する。
    const bool viewingPast = rec != nullptr && rec != console->Latest();
    ImGui::TextDisabled("%s", viewingPast ? "Output (latest build)" : "Output");
    ImGui::SameLine();
    m_rawView.DrawToolbar();
    m_rawView.DrawList({ 0.0f, 0.0f });
}

} // namespace fbzz::editor
