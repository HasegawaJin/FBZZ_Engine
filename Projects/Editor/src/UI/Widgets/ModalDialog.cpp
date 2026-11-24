/// @file    ModalDialog.cpp
/// @brief   ImGui modal confirmation dialogs.
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Editor/UI/Widgets/ModalDialog.hpp>
#include <Editor/UI/Localization.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstring>
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

void ModalDialog::OpenInput(const std::string& title,
                            const std::string& hint,
                            std::function<void(const std::string&)> onConfirm,
                            const std::string& note)
{
    s_state = {};
    s_state.title   = title;
    s_state.note    = note;
    s_state.onInput = std::move(onConfirm);
    s_state.isInput        = true;
    s_state.inputNeedsFocus = true;
    s_state.pending        = true;
    std::strncpy(s_state.inputBuf, hint.c_str(), sizeof(s_state.inputBuf) - 1);
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
    const auto available = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSizeConstraints({ 0.0f, 0.0f },
        { available.x * 0.9f, available.y * 0.9f });

    if (ImGui::BeginPopupModal(s_state.title.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        const auto& style = ImGui::GetStyle();
        const float maxContentWidth = std::max(1.0f, available.x * 0.9f - style.WindowPadding.x * 2.0f);
        const auto buttonWidth = [&](int count) {
            return std::clamp((maxContentWidth - style.ItemSpacing.x * (count - 1)) / count, 1.0f, 120.0f);
        };
        /// @note テキスト入力ダイアログ
        if (s_state.isInput) {
            ImGui::SetNextItemWidth(std::min(320.0f, maxContentWidth));
            /// @note 初回フォーカスを InputText に当てる
            if (s_state.inputNeedsFocus) {
                ImGui::SetKeyboardFocusHere();
                s_state.inputNeedsFocus = false;
            }
            const bool entered = ImGui::InputText("##input", s_state.inputBuf,
                                                  sizeof(s_state.inputBuf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if (!s_state.note.empty()) {
                ImGui::TextDisabled("%s", s_state.note.c_str());
            }
            ImGui::Spacing();
            const bool ok = ImGui::Button(LOCT("Create"), { buttonWidth(2), 0 }) || entered;
            ImGui::SameLine();
            const bool cancel = ImGui::Button(LOCT("Cancel"), { buttonWidth(2), 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (ok && s_state.inputBuf[0] != '\0') {
                if (s_state.onInput) s_state.onInput(s_state.inputBuf);
                s_state.pending = false;
                s_state.opened  = false;
                ImGui::CloseCurrentPopup();
            } else if (cancel) {
                s_state.pending = false;
                s_state.opened  = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return;
        }

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + available.x * 0.55f);
        ImGui::TextUnformatted(s_state.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        if (s_state.onSave || s_state.onDiscard) {
            if (ImGui::Button(LOCT("Save"), { buttonWidth(3), 0 })) {
                const bool saved = s_state.onSave ? s_state.onSave() : true;
                if (saved) {
                    s_state.pending = false;
                    s_state.opened = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(LOCT("Discard"), { buttonWidth(3), 0 })) {
                if (s_state.onDiscard) s_state.onDiscard();
                s_state.pending = false;
                s_state.opened = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(LOCT("Cancel"), { buttonWidth(3), 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                s_state.pending = false;
                s_state.opened = false;
                ImGui::CloseCurrentPopup();
            }
        } else {
            if (ImGui::Button("OK", { buttonWidth(2), 0 })) {
                if (s_state.onConfirm) s_state.onConfirm();
                s_state.pending = false;
                s_state.opened = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(LOCT("Cancel"), { buttonWidth(2), 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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

} /// @note namespace fbzz::editor
