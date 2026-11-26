/// @file    BuildOutputPanel.cpp
/// @brief   ビルド診断・ライブログ・履歴パネルの実装。
/// @author  Hasegawa Jin
/// @date    2026-07-19
#include <Editor/Panels/BuildOutput/BuildOutputPanel.hpp>

#include "ManagedBuildPresentation.hpp"
#include <Editor/Script/Authoring/SourceOpen.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/UI/Appearance/EditorTheme.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

/// @note パスからファイル名部分だけを取り出す (一覧を短く保つため)。
std::string FileNameOnly(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

const char* KindLabel(BuildRecord::Kind kind)
{
    (void)kind;
    return "HLSL";
}

/// @note 履歴コンボに出す 1 行ラベル (例: "14:32:05  Script  FAIL  3E 1W  (2.3s)")。
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
    const std::uint64_t id = rec ? rec->id : 0;
    if (id != m_diagRecord) {
        m_diagRecord = id;
        m_diagCount       = ~static_cast<std::size_t>(0);
        m_diagView.ClearSelection();
    }

    if (rec == nullptr) {
        m_diagView.SetEmptyText("No builds yet.");
        if (!m_diagView.Lines().empty()) {
            m_diagView.SetLines({});
        }
        m_diagCount = 0;
        return;
    }

    switch (rec->result) {
        case BuildRecord::Result::Building: m_diagView.SetEmptyText("No diagnostics yet."); break;
        case BuildRecord::Result::Success:  m_diagView.SetEmptyText("Build succeeded - no diagnostics."); break;
        default:                            m_diagView.SetEmptyText("No diagnostics."); break;
    }

    if (rec->diagnostics.size() == m_diagCount) {
        return;
    }

    std::vector<LogListLine> lines;
    lines.reserve(rec->diagnostics.size());
    for (std::size_t i = 0; i < rec->diagnostics.size(); ++i) {
        lines.push_back(MakeDiagnosticLine(rec->diagnostics[i], i));
    }
    m_diagView.SetLines(std::move(lines));
    m_diagCount = rec->diagnostics.size();
}

void BuildOutputPanel::DrawHeader(EditorContext& ctx, BuildConsole& console, const BuildRecord* rec)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float       sp = st.ItemSpacing.x;
    const auto btnW = [&](const char* s) { return ImGui::CalcTextSize(s).x + st.FramePadding.x * 2.0f; };

    if (console.IsBuilding() && rec == console.Latest()) {
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
    const float rightW  = (history.empty() ? 0.0f : comboW + sp) + btnW("Clear");
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > rightW) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - rightW);
    } else {
        ImGui::NewLine();
    }

    if (!history.empty()) {
        ImGui::SetNextItemWidth(comboW);
        const bool viewingLatest = !rec || m_selectedBuild != rec->id;
        const std::string current = viewingLatest
            ? std::string("Latest: ") + RecordLabel(history.back())
            : RecordLabel(*rec);
        if (ImGui::BeginCombo("##BuildHistory", current.c_str())) {
            if (ImGui::Selectable("Latest (follow new builds)", viewingLatest)) {
                m_selectedBuild = 0;
            }
            ImGui::Separator();
            for (auto entry = history.rbegin(); entry != history.rend(); ++entry) {
                ImGui::PushID(static_cast<int>(entry->id));
                if (ImGui::Selectable(RecordLabel(*entry).c_str(), m_selectedBuild == entry->id)) {
                    m_selectedBuild = entry->id;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, sp);
    }

    if (ImGui::Button("Clear")) {
        ClearHistory(console);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear the build history (a running build is kept)");
    }
}

void BuildOutputPanel::DrawSplitter()
{
    constexpr float kThickness = 4.0f;
    ImGui::InvisibleButton("##build_splitter", ImVec2(ImGui::GetContentRegionAvail().x, kThickness));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
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

void BuildOutputPanel::DrawManagedBuildStatus(EditorContext& ctx)
{
    if (!ctx.managedScripts) {
        return;
    }
    const ManagedBuildRequest* latest = ctx.managedScripts->Builds().Latest();
    if (!latest) {
        return;
    }
    const ManagedBuildRequest& request = *latest;
    double elapsedMilliseconds = request.elapsedMilliseconds;
    double queueMilliseconds = request.queueMilliseconds;
    double compileMilliseconds = request.compileMilliseconds;
    double sourceMilliseconds = request.sourceMilliseconds;
    double validationMilliseconds = request.validationMilliseconds;
    double applyMilliseconds = request.applyMilliseconds;
    if (!ManagedBuildHistory::IsTerminal(request.state)) {
        const auto now = std::chrono::steady_clock::now();
        const double stage = std::chrono::duration<double, std::milli>(now - request.stageStartedAt).count();
        elapsedMilliseconds = std::chrono::duration<double, std::milli>(now - request.queuedAt).count();
        if (request.state == ManagedBuildState::QUEUED) {
            queueMilliseconds += stage;
        }
        if (request.state == ManagedBuildState::COMPILING) {
            compileMilliseconds += stage;
        }
        if (request.state == ManagedBuildState::APPLYING) {
            applyMilliseconds += stage;
        }
        if (request.state == ManagedBuildState::SCANNING) {
            sourceMilliseconds += stage;
        }
        if (request.state == ManagedBuildState::VALIDATING) {
            validationMilliseconds += stage;
        }
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (request.state == ManagedBuildState::FAILED) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    const bool open = ImGui::TreeNodeEx("C# ビルド", flags);
    ImGui::SameLine();
    const ImVec4 color = request.state == ManagedBuildState::FAILED
                       ? EditorTheme::Color(ThemeColor::Danger)
                       : EditorTheme::Color(ThemeColor::TextMuted);
    ImGui::TextColored(color, "%s  %.0f ms", ManagedBuildStateLabel(request.state),
                       elapsedMilliseconds);
    if (!open) {
        return;
    }

    ImGui::TextDisabled("待機 %.0f ms  ソース %.0f ms  コンパイル %.0f ms  検証 %.0f ms  適用 %.0f ms",
                        queueMilliseconds, sourceMilliseconds,
                        compileMilliseconds, validationMilliseconds,
                        applyMilliseconds);
    if (!request.message.empty()) {
        ImGui::TextWrapped("%s", request.message.c_str());
    }
    if (m_managedDiagnosticRequestId != request.managedRequestId) {
        m_managedDiagnosticRequestId = request.managedRequestId;
        m_managedDiagnosticPage = 0;
        m_managedDiagnosticSelection = 0;
    }
    if (m_managedDiagnosticSelection >= request.diagnostics.size())
        m_managedDiagnosticSelection = request.diagnostics.empty() ? 0 : request.diagnostics.size() - 1;
    ImGui::TextDisabled("診断 %zu 件  Editor保持出力 %llu / %llu bytes%s",
        request.diagnostics.size(), static_cast<unsigned long long>(request.logRetainedBytes),
        static_cast<unsigned long long>(request.logOriginalBytes),
        request.logRetainedBytes < request.logOriginalBytes ? " (末尾を保持)" : "");
    if (ImGui::SmallButton("保持したEditor出力をコピー")) {
        ImGui::SetClipboardText(request.log.c_str());
    }
    if (!request.logFilePath.empty()) {
        ImGui::TextWrapped("完全な AgentBuild ログ: %s", request.logFilePath.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("ログパスをコピー")) {
            ImGui::SetClipboardText(request.logFilePath.c_str());
        }
    }
    constexpr std::size_t diagnosticPageSize = 128;
    const std::size_t pageCount = std::max<std::size_t>(1,
        (request.diagnostics.size() + diagnosticPageSize - 1) / diagnosticPageSize);
    m_managedDiagnosticPage = std::min(m_managedDiagnosticPage, pageCount - 1);
    ImGui::BeginDisabled(m_managedDiagnosticPage == 0);
    if (ImGui::SmallButton("前の診断")) {
        --m_managedDiagnosticPage;
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(m_managedDiagnosticPage + 1 >= pageCount);
    if (ImGui::SmallButton("次の診断")) {
        ++m_managedDiagnosticPage;
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    const std::size_t firstDiagnostic = m_managedDiagnosticPage * diagnosticPageSize;
    const std::size_t lastDiagnostic = std::min(request.diagnostics.size(), firstDiagnostic + diagnosticPageSize);
    ImGui::Text("%zu / %zu", m_managedDiagnosticPage + 1, pageCount);
    const std::vector<std::string> roots = {
        ctx.scriptsSourceDir,
        ctx.projectRoot,
        ctx.engineRoot
    };
    ImGui::BeginChild("##managed_diagnostics", ImVec2(0.0f, 220.0f), true);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lastDiagnostic - firstDiagnostic));
    while (clipper.Step()) for (int local = clipper.DisplayStart; local < clipper.DisplayEnd; ++local) {
        const std::size_t i = firstDiagnostic + static_cast<std::size_t>(local);
        const ManagedBuildDiagnostic& diagnostic = request.diagnostics[i];
        std::string resolved = ResolveSourceFilePath(diagnostic.file, roots);
        if (resolved.empty()) {
            resolved = ResolveSourceFileByName(diagnostic.file, roots);
        }
        const std::size_t separator = diagnostic.file.find_last_of("/\\");
        const std::string fileLabel = diagnostic.file.empty()
                                    ? "(source unavailable)"
                                    : diagnostic.file.substr(separator == std::string::npos ? 0 : separator + 1);
        char location[512];
        if (diagnostic.line > 0 && diagnostic.column > 0)
            std::snprintf(location, sizeof(location), "%s:%d:%d", fileLabel.c_str(),
                          diagnostic.line, diagnostic.column);
        else if (diagnostic.line > 0) {
            std::snprintf(location, sizeof(location), "%s:%d", fileLabel.c_str(), diagnostic.line);
        } else {
            std::snprintf(location, sizeof(location), "%s", fileLabel.c_str());
        }
        ImGui::PushID(&diagnostic);
        char rowLabel[640];
        std::snprintf(rowLabel, sizeof(rowLabel), "%zu  %s  %s: %s", i + 1,
            diagnostic.warning ? "WARN" : "ERROR", diagnostic.code.c_str(), diagnostic.message.c_str());
        if (ImGui::Selectable(rowLabel, m_managedDiagnosticSelection == i,
                0, ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing()))) {
            m_managedDiagnosticSelection = i;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(resolved.empty() || diagnostic.line <= 0);
        if (ImGui::SmallButton(location)) {
            OpenSourceInExternalEditor(resolved, diagnostic.line);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s\n%s", diagnostic.file.c_str(), diagnostic.message.c_str());
        }
        ImGui::PopID();
    }
    clipper.End();
    ImGui::EndChild();
    if (!request.diagnostics.empty()) {
        const ManagedBuildDiagnostic& diagnostic = request.diagnostics[m_managedDiagnosticSelection];
        ImGui::TextColored(EditorTheme::Color(diagnostic.warning ? ThemeColor::Warning : ThemeColor::Danger),
            "%s %s", diagnostic.warning ? "警告" : "エラー", diagnostic.code.c_str());
        ImGui::TextWrapped("%s", diagnostic.file.c_str());
        ImGui::Text("%d:%d", diagnostic.line, diagnostic.column);
        ImGui::TextWrapped("%s", diagnostic.message.c_str());
        if (ImGui::SmallButton("選択した診断をコピー")) {
            const std::string copied = diagnostic.file + ":" + std::to_string(diagnostic.line) + ":" +
                std::to_string(diagnostic.column) + " " + diagnostic.code + " " + diagnostic.message;
            ImGui::SetClipboardText(copied.c_str());
        }
    }
    ImGui::TreePop();
}

void BuildOutputPanel::OnRenderContent(EditorContext& ctx)
{
    DrawManagedBuildStatus(ctx);
    BuildConsole* console = ctx.buildConsole;
    if (!console) {
        if (!ctx.managedScripts) {
            ImGui::TextDisabled("Build console is not available.");
        }
        return;
    }

    DrawHeader(ctx, *console, SelectedRecord(*console));
    const BuildRecord* rec = SelectedRecord(*console);
    ImGui::Separator();

    SyncDiagnostics(rec);
    m_diagView.SetInfoToggleVisible(false);
    m_rawView.SetEmptyText("No output yet.");
    if (rec && rec->result == BuildRecord::Result::Building) {
        m_rawFeed.Sync(console->LiveLog(), rec->id, console->LiveLogFirstLine(), m_rawView);
    } else if (rec) {
        m_rawFeed.Sync(rec->rawLog, rec->id, rec->firstRawLine, m_rawView);
    } else {
        m_rawFeed.Sync({}, 0, 0, m_rawView);
    }

    if (m_focusFirstError && rec) {
        for (std::size_t i = 0; i < rec->diagnostics.size(); ++i) {
            if (rec->diagnostics[i].severity == BuildDiagnostic::Severity::Error) {
                m_diagView.Reveal(i);
                break;
            }
        }
        m_focusFirstError = false;
    }

    // 高さは先にツールバー 2 つとスプリッタの分を引き、残りを比率で分ける。
    // 診断を固定 55% で確保すると、パネルを縮めたとき生ログのツールバーごと隠れていた。
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
    ImGui::TextDisabled("Output");
    ImGui::SameLine();
    m_rawView.DrawToolbar();
    m_rawView.DrawList({ 0.0f, 0.0f });
}

} // namespace fbzz::editor
