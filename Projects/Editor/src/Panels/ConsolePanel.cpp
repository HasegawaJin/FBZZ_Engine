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
    ImGui::Checkbox("INFO",  &m_showInfo);  ImGui::SameLine();
    ImGui::Checkbox("WARN",  &m_showWarn);  ImGui::SameLine();
    ImGui::Checkbox("ERROR", &m_showError); ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Filter", m_filterBuf.data(), m_filterBuf.size()); ImGui::SameLine();
    if (ImGui::Button("Clear")) m_sink.Clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto Scroll", &m_autoScroll);
    ImGui::Separator();

    // エントリ一覧
    ImGui::BeginChild("##log", {0, 0}, false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& entry : m_sink.GetEntries()) {
        if (entry.level == core::LogLevel::INFO    && !m_showInfo)  continue;
        if (entry.level == core::LogLevel::WARNING && !m_showWarn)  continue;
        if (entry.level == core::LogLevel::LOG_ERROR && !m_showError) continue;

        std::string filter(m_filterBuf.data());
        if (!filter.empty() && entry.message.find(filter) == std::string::npos) continue;

        ImVec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (entry.level == core::LogLevel::WARNING)   color = { 1.0f, 0.8f, 0.2f, 1.0f };
        if (entry.level == core::LogLevel::LOG_ERROR) color = { 1.0f, 0.3f, 0.3f, 1.0f };

        ImGui::TextColored(color, "%s", entry.message.c_str());
    }

    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

} // namespace fbzz::editor
