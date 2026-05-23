// FBZZ Engine
// ProjectSettingsPanel.cpp | fbzz::editor
// タグ・レイヤー名の編集 UI
#include <Editor/Panels/ProjectSettingsPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <imgui.h>
#include <cstdio>

namespace fbzz::editor {

void ProjectSettingsPanel::OnRenderContent(EditorContext& ctx)
{
    auto& ps = ctx.projectSettings;

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
