// FBZZ Engine
// ConsolePanel.cpp | fbzz::editor
// ログエントリをフィルタ・検索・表示するコンソールパネル
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <imgui.h>

namespace fbzz::editor {

namespace {

// ログレベルごとの表示色を返す。
// WHY: Console では重大度を視線だけで判別できることが重要なため、本文全体をレベル色で描画する。
ImVec4 LogLevelColor(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::DEBUG:     return { 0.72f, 0.45f, 1.00f, 1.0f }; // 紫
    case core::LogLevel::INFO:      return { 0.92f, 0.92f, 0.92f, 1.0f }; // 白
    case core::LogLevel::WARNING:   return { 1.00f, 0.82f, 0.20f, 1.0f }; // 黄色
    case core::LogLevel::LOG_ERROR: return { 1.00f, 0.30f, 0.25f, 1.0f }; // 赤
    default:                        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
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

void ConsolePanel::OnRenderContent(EditorContext& /*ctx*/)
{
    // ツールバー
    ImGui::Checkbox("DEBUG", &m_showDebug); ImGui::SameLine();
    if (m_showDebug)
        core::Logger::SetMinLevel(core::LogLevel::DEBUG);
    else
        core::Logger::SetMinLevel(core::LogLevel::INFO);

    ImGui::Checkbox("INFO",  &m_showInfo);  ImGui::SameLine();
    ImGui::Checkbox("WARN",  &m_showWarn);  ImGui::SameLine();
    ImGui::Checkbox("ERROR", &m_showError); ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Filter", m_filterBuf.data(), m_filterBuf.size()); ImGui::SameLine();
    const bool copyVisibleRequested = ImGui::Button("Copy Visible");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) m_sink.Clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto Scroll", &m_autoScroll);
    ImGui::Separator();

    m_visibleLogText.clear();
    const std::string filter(m_filterBuf.data());
    auto isVisible = [&](const core::LogEntry& entry) {
        if (entry.level == core::LogLevel::DEBUG     && !m_showDebug) return false;
        if (entry.level == core::LogLevel::INFO      && !m_showInfo)  return false;
        if (entry.level == core::LogLevel::WARNING   && !m_showWarn)  return false;
        if (entry.level == core::LogLevel::LOG_ERROR && !m_showError) return false;
        if (!filter.empty() && entry.message.find(filter) == std::string::npos) return false;
        return true;
    };

    for (const auto& entry : m_sink.GetEntries()) {
        if (!isVisible(entry)) continue;
        m_visibleLogText += entry.message;
        m_visibleLogText += '\n';
    }

    if (copyVisibleRequested)
        ImGui::SetClipboardText(m_visibleLogText.c_str());

    // WHY: ImVec2(0, 0) は InputTextMultiline の既定サイズになり、Console ウィンドウを広げても
    //      実際のログ表示欄が小さいまま残る。
    // WHAT: ツールバー描画後の残り領域をそのまま使い、Dock / リサイズ時も表示欄を追従させる。
    ImVec2 logRegion = ImGui::GetContentRegionAvail();
    if (logRegion.x < 1.0f) logRegion.x = 1.0f;
    if (logRegion.y < 1.0f) logRegion.y = 1.0f;

    ImGui::BeginChild("##log", logRegion, true, ImGuiWindowFlags_HorizontalScrollbar);
    int shownCount = 0;
    for (const auto& entry : m_sink.GetEntries()) {
        if (!isVisible(entry)) continue;

        ImGui::PushStyleColor(ImGuiCol_Text, LogLevelColor(entry.level));
        ImGui::TextUnformatted(entry.message.c_str());
        ImGui::PopStyleColor();
        ++shownCount;
    }
    // 空状態ガイド: ログが無いのか、フィルタで隠れているのかを区別して案内する。
    if (shownCount == 0) {
        const bool hasAny = !m_sink.GetEntries().empty();
        ImGui::TextDisabled("%s", hasAny
            ? "No logs match the current filters (level toggles / filter text)."
            : "No logs yet. Engine and script messages will appear here.");
    }

    if (m_autoScroll && !m_visibleLogText.empty())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

} // namespace fbzz::editor
