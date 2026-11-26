/// @file    HotkeyEditorPanel.cpp
/// @brief   ホットキー一覧表示とリバインド UI。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#include <Editor/Panels/Project/HotkeyEditorPanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/HotkeyManager.hpp>
#include <Editor/UI/Localization.hpp>
#include "../../UI/PanelToolbar.hpp"
#include <Engine/Util/Text/StringUtils.hpp>
#include <imgui.h>

namespace fbzz::editor {

namespace {

std::string ScopeLabel(HotkeyScope scope)
{
    if (HasScope(scope, HotkeyScope::Global)) {
        return LOCT("Anywhere");
    }
    struct Entry { HotkeyScope scope; const char* label; };
    constexpr Entry scopes[] = {
        { HotkeyScope::SceneViewport, "Scene View" },
        { HotkeyScope::Hierarchy, "Hierarchy" },
        { HotkeyScope::AssetBrowser, "Assets" },
        { HotkeyScope::FluidEditor, "Fluid Editor" },
        { HotkeyScope::Sequence, "Sequence" },
        { HotkeyScope::BehaviorTree, "Behavior Tree" },
        { HotkeyScope::SpriteEditor, "Sprite Editor" },
        { HotkeyScope::AnimationGraph, "Animation Graph" },
        { HotkeyScope::HotkeyEditor, "Hotkey Editor" },
    };
    std::string text;
    for (const auto& entry : scopes) {
        if (!HasScope(scope, entry.scope)) {
            continue;
        }
        if (!text.empty()) {
            text += " / ";
        }
        text += LOCT(entry.label);
    }
    return text;
}

} // namespace

void HotkeyEditorPanel::OnRegisterOperators(OperatorRegistry& registry)
{
    EditorOperator inspect;
    inspect.id = "hotkey.capture.inspect";
    inspect.label = "Inspect Hotkey Capture";
    inspect.category = "Hotkeys";
    inspect.kind = OpKind::Query;
    inspect.desc = "キー入力待機中の対象と競合メッセージを返す。";
    inspect.exec = [this](OpContext&, const OpArgs&) {
        auto data = OpData::MakeObject();
        data.Set("target", m_rebindTarget);
        data.Set("conflict", m_conflictNote);
        return OpResult::Data(std::move(data));
    };
    registry.Register(std::move(inspect));

    EditorOperator control;
    control.id = "hotkey.capture.control";
    control.label = "Control Hotkey Capture";
    control.category = "Hotkeys";
    control.kind = OpKind::Action;
    control.desc = "キー待機を開始・取消し、競合表示を閉じる。キー割当はworkspace.settings.configureでも変更可能。";
    control.params = {
        {"action", OpParamType::String, "Capture action", true, {}},
        {"target", OpParamType::String, "Hotkey display name", false, {}}
    };
    control.params.front().enumValues = {"begin", "cancel", "dismiss"};
    control.poll = [](const OpContext& context, const OpArgs&) { return context.ctx.hotkeyManager != nullptr; };
    control.exec = [this](OpContext& context, const OpArgs& args) {
        const auto action = args.GetString("action");
        if (action == "begin") {
            const auto target = args.GetString("target");
            bool found = false;
            for (const auto& key : context.ctx.hotkeyManager->GetHotkeys())
                if (key.name == target && !key.infoOnly) {
                    found = true;
                    break;
                }
            if (!found) {
                return OpResult::Err("HOTKEY_NOT_FOUND", "target");
            }
            m_rebindTarget = target;
        } else if (action == "cancel") {
            m_rebindTarget.clear();
        } else if (action != "dismiss") {
            return OpResult::Err("BAD_ARG", "action");
        }
        m_conflictNote.clear();
        return OpResult::Ok();
    };
    registry.Register(std::move(control));
}

void HotkeyEditorPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.hotkeyManager) {
        ImGui::TextDisabled("HotkeyManager not available.");
        return;
    }

    // リバインド待ち中: 次のキー入力を捕捉する
    const bool capturing = !m_rebindTarget.empty();
    if (capturing) {
        ImGui::TextColored({ 0.4f, 0.9f, 1.0f, 1.0f },
            "Press any key to rebind \"%s\"  (Esc = cancel)",
            m_rebindTarget.c_str());
        ImGui::Separator();

        if (!ctx.PanelScopeFocused(HotkeyScope::HotkeyEditor)) {
            ImGui::TextDisabled("Focus this panel to enter the new shortcut.");
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_rebindTarget.clear();
            m_conflictNote.clear();
        } else {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
                if (k == ImGuiKey_Escape) {
                    continue;
                }
                if (k == ImGuiKey_LeftCtrl  || k == ImGuiKey_RightCtrl) {
                    continue;
                }
                if (k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift) {
                    continue;
                }
                if (k == ImGuiKey_LeftAlt   || k == ImGuiKey_RightAlt) {
                    continue;
                }
                if (k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper) {
                    continue;
                }
                if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
                    continue;
                }

                const bool ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
                const bool alt   = ImGui::IsKeyDown(ImGuiKey_LeftAlt)   || ImGui::IsKeyDown(ImGuiKey_RightAlt);

                const std::string conflict =
                    ctx.hotkeyManager->FindConflict(m_rebindTarget, k, ctrl, shift, alt);
                if (!conflict.empty()) {
                    m_conflictNote = "This shortcut is already used by \"" + conflict + "\".";
                    break;
                }
                ctx.hotkeyManager->Rebind(m_rebindTarget, k, ctrl, shift, alt);
                m_conflictNote.clear();
                m_rebindTarget.clear();
                break;
            }
        }
    }

    if (!m_conflictNote.empty()) {
        ImGui::TextColored({ 1.0f, 0.75f, 0.25f, 1.0f }, "%s", m_conflictNote.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(LOC("Dismiss"))) {
            m_conflictNote.clear();
        }
        ImGui::Separator();
    }

    const auto& hotkeys = ctx.hotkeyManager->GetHotkeys();
    if (hotkeys.empty()) {
        ImGui::TextDisabled("No hotkeys registered.");
        return;
    }

    if (!capturing) {
        toolbar::SearchField("##hotkey_search", "Search actions, shortcuts or contexts...", m_search, sizeof(m_search));
    }

    if (ImGui::BeginTable("##hotkeys", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn(LOCT("Action"),   ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(LOCT("Context"),  ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(LOCT("Binding"),  ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed,   toolbar::ButtonWidth(LOCT("Rebind")));
        ImGui::TableHeadersRow();

        bool anyShown = false;
        for (const auto& hk : hotkeys) {
            const char* action = LOCT(hk.name.c_str());
            const std::string scopeText = ScopeLabel(hk.scope);
            const std::string binding = HotkeyManager::FormatBinding(hk);
            if (m_search[0] && hk.name != m_rebindTarget
                && !util::StringUtils::ContainsCI(action, m_search)
                && !util::StringUtils::ContainsCI(hk.name, m_search)
                && !util::StringUtils::ContainsCI(binding, m_search)
                && !util::StringUtils::ContainsCI(scopeText, m_search)) {
                continue;
            }
            anyShown = true;
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(action);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", scopeText.c_str());

            ImGui::TableSetColumnIndex(2);
            const bool waiting = (m_rebindTarget == hk.name);
            if (waiting) {
                ImGui::TextColored({ 0.4f, 0.9f, 1.0f, 1.0f }, "...");
            } else {
                ImGui::TextUnformatted(binding.c_str());
            }

            ImGui::TableSetColumnIndex(3);
            // 説明専用エントリ (マウス操作など) は割り当てを持たないのでリバインドできない。
            if (hk.infoOnly) {
                ImGui::TextDisabled("-");
            } else {
                ImGui::PushID(hk.name.c_str());
                if (ImGui::SmallButton(LOC("Rebind"))) {
                    m_rebindTarget = hk.name;
                    m_conflictNote.clear();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        if (!anyShown) {
            ImGui::TextDisabled("%s", LOCT("No matching shortcuts"));
        }
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", LOCT("Shortcut changes are saved when the editor exits."));
    ImGui::PopTextWrapPos();
}

} // namespace fbzz::editor
