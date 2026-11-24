/// @file    PanelToolbar.cpp
/// @brief   パネル単位の検索入力と、実測幅に基づくツールバー配置。
/// @author  Hasegawa Jin
/// @date    2026-11-23
#include "PanelToolbar.hpp"
#include <Editor/Util/Localization.hpp>
#include <imgui_internal.h>
#include <algorithm>

namespace fbzz::editor::toolbar {

float ButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x + 2.0f * ImGui::GetStyle().FramePadding.x;
}

float ButtonWidth(const char* label, const icons::detail::Glyph& icon)
{
    const float extra = icons::Available() ? ImGui::CalcTextSize(icon.text).x + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
    return ButtonWidth(label) + extra;
}

bool IconButton(const char* label, const icons::detail::Glyph& icon)
{
    if (!icons::Available()) return ImGui::Button(label);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(1.0f, 0.5f));
    const bool pressed = ImGui::Button(label, {ButtonWidth(label, icon), 0.0f});
    ImGui::PopStyleVar();
    if (ImGui::IsItemVisible()) {
        const auto origin = ImGui::GetItemRectMin();
        const auto padding = ImGui::GetStyle().FramePadding;
        ImGui::GetWindowDrawList()->AddText({origin.x + padding.x, origin.y + padding.y},
                                           ImGui::GetColorU32(ImGuiCol_Text), icon.text);
    }
    return pressed;
}

float Action::Width(bool compact) const
{
    return compact ? ImGui::GetFrameHeight() : ButtonWidth(label, icon);
}

bool Action::Draw(bool compact) const
{
    bool pressed;
    if (compact) {
        const float side = ImGui::GetFrameHeight();
        ImGui::PushOverrideID(ImGui::GetID(label));
        pressed = ImGui::Button("", {side, side});
        ImGui::PopID();
        if (ImGui::IsItemVisible()) {
            const char* mark = icons::Or(icon, fallback);
            const auto size = ImGui::CalcTextSize(mark);
            const auto min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText({min.x + (side - size.x) * 0.5f, min.y + (side - size.y) * 0.5f},
                                                ImGui::GetColorU32(ImGuiCol_Text), mark);
        }
    } else {
        pressed = IconButton(label, icon);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

void SameLineIfFits(float nextWidth)
{
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + nextWidth <= right)
        ImGui::SameLine();
}

void SearchField(const char* id, const char* hint, char* buffer, std::size_t capacity, float width)
{
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    width = width > 0.0f ? std::min(width, available) : available;
    const auto& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    const float height = ImGui::GetFrameHeight();
    const bool clearVisible = buffer[0] != '\0' && width >= height + style.ItemInnerSpacing.x + 2.0f * font;
    const float inputWidth = clearVisible ? width - height - style.ItemInnerSpacing.x : width;
    const bool showLens = inputWidth >= 6.0f * font;
    const float padding = style.FramePadding.x;
    const ImGuiID inputId = ImGui::GetID(id);

    ImGui::BeginGroup();
    /// @note 同じドックのツリーや一覧からも検索できる。別パネルや開いたメニューからはフォーカスを奪わない。
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy))
        ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteGlobal);
    ImGui::SetNextItemWidth(inputWidth);
    if (showLens) ImGui::PushStyleVarX(ImGuiStyleVar_FramePadding, padding + font);
    ImGui::InputTextWithHint(id, LOCT(hint), buffer, capacity, ImGuiInputTextFlags_EscapeClearsAll);
    if (showLens) {
        ImGui::PopStyleVar();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 lens{ min.x + padding + font * 0.28f, min.y + height * 0.46f };
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        auto& draw = *ImGui::GetWindowDrawList();
        draw.AddCircle(lens, font * 0.25f, color, 16, 1.4f);
        draw.AddLine({ lens.x + font * 0.17f, lens.y + font * 0.17f },
                     { lens.x + font * 0.47f, lens.y + font * 0.47f }, color, 1.4f);
    }
    ImGui::SetItemTooltip("%s", LOCT("Ctrl+F to search, Esc to clear."));
    if (clearVisible) {
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        ImGui::PushID(id);
        if (ImGui::Button("##clear", { height, height })) {
            buffer[0] = '\0';
            ImGui::ActivateItemByID(inputId);
        }
        ImGui::PopID();
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const float inset = height * 0.34f;
        auto& draw = *ImGui::GetWindowDrawList();
        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
        draw.AddLine({ min.x + inset, min.y + inset }, { max.x - inset, max.y - inset }, ink, 1.4f);
        draw.AddLine({ max.x - inset, min.y + inset }, { min.x + inset, max.y - inset }, ink, 1.4f);
        ImGui::SetItemTooltip("%s", LOCT("Clear search"));
    }
    ImGui::EndGroup();
}

}
