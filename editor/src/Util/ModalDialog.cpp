// FBZZ Engine
// ModalDialog.cpp | fbzz::editor
// ImGui モーダル確認ダイアログ実装
#include <editor/Util/ModalDialog.hpp>
#include <imgui.h>

namespace fbzz::editor {

ModalDialog::State ModalDialog::s_state;

void ModalDialog::OpenConfirm(const std::string& title,
                               const std::string& message,
                               std::function<void()> onConfirm)
{
    // ImGui::OpenPopup は OnRender 内 (Begin/End ブロック内) で呼ぶ必要があるため、
    // ここではフラグだけ立てる
    s_state = { title, message, std::move(onConfirm), true, false };
}

void ModalDialog::OnRender()
{
    if (!s_state.pending) return;

    // 最初のフレームだけ OpenPopup を発行する (BeginPopupModal と同じウィンドウ内で呼ぶこと)
    if (!s_state.opened) {
        ImGui::OpenPopup(s_state.title.c_str());
        s_state.opened = true;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, { 0.5f, 0.5f });

    if (ImGui::BeginPopupModal(s_state.title.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(s_state.message.c_str());
        ImGui::Spacing();

        if (ImGui::Button("OK", { 120, 0 })) {
            if (s_state.onConfirm) s_state.onConfirm();
            s_state.pending = false;
            s_state.opened  = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", { 120, 0 })) {
            s_state.pending = false;
            s_state.opened  = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        // ESC 等で外部から閉じられた
        s_state.pending = false;
        s_state.opened  = false;
    }
}

} // namespace fbzz::editor
