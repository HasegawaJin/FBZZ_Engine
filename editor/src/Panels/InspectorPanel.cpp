// FBZZ Engine
// InspectorPanel.cpp | fbzz::editor
// 選択 Entity のコンポーネントを表示・編集する
#include <editor/Panels/InspectorPanel.hpp>
#include <editor/EditorContext.hpp>
#include <engine/Scene/Scene.hpp>
#include <engine/Scene/GameObject.hpp>
#include <imgui.h>
#include <cmath>
#include <cstdio>

namespace fbzz::editor {

namespace {

// Quaternion → オイラー角 (度) — XYZ 順
math::Vector3 QuatToEulerDeg(const math::Quaternion& q)
{
    constexpr float DEG = 180.0f / 3.14159265f;

    float sinr = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float roll  = std::atan2(sinr, cosr) * DEG;

    float sinp  = 2.0f * (q.w * q.y - q.z * q.x);
    float pitch = (std::abs(sinp) >= 1.0f)
                ? std::copysign(90.0f, sinp)
                : std::asin(sinp) * DEG;

    float siny = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    float yaw   = std::atan2(siny, cosy) * DEG;

    return { roll, pitch, yaw };
}

} // namespace

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

        math::Vector3 euler = QuatToEulerDeg(t.localRotation);
        float rot[3] = { euler.x, euler.y, euler.z };
        if (ImGui::DragFloat3("Rotation", rot, 0.5f)) {
            constexpr float RAD = 3.14159265f / 180.0f;
            t.localRotation = math::Quaternion::FromEuler(
                { rot[0] * RAD, rot[1] * RAD, rot[2] * RAD });
        }

        float scale[3] = { t.localScale.x, t.localScale.y, t.localScale.z };
        if (ImGui::DragFloat3("Scale", scale, 0.01f, 0.001f, 1000.0f))
            t.localScale = { scale[0], scale[1], scale[2] };
    }

    ImGui::End();
}

} // namespace fbzz::editor
