// FBZZ Engine
// ProjectSettingsPanel.cpp | fbzz::editor
// タグ・レイヤー名の編集 UI
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Core/Time.hpp>
#include <imgui.h>
#include <cstdio>

namespace fbzz::editor {

void ProjectSettingsPanel::OnRenderContent(EditorContext& ctx)
{
    auto& ps = ctx.projectSettings;

    if (ImGui::CollapsingHeader("Application", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::DragInt("Target FPS", &ps.app.targetFps, 1.0f, 0, 360))
            core::Time::SetTargetFps(ps.app.targetFps);
        ImGui::SameLine();
        ImGui::TextDisabled("(0 = unlimited)");
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Screen", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::DragInt("Width",  &ps.screen.width,  1.0f, 1, 7680);
        ImGui::DragInt("Height", &ps.screen.height, 1.0f, 1, 4320);
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("BGM Volume", &ps.audio.bgmVolume, 0.0f, 1.0f);
        ImGui::SliderFloat("SE Volume",  &ps.audio.seVolume,  0.0f, 1.0f);
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& r = ps.render;
        ImGui::Checkbox("Shadow",      &r.shadowEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Bloom",       &r.bloomEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Fog",         &r.fogEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("FXAA",        &r.fxaaEnabled);
        ImGui::Checkbox("Wireframe",   &r.wireframeMode);
        ImGui::SameLine();
        ImGui::Checkbox("Colliders",   &r.showColliders);
        ImGui::SliderFloat("Exposure",    &r.exposure,   0.1f, 4.0f);
        ImGui::SliderFloat("Fog Density", &r.fogDensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Fog Far",     &r.fogFar,     1.0f, 100.0f);
        ImGui::ColorEdit3("Fog Color",    r.fogColor);
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::DragInt("Hz",       &ps.physics.hz,       1.0f, 1, 1000);
        ImGui::DragInt("Substeps", &ps.physics.substeps, 1.0f, 1, 32);

        float gravity[3] = { ps.physics.gravity.x, ps.physics.gravity.y, ps.physics.gravity.z };
        if (ImGui::DragFloat3("Gravity", gravity, 0.05f, -1000.0f, 1000.0f))
            ps.physics.gravity = { gravity[0], gravity[1], gravity[2] };
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Tags", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int removeIdx = -1;
        for (int i = 0; i < (int)ps.tags.size(); ++i)
        {
            ImGui::PushID(i);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", ps.tags[i].c_str());
            ImGui::SetNextItemWidth(-60.0f);
            if (ImGui::InputText("##tag", buf, sizeof(buf)))
                ps.tags[i] = buf;
            ImGui::SameLine();
            if (ps.tags[i] != "Untagged" && ImGui::SmallButton("Remove"))
                removeIdx = i;
            ImGui::PopID();
        }
        if (removeIdx >= 0)
            ps.tags.erase(ps.tags.begin() + removeIdx);

        ImGui::Spacing();
        static char s_newTag[64] = {};
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("##newtag", s_newTag, sizeof(s_newTag));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add") && s_newTag[0] != '\0')
        {
            ps.tags.push_back(s_newTag);
            s_newTag[0] = '\0';
        }
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int i = 0; i < 32; ++i)
        {
            ImGui::PushID(i);
            ImGui::Text("%2d", i);
            ImGui::SameLine();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", ps.layerNames[i].c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##layer", buf, sizeof(buf)))
                ps.layerNames[i] = buf;
            ImGui::PopID();
        }
    }
}

} // namespace fbzz::editor
