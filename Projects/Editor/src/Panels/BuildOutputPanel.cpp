// FBZZ Engine
// BuildOutputPanel.cpp | fbzz::editor
#include <Editor/Panels/BuildOutputPanel.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/SourceOpen.hpp>

#include <Engine/Util/StringUtils.hpp>

#include <imgui.h>

#include <cstdio>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

ImVec4 SeverityColor(BuildDiagnostic::Severity sev)
{
    return (sev == BuildDiagnostic::Severity::Error)
        ? ImVec4(1.00f, 0.35f, 0.30f, 1.0f)   // 赤
        : ImVec4(1.00f, 0.80f, 0.20f, 1.0f);  // 黄
}

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

// 履歴コンボに出す 1 行ラベル (例: "14:32:05  Script  ✖ 3E 1W  (2.3s)")。
std::string RecordLabel(const BuildRecord& rec)
{
    const char* mark = "…";
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

} // namespace

void BuildOutputPanel::OnRenderContent(EditorContext& ctx)
{
    BuildConsole* console = ctx.buildConsole;
    if (!console) {
        ImGui::TextDisabled("Build console is not available.");
        return;
    }

    const auto& history = console->History();

    // 表示対象レコードを決める。-1 (最新) は履歴末尾、または履歴が空なら nullptr。
    const BuildRecord* rec = nullptr;
    if (m_viewHistoryIndex >= 0 && m_viewHistoryIndex < static_cast<int>(history.size()))
        rec = &history[static_cast<size_t>(m_viewHistoryIndex)];
    else
        rec = console->Latest();

    // ---- ヘッダ: 状態バッジ + 操作ボタン ----
    if (console->IsBuilding()) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.15f, 1.0f), "%s", "Building...");
        if (!console->CurrentFile().empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("compiling %s", console->CurrentFile().c_str());
        }
    } else if (rec) {
        const int e = rec->errorCount, w = rec->warnCount;
        if (e > 0) ImGui::TextColored(SeverityColor(BuildDiagnostic::Severity::Error),   "%d errors", e);
        else       ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "%s", "0 errors");
        ImGui::SameLine();
        ImGui::TextColored(SeverityColor(BuildDiagnostic::Severity::Warning), "%d warnings", w);
    } else {
        ImGui::TextDisabled("No builds yet.");
    }

    ImGui::SameLine();
    // 右寄せでボタン群を配置する。
    const float btnGroupW = 220.0f;
    const float rightX = ImGui::GetWindowWidth() - btnGroupW;
    if (rightX > ImGui::GetCursorPosX()) ImGui::SameLine(rightX);

    // Rebuild は script.reload operator を通す。
    // WHY 直接 requestScriptReload を立てないか: その operator は「コンパイル中は
    //     開始できない」条件 (poll) を持っており、フラグを直に立てるとその条件を
    //     素通りする。console->IsBuilding() だけを見ていると、HLSL リロードや
    //     ホットリロード監視が走らせたビルドの最中でも押せてしまう。
    const bool canRebuild = CanInvokeOperator(ctx, "script.reload") && !console->IsBuilding();
    if (!canRebuild) ImGui::BeginDisabled();
    if (ImGui::Button("Rebuild")) InvokeOperator(ctx, "script.reload");
    if (!canRebuild) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Errors only", &m_errorsOnly);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        console->ClearHistory();
        m_viewHistoryIndex = -1;
    }

    // ---- 履歴セレクタ ----
    if (!history.empty()) {
        ImGui::SetNextItemWidth(-1.0f);
        const std::string current = (m_viewHistoryIndex < 0 || m_viewHistoryIndex >= static_cast<int>(history.size()))
            ? std::string("Latest — ") + RecordLabel(history.back())
            : RecordLabel(history[static_cast<size_t>(m_viewHistoryIndex)]);
        if (ImGui::BeginCombo("##BuildHistory", current.c_str())) {
            // 最新 (ライブ追従) を先頭に。
            if (ImGui::Selectable("Latest", m_viewHistoryIndex < 0))
                m_viewHistoryIndex = -1;
            // 新しい順に列挙する。
            for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(RecordLabel(history[static_cast<size_t>(i)]).c_str(), m_viewHistoryIndex == i))
                    m_viewHistoryIndex = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // ---- 上段: 診断リスト / 下段: 生ログ ----
    const float avail = ImGui::GetContentRegionAvail().y;
    const float diagH = avail * 0.55f;

    ImGui::BeginChild("##Diagnostics", ImVec2(0, diagH), true);
    if (rec) DrawDiagnostics(*rec);
    else     ImGui::TextDisabled("No diagnostics.");
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextDisabled("Raw output");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##rawlog")) {
        // ライブ or 選択レコードの生ログをコピー。レコード側は生ログを保持しないため
        // ライブログ (最新ビルド) のみコピー対象にする。
        ImGui::SetClipboardText(console->LiveLog().c_str());
    }
    DrawRawLog(*console, rec);
}

void BuildOutputPanel::DrawDiagnostics(const BuildRecord& rec)
{
    if (rec.diagnostics.empty()) {
        ImGui::TextDisabled("%s", rec.result == BuildRecord::Result::Success
                                      ? "Build succeeded — no diagnostics."
                                      : "No diagnostics.");
        return;
    }

    int errorRowIndex = -1;  // 最初のエラー行の描画インデックス (フォーカス用)
    int row = 0;
    for (const auto& d : rec.diagnostics) {
        if (m_errorsOnly && d.severity != BuildDiagnostic::Severity::Error) continue;
        if (errorRowIndex < 0 && d.severity == BuildDiagnostic::Severity::Error) errorRowIndex = row;

        ImGui::PushID(row);
        ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(d.severity));

        // 選択可能な 1 行。ダブルクリックでファイルを開く。
        char label[512];
        if (d.line > 0)
            std::snprintf(label, sizeof(label), "%s(%d)  %s  %s",
                          FileNameOnly(d.file).c_str(), d.line, d.code.c_str(), d.message.c_str());
        else
            std::snprintf(label, sizeof(label), "%s  %s  %s",
                          FileNameOnly(d.file).c_str(), d.code.c_str(), d.message.c_str());

        ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick);
        ImGui::PopStyleColor();

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", d.raw.c_str());
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                OpenSourceInExternalEditor(d.file, d.line);
        }

        // 通知経由で開かれた場合、最初のエラー行へスクロールする。
        if (m_focusFirstError && row == errorRowIndex) {
            ImGui::SetScrollHereY(0.5f);
            m_focusFirstError = false;
        }

        ImGui::PopID();
        ++row;
    }
}

void BuildOutputPanel::DrawRawLog(const BuildConsole& console, const BuildRecord* /*rec*/)
{
    ImGui::BeginChild("##RawLog", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    // WHY: レコードは生ログ全文を保持しないため (メモリ節約)、生ログは常に最新ビルドの
    //      LiveLog を表示する。過去ビルドの詳細は診断リスト側で参照する運用。
    const std::string& log = console.LiveLog();
    ImGui::TextUnformatted(log.c_str(), log.c_str() + log.size());
    if (m_autoScroll && console.IsBuilding())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

} // namespace fbzz::editor
