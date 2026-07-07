// FBZZ Engine
// SelectionVisuals.hpp | fbzz::editor
// Editor 全体で共有する選択・ホバー表示の描画ヘルパー
//
// WHY: Hierarchy / AssetBrowser / Inspector がそれぞれ別の色・太さで選択表示すると、
//      ユーザーは「現在の操作対象」を毎回読み直す必要がある。選択・ホバー・主選択を
//      共通パレットに集約し、視線移動だけで状態を判断できるようにする。
#pragma once

#include <imgui.h>

namespace fbzz::editor::ui {

inline ImU32 SelectionFillColor(bool primary)
{
    return primary ? IM_COL32(255, 196, 64, 48) : IM_COL32(72, 148, 255, 42);
}

inline ImU32 SelectionBorderColor(bool primary)
{
    return primary ? IM_COL32(255, 205, 92, 235) : IM_COL32(92, 172, 255, 215);
}

inline ImU32 HoverFillColor()
{
    return IM_COL32(255, 255, 255, 18);
}

inline void DrawSelectionBackground(ImDrawList* dl,
                                    ImVec2 min,
                                    ImVec2 max,
                                    bool selected,
                                    bool hovered,
                                    bool primary,
                                    float rounding = 4.0f)
{
    if (!dl) return;
    if (hovered)
        dl->AddRectFilled(min, max, HoverFillColor(), rounding);
    if (selected) {
        dl->AddRectFilled(min, max, SelectionFillColor(primary), rounding);
        dl->AddRect(min, max, SelectionBorderColor(primary), rounding, 0, primary ? 2.0f : 1.35f);
    }
}

inline void DrawSelectionAccent(ImDrawList* dl,
                                ImVec2 min,
                                ImVec2 max,
                                bool selected,
                                bool primary,
                                float rounding = 3.0f)
{
    if (!dl || !selected) return;
    const ImU32 color = SelectionBorderColor(primary);
    dl->AddRectFilled(min, { min.x + 3.0f, max.y }, color, rounding);
}

inline void PushHierarchySelectionColors()
{
    ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(72, 148, 255, 54));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 22));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(255, 196, 64, 70));
}

inline void PopHierarchySelectionColors()
{
    ImGui::PopStyleColor(3);
}

} // namespace fbzz::editor::ui
