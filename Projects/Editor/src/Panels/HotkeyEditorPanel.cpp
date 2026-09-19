/// @file    HotkeyEditorPanel.cpp
/// @brief   ホットキー一覧表示とリバインド UI。
/// @author  Hasegawa Jin
/// @date    2026-06-16
///
/// 一覧・整形・競合判定は HotkeyManager 側に持たせ、このパネルは表示と入力待ちだけを担当する
/// (整形処理を F1 オーバーレイと重複させない)。
#include <Editor/Panels/HotkeyEditorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <imgui.h>

namespace fbzz::editor {

void HotkeyEditorPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.hotkeyManager) {
        ImGui::TextDisabled("HotkeyManager not available.");
        return;
    }

    /// @note リバインド待ち中: 次のキー入力を捕捉する
    if (!m_rebindTarget.empty()) {
        ImGui::TextColored({ 0.4f, 0.9f, 1.0f, 1.0f },
            "Press any key to rebind \"%s\"  (Esc = cancel)",
            m_rebindTarget.c_str());
        ImGui::Separator();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_rebindTarget.clear();
            m_conflictNote.clear();
        } else {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                if (k == ImGuiKey_Escape) continue;
                if (k == ImGuiKey_LeftCtrl  || k == ImGuiKey_RightCtrl)  continue;
                if (k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift) continue;
                if (k == ImGuiKey_LeftAlt   || k == ImGuiKey_RightAlt)   continue;
                if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) continue;

                const bool ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
                const bool alt   = ImGui::IsKeyDown(ImGuiKey_LeftAlt)   || ImGui::IsKeyDown(ImGuiKey_RightAlt);

                /// @note 既存の割り当てを黙って潰すと後で気づけないため、割り当ては行ったうえで警告を残す。
                const std::string conflict =
                    ctx.hotkeyManager->FindConflict(m_rebindTarget, k, ctrl, shift, alt);
                ctx.hotkeyManager->Rebind(m_rebindTarget, k, ctrl, shift, alt);
                m_conflictNote = conflict.empty()
                    ? std::string{}
                    : ("\"" + m_rebindTarget + "\" now shares its binding with \"" + conflict + "\"");
                m_rebindTarget.clear();
                break;
            }
        }
    }

    if (!m_conflictNote.empty()) {
        ImGui::TextColored({ 1.0f, 0.75f, 0.25f, 1.0f }, "%s", m_conflictNote.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) m_conflictNote.clear();
        ImGui::Separator();
    }

    const auto& hotkeys = ctx.hotkeyManager->GetHotkeys();
    if (hotkeys.empty()) {
        ImGui::TextDisabled("No hotkeys registered.");
        return;
    }

    if (ImGui::BeginTable("##hotkeys", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Context",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Binding",  ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed,   80.0f);
        ImGui::TableHeadersRow();

        for (const auto& hk : hotkeys) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(hk.name.c_str());

            /// @note どこにフォーカスがあるとき効くのかを出す。同じ Delete が複数のスコープで効くことが
            ///       分からないと「効いたり効かなかったりする」ように見える。
            ImGui::TableSetColumnIndex(1);
            std::string scopeText;
            if (HasScope(hk.scope, HotkeyScope::Global))        scopeText = "Anywhere";
            else {
                if (HasScope(hk.scope, HotkeyScope::SceneViewport)) scopeText += "Scene View";
                if (HasScope(hk.scope, HotkeyScope::Hierarchy))
                    scopeText += scopeText.empty() ? "Hierarchy" : " / Hierarchy";
                if (HasScope(hk.scope, HotkeyScope::AssetBrowser))
                    scopeText += scopeText.empty() ? "Assets" : " / Assets";
                if (HasScope(hk.scope, HotkeyScope::FluidEditor))
                    scopeText += scopeText.empty() ? "Fluid Editor" : " / Fluid Editor";
            }
            ImGui::TextDisabled("%s", scopeText.c_str());

            ImGui::TableSetColumnIndex(2);
            const bool waiting = (m_rebindTarget == hk.name);
            if (waiting)
                ImGui::TextColored({ 0.4f, 0.9f, 1.0f, 1.0f }, "...");
            else
                ImGui::TextUnformatted(HotkeyManager::FormatBinding(hk).c_str());

            ImGui::TableSetColumnIndex(3);
            /// @note 説明専用エントリ (マウス操作など) は割り当てを持たないのでリバインドできない。
            if (hk.infoOnly) {
                ImGui::TextDisabled("-");
            } else {
                ImGui::PushID(hk.name.c_str());
                if (ImGui::SmallButton("Rebind")) {
                    m_rebindTarget = hk.name;
                    m_conflictNote.clear();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Rebinds are saved to editor_settings.toml when the editor exits.");
}

} // namespace fbzz::editor
