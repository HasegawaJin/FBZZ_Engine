// FBZZ Engine
// ConsolePanel.cpp | fbzz::editor
// ログエントリをフィルタ・検索・表示するコンソールパネル
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <imgui.h>

namespace fbzz::editor {

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

    // WHAT: ImGui::TextColored は表示専用で、ユーザーがログ本文を範囲選択できない。
    //       read-only の InputTextMultiline に集約することで、Ctrl+C / 右クリックコピー / 範囲選択を
    //       OS のテキスト操作に近い感覚で扱えるようにする。
    m_visibleLogText.clear();
    const std::string filter(m_filterBuf.data());
    for (const auto& entry : m_sink.GetEntries()) {
        if (entry.level == core::LogLevel::DEBUG    && !m_showDebug) continue;
        if (entry.level == core::LogLevel::INFO     && !m_showInfo)  continue;
        if (entry.level == core::LogLevel::WARNING  && !m_showWarn)  continue;
        if (entry.level == core::LogLevel::LOG_ERROR && !m_showError) continue;

        if (!filter.empty() && entry.message.find(filter) == std::string::npos) continue;

        m_visibleLogText += entry.message;
        m_visibleLogText += '\n';
    }

    if (copyVisibleRequested)
        ImGui::SetClipboardText(m_visibleLogText.c_str());

    ImGui::InputTextMultiline(
        "##log",
        m_visibleLogText.data(),
        m_visibleLogText.size() + 1,
        ImVec2(0.0f, 0.0f),
        ImGuiInputTextFlags_ReadOnly);

    if (m_autoScroll && !m_visibleLogText.empty() && !ImGui::IsItemActive())
        ImGui::SetScrollHereY(1.0f);
}

} // namespace fbzz::editor
