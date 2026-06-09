// FBZZ Engine
// InspectorCore.cpp | fbzz::editor
// Transform / Script の Inspector 描画
#include "InspectorCore.hpp"

namespace fbzz::editor {

void DrawTransformInspector(scene::GameObject* go, EditorContext& ctx)
{
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& t = go->transform;
        ImGui::Spacing();

        const bool isUI = go->GetComponent<scene::UIImage>() || go->GetComponent<scene::UIText>();

        if (isUI) {
            const float itemW = (ImGui::GetContentRegionAvail().x
                                 - ImGui::CalcTextSize("X").x * 2
                                 - ImGui::GetStyle().ItemSpacing.x * 3) * 0.5f;

            ImGui::Text("Pos");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(itemW);
            ImGui::DragFloat("##px", &t.localPosition.x, 1.0f, 0.0f, 0.0f, "X %.0f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(itemW);
            ImGui::DragFloat("##py", &t.localPosition.y, 1.0f, 0.0f, 0.0f, "Y %.0f");

            math::Vector3 euler = widgets::QuatToEulerDeg(t.localRotation);
            float rotZ = euler.z;
            ImGui::Text("Rot");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##rz", &rotZ, 0.5f, -360.0f, 360.0f, "Z %.1f deg"))
                t.localRotation = widgets::EulerDegToQuat({ euler.x, euler.y, rotZ });

            if (go->GetComponent<scene::UIImage>()) {
                ImGui::Text("Size");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(itemW);
                ImGui::DragFloat("##sw", &t.localScale.x, 1.0f, 1.0f, 0.0f, "W %.0f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(itemW);
                ImGui::DragFloat("##sh", &t.localScale.y, 1.0f, 1.0f, 0.0f, "H %.0f");
            }
        } else {
            float pos[3] = { t.localPosition.x, t.localPosition.y, t.localPosition.z };
            if (ImGui::DragFloat3("Position", pos, 0.1f))
                t.localPosition = { pos[0], pos[1], pos[2] };

            widgets::DragQuatEuler3("Rotation", t.localRotation, 0.5f);

            float scale[3] = { t.localScale.x, t.localScale.y, t.localScale.z };
            if (ImGui::DragFloat3("Scale", scale, 0.01f, 0.001f, 1000.0f))
                t.localScale = { scale[0], scale[1], scale[2] };
        }

        ImGui::Spacing();
    }

}

void DrawScriptInspectors(scene::GameObject* go, EditorContext& ctx)
{
    if (auto* sc = go->GetComponent<scene::ScriptComponent>()) {
        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(sc->scripts.size()); ++i) {
            auto& entry = sc->scripts[static_cast<size_t>(i)];
            ImGui::PushID(i);

            if (entry.script) {
                const char* header = entry.script->GetTypeName();
                ImGui::Checkbox("##en", &entry.script->enabled);
                ImGui::SameLine();

                const bool open = ImGui::CollapsingHeader(header,
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

                const float btnW = ImGui::GetFrameHeight();
                ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW);
                if (ImGui::SmallButton("..."))
                    ImGui::OpenPopup("##script_opts");

                if (ImGui::BeginPopup("##script_opts")) {
                    if (ImGui::MenuItem("Remove Component"))
                        removeIndex = i;
                    ImGui::EndPopup();
                }

                if (open) {
                    ImGui::Spacing();
                    ImGuiReflector reflector;
                    if (ctx.activeScene) {
                        reflector.m_goNameResolver = [scene = ctx.activeScene](scene::EntityID id) -> std::string {
                            auto* go = scene->GetGameObject(id);
                            return go ? go->name : "(Missing)";
                        };
                    }
                    entry.script->Reflect(reflector);
                    ImGui::Spacing();
                }
            } else if (entry.serialized && !entry.serialized->type.empty()) {
                ImGui::Checkbox("##en", &entry.serialized->enabled);
                ImGui::SameLine();

                const std::string header = "Missing Script: " + entry.serialized->type;
                const bool open = ImGui::CollapsingHeader(
                    header.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

                const float btnW = ImGui::GetFrameHeight();
                ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW);
                if (ImGui::SmallButton("..."))
                    ImGui::OpenPopup("##missing_script_opts");

                if (ImGui::BeginPopup("##missing_script_opts")) {
                    if (ImGui::MenuItem("Remove Component"))
                        removeIndex = i;
                    ImGui::EndPopup();
                }

                if (open) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Script DLL is not loaded. Serialized fields are preserved.");
                    ImGui::Spacing();
                }
            }

            ImGui::PopID();
        }

        if (removeIndex >= 0) {
            sc->scripts.erase(sc->scripts.begin() + removeIndex);
            if (sc->scripts.empty())
                go->RemoveComponent<scene::ScriptComponent>();
        }
    }

}

} // namespace fbzz::editor
