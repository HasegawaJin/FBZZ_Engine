// FBZZ Engine
// ImGuiWidgets.cpp | fbzz::editor
// プロジェクト固有の ImGui カスタムウィジェット実装
#include <editor/Util/ImGuiWidgets.hpp>
#include <math/Vector3.hpp>
#include <imgui.h>

namespace fbzz::editor::widgets {

bool DragVec3(const char* label, math::Vector3& v, float speed, float min, float max)
{
    ImGui::PushID(label);
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 100.0f);
    ImGui::Text("%s", label);
    ImGui::NextColumn();
    float arr[3] = { v.x, v.y, v.z };
    bool changed = ImGui::DragFloat3("##v", arr, speed, min, max);
    if (changed) { v.x = arr[0]; v.y = arr[1]; v.z = arr[2]; }
    ImGui::Columns(1);
    ImGui::PopID();
    return changed;
}

bool ColorEdit3(const char* label, math::Vector3& color)
{
    float arr[3] = { color.x, color.y, color.z };
    bool changed = ImGui::ColorEdit3(label, arr);
    if (changed) { color.x = arr[0]; color.y = arr[1]; color.z = arr[2]; }
    return changed;
}

void SectionHeader(const char* label)
{
    ImGui::Separator();
    ImGui::TextColored({ 0.9f, 0.7f, 0.3f, 1.0f }, "%s", label);
    ImGui::Separator();
}

void ColoredText(const char* text, ImVec4 color)
{
    ImGui::TextColored(color, "%s", text);
}

void ReadOnlyText(const char* label, const char* text)
{
    ImGui::LabelText(label, "%s", text);
}

} // namespace fbzz::editor::widgets
