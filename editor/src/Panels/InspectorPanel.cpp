// FBZZ Engine
// InspectorPanel.cpp | fbzz::editor
// 選択 Entity のコンポーネントを表示・編集する
#include <editor/Panels/InspectorPanel.hpp>
#include <editor/EditorContext.hpp>
#include <editor/ImGuiReflector.hpp>
#include <editor/Util/ImGuiWidgets.hpp>
#include <engine/Scene/Scene.hpp>
#include <engine/Scene/GameObject.hpp>
#include <imgui.h>
#include <cstdio>

namespace fbzz::editor {

void InspectorPanel::OnRender(EditorContext& ctx)
{
    if (!ImGui::Begin("Inspector")) { ImGui::End(); return; }

    scene::EntityID sel = ctx.PrimarySelected();
    if (!sel.IsValid() || !ctx.activeScene) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }

    scene::GameObject* go = ctx.activeScene->GetGameObject(sel);
    if (!go) { ImGui::End(); return; }

    // 名前フィールド
    char nameBuf[256];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", go->name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        go->name = nameBuf;

    ImGui::Separator();

    // Transform
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& t = go->transform;

        float pos[3] = { t.localPosition.x, t.localPosition.y, t.localPosition.z };
        if (ImGui::DragFloat3("Position", pos, 0.1f))
            t.localPosition = { pos[0], pos[1], pos[2] };

        math::Vector3 euler = widgets::QuatToEulerDeg(t.localRotation);
        float rot[3] = { euler.x, euler.y, euler.z };
        if (ImGui::DragFloat3("Rotation", rot, 0.5f))
            t.localRotation = widgets::EulerDegToQuat({ rot[0], rot[1], rot[2] });

        float scale[3] = { t.localScale.x, t.localScale.y, t.localScale.z };
        if (ImGui::DragFloat3("Scale", scale, 0.01f, 0.001f, 1000.0f))
            t.localScale = { scale[0], scale[1], scale[2] };
    }

    if (auto* sc = go->GetComponent<scene::ScriptComponent>()) {
        if (sc->script) {
            const char* header = sc->script->GetTypeName();
            if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Enabled", &sc->script->enabled);
                ImGui::Separator();
                ImGuiReflector reflector;
                sc->script->Reflect(reflector);
            }
        }
    }

    ImGui::End();
}

} // namespace fbzz::editor
