// FBZZ Engine
// StatusBar.cpp | fbzz::editor
// ウィンドウ最下部に固定表示される情報バー
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>

namespace fbzz::editor {

void StatusBar::OnRender(EditorContext& ctx)
{
    // FPS 更新 (1秒平均)
    m_fpsTimer += ImGui::GetIO().DeltaTime;
    ++m_fpsCount;
    if (m_fpsTimer >= 1.0f) {
        m_fps      = static_cast<float>(m_fpsCount) / m_fpsTimer;
        m_fpsTimer = 0.0f;
        m_fpsCount = 0;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float barHeight   = ImGui::GetFrameHeight() + 4.0f;
    ImGui::SetNextWindowPos({ vp->Pos.x, vp->Pos.y + vp->Size.y - barHeight });
    ImGui::SetNextWindowSize({ vp->Size.x, barHeight });
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                           | ImGuiWindowFlags_NoNav
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoScrollWithMouse
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoDocking;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 4.0f, 2.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##statusbar", nullptr, flags);
    ImGui::PopStyleVar(2);

    ImGui::Text("FPS: %.1f", m_fps);
    ImGui::SameLine(200.0f);

    // 選択 Entity 名
    auto sel = ctx.PrimarySelected();
    const char* selName = "None";
    if (sel.IsValid() && ctx.activeScene) {
        if (auto* go = ctx.activeScene->GetGameObject(sel))
            selName = go->name.c_str();
    }
    ImGui::Text("Selected: %s", selName);
    ImGui::SameLine(400.0f);

    // ステータスメッセージ
    if (!m_message.empty())
        ImGui::TextUnformatted(m_message.c_str());

    ImGui::End();
}

} // namespace fbzz::editor
