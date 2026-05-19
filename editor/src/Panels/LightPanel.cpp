// FBZZ Engine
// LightPanel.cpp | fbzz::editor
// Directional / Point / Spot ライトをスライダーで編集する
#include <editor/Panels/LightPanel.hpp>
#include <editor/EditorContext.hpp>
#include <engine/Renderer/LightSystem.hpp>
#include <imgui.h>
#include <cstdio>

namespace fbzz::editor {

void LightPanel::OnRender(EditorContext& ctx)
{
    if (!ImGui::Begin("Lights")) { ImGui::End(); return; }

    if (!ctx.lightSystem) {
        ImGui::TextDisabled("No light system");
        ImGui::End();
        return;
    }

    renderer::LightSystem& ls = *ctx.lightSystem;

    // Directional Light
    if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderer::DirectionalLight dir = ls.GetDirectional();
        bool changed = false;

        float dirArr[3] = { dir.direction.x, dir.direction.y, dir.direction.z };
        if (ImGui::DragFloat3("Direction##dir", dirArr, 0.01f, -1.0f, 1.0f))
        { dir.direction = { dirArr[0], dirArr[1], dirArr[2] }; changed = true; }

        float col[3] = { dir.color.x, dir.color.y, dir.color.z };
        if (ImGui::ColorEdit3("Color##dir", col))
        { dir.color = { col[0], col[1], col[2] }; changed = true; }

        if (ImGui::DragFloat("Intensity##dir", &dir.intensity, 0.01f, 0.0f, 100.0f))
            changed = true;

        if (changed) ls.SetDirectional(dir);
    }

    // Point Lights
    auto& points = ls.GetPointLights();
    char header[64];
    std::snprintf(header, sizeof(header), "Point Lights (%d)", (int)points.size());
    if (ImGui::CollapsingHeader(header)) {
        for (int i = 0; i < (int)points.size(); ++i) {
            ImGui::PushID(i);
            auto& p = points[i];

            float pos[3] = { p.position.x, p.position.y, p.position.z };
            if (ImGui::DragFloat3("Position", pos, 0.1f))
                p.position = { pos[0], pos[1], pos[2] };

            ImGui::DragFloat("Range",     &p.range,     0.1f,  0.0f, 500.0f);
            ImGui::DragFloat("Intensity", &p.intensity, 0.1f,  0.0f, 200.0f);

            float col[3] = { p.color.x, p.color.y, p.color.z };
            if (ImGui::ColorEdit3("Color", col))
                p.color = { col[0], col[1], col[2] };

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    // Spot Lights
    auto& spots = ls.GetSpotLights();
    std::snprintf(header, sizeof(header), "Spot Lights (%d)", (int)spots.size());
    if (ImGui::CollapsingHeader(header)) {
        for (int i = 0; i < (int)spots.size(); ++i) {
            ImGui::PushID(100 + i);
            auto& s = spots[i];

            float pos[3] = { s.position.x, s.position.y, s.position.z };
            if (ImGui::DragFloat3("Position", pos, 0.1f))
                s.position = { pos[0], pos[1], pos[2] };

            float dir[3] = { s.direction.x, s.direction.y, s.direction.z };
            if (ImGui::DragFloat3("Direction", dir, 0.01f, -1.0f, 1.0f))
                s.direction = { dir[0], dir[1], dir[2] };

            ImGui::DragFloat("Range",     &s.range,     0.1f,  0.0f, 500.0f);
            ImGui::DragFloat("Intensity", &s.intensity, 0.1f,  0.0f, 200.0f);
            ImGui::DragFloat("Inner Cos", &s.innerCos,  0.001f, 0.0f, 1.0f);
            ImGui::DragFloat("Outer Cos", &s.outerCos,  0.001f, 0.0f, 1.0f);

            float col[3] = { s.color.x, s.color.y, s.color.z };
            if (ImGui::ColorEdit3("Color", col))
                s.color = { col[0], col[1], col[2] };

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    ImGui::End();
}

} // namespace fbzz::editor
