// FBZZ Engine
// ModalDialog.cpp | fbzz::editor
// ImGui modal confirmation dialogs
#include <Editor/Util/ModalDialog.hpp>
#include <imgui.h>
#include <utility>

namespace fbzz::editor {

ModalDialog::State ModalDialog::s_state;

void ModalDialog::OpenConfirm(const std::string& title,
                              const std::string& message,
                              std::function<void()> onConfirm)
{
    s_state = {};
    s_state.title = title;
    s_state.message = message;
    s_state.onConfirm = std::move(onConfirm);
    s_state.pending = true;
}

void ModalDialog::OpenUnsavedChanges(const std::string& title,
                                     const std::string& message,
                                     std::function<bool()> onSave,
                                     std::function<void()> onDiscard)
{
    s_state = {};
    s_state.title = title;
    s_state.message = message;
    s_state.onSave = std::move(onSave);
    s_state.onDiscard = std::move(onDiscard);
    s_state.pending = true;
}

void ModalDialog::OnRender()
{
    if (!s_state.pending) return;

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

        if (s_state.onSave || s_state.onDiscard) {
            if (ImGui::Button("Save", { 120, 0 })) {
                const bool saved = s_state.onSave ? s_state.onSave() : true;
                if (saved) {
                    s_state.pending = false;
                    s_state.opened = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard", { 120, 0 })) {
                if (s_state.onDiscard) s_state.onDiscard();
                s_state.pending = false;
                s_state.opened = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", { 120, 0 })) {
                s_state.pending = false;
                s_state.opened = false;
                ImGui::CloseCurrentPopup();
            }
        } else {
            if (ImGui::Button("OK", { 120, 0 })) {
                if (s_state.onConfirm) s_state.onConfirm();
                s_state.pending = false;
                s_state.opened = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", { 120, 0 })) {
                s_state.pending = false;
                s_state.opened = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    } else {
        s_state.pending = false;
        s_state.opened = false;
    }
}

} // namespace fbzz::editor
