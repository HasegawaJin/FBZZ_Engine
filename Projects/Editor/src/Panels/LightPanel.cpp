// FBZZ Engine
// LightPanel.cpp | fbzz::editor
// シーン内の LightComponent を一覧表示・編集する
#include <Editor/Panels/LightPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <imgui.h>
#include <cstdio>

namespace fbzz::editor {

void LightPanel::OnRender(EditorContext& ctx)
{
    if (!ImGui::Begin("Lights")) { ImGui::End(); return; }

    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        ImGui::End();
        return;
    }

    int lightIdx = 0;
    for (auto& go : ctx.activeScene->GameObjects()) {
        auto* lc = go.GetComponent<scene::LightComponent>();
        if (!lc) continue;

        ImGui::PushID(lightIdx++);

        const char* typeName = lc->type == scene::LightComponent::Type::Directional ? "Directional"
                             : lc->type == scene::LightComponent::Type::Point       ? "Point"
                             : "Spot";
        char header[128];
        std::snprintf(header, sizeof(header), "[%s] %s", typeName, go.name.c_str());

        if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
            // Type
            const char* types[] = { "Directional", "Point", "Spot" };
            int typeIdx = static_cast<int>(lc->type);
            if (ImGui::Combo("Type", &typeIdx, types, 3))
                lc->type = static_cast<scene::LightComponent::Type>(typeIdx);

            // Color + Intensity (全タイプ共通)
            float col[3] = { lc->color.x, lc->color.y, lc->color.z };
            if (ImGui::ColorEdit3("Color", col))
                lc->color = { col[0], col[1], col[2] };
            ImGui::DragFloat("Intensity", &lc->intensity, 0.01f, 0.0f, 200.0f);
            ImGui::Checkbox("Enabled", &lc->enabled);

            // Position (Point / Spot)
            if (lc->type != scene::LightComponent::Type::Directional) {
                float pos[3] = { go.transform.localPosition.x,
                                 go.transform.localPosition.y,
                                 go.transform.localPosition.z };
                if (ImGui::DragFloat3("Position", pos, 0.1f))
                    go.transform.localPosition = { pos[0], pos[1], pos[2] };
                ImGui::DragFloat("Range", &lc->range, 0.1f, 0.0f, 500.0f);
            }

            // Cone (Spot のみ)
            if (lc->type == scene::LightComponent::Type::Spot) {
                ImGui::DragFloat("Inner Cone (deg)", &lc->innerCone, 0.5f, 0.0f, 89.0f);
                ImGui::DragFloat("Outer Cone (deg)", &lc->outerCone, 0.5f, 0.0f, 89.0f);
            }

            // 方向表示 (Directional / Spot は Transform::Forward を読み取り専用表示)
            if (lc->type != scene::LightComponent::Type::Point) {
                auto fwd = go.transform.Forward();
                float dir[3] = { fwd.x, fwd.y, fwd.z };
                ImGui::InputFloat3("Forward (read-only)", dir, "%.3f",
                                   ImGuiInputTextFlags_ReadOnly);
            }
        }

        ImGui::PopID();
    }

    if (lightIdx == 0)
        ImGui::TextDisabled("No LightComponent in scene");

    ImGui::End();
}

} // namespace fbzz::editor
