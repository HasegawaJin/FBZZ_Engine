// FBZZ Engine
// InspectorPanel.cpp | fbzz::editor
// 選択 Entity のコンポーネントを表示・編集する
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Renderer/Material.hpp>
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

    // MeshRenderer
    if (auto* mr = go->GetComponent<scene::MeshRenderer>()) {
        if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##mr", &mr->enabled);
            ImGui::Separator();
            widgets::ReadOnlyText("Mesh",   mr->meshPath.c_str());
            widgets::ReadOnlyText("Shader", mr->shaderPath.c_str());

            char albedoBuf[256]; std::snprintf(albedoBuf, sizeof(albedoBuf), "%s", mr->albedoTexPath.c_str());
            if (ImGui::InputText("Albedo Tex", albedoBuf, sizeof(albedoBuf))) mr->albedoTexPath = albedoBuf;

            char normalBuf[256]; std::snprintf(normalBuf, sizeof(normalBuf), "%s", mr->normalTexPath.c_str());
            if (ImGui::InputText("Normal Tex", normalBuf, sizeof(normalBuf))) mr->normalTexPath = normalBuf;

            if (mr->material) {
                auto& p = mr->material->params;
                float col[4] = { p.albedo.x, p.albedo.y, p.albedo.z, p.albedo.w };
                if (ImGui::ColorEdit4("Albedo Color", col)) {
                    p.albedo = { col[0], col[1], col[2], col[3] };
                    mr->material->Upload();
                }
                if (ImGui::SliderFloat("Metallic",       &p.metallic,      0.0f, 1.0f)) mr->material->Upload();
                if (ImGui::SliderFloat("Roughness",      &p.roughness,     0.0f, 1.0f)) mr->material->Upload();
                if (ImGui::DragFloat ("Emissive Scale", &p.emissiveScale, 0.01f, 0.0f, 10.0f)) mr->material->Upload();
            }
        }
    }

    // LightComponent
    if (auto* lc = go->GetComponent<scene::LightComponent>()) {
        if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##lc", &lc->enabled);
            ImGui::Separator();

            static constexpr const char* kTypeNames[] = { "Directional", "Point", "Spot" };
            int typeIdx = static_cast<int>(lc->type);
            if (ImGui::Combo("Type", &typeIdx, kTypeNames, 3))
                lc->type = static_cast<scene::LightComponent::Type>(typeIdx);

            widgets::ColorEdit3("Color", lc->color);
            ImGui::DragFloat("Intensity", &lc->intensity, 0.05f, 0.0f, 100.0f);
            if (lc->type != scene::LightComponent::Type::Directional)
                ImGui::DragFloat("Range", &lc->range, 0.1f, 0.0f, 500.0f);
            if (lc->type == scene::LightComponent::Type::Spot) {
                ImGui::DragFloat("Inner Cone", &lc->innerCone, 0.5f, 0.0f, 89.0f);
                ImGui::DragFloat("Outer Cone", &lc->outerCone, 0.5f, 0.0f, 89.0f);
            }
        }
    }

    // CameraComponent
    if (auto* cc = go->GetComponent<scene::CameraComponent>()) {
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##cc", &cc->enabled);
            ImGui::Checkbox("Is Main",     &cc->isMain);
            ImGui::Separator();
            ImGui::DragFloat("FOV",  &cc->fovY,  0.5f,  1.0f, 170.0f);
            ImGui::DragFloat("Near", &cc->nearZ, 0.001f, 0.001f, 10.0f);
            ImGui::DragFloat("Far",  &cc->farZ,  1.0f,  1.0f, 10000.0f);
        }
    }

    // ParticleEmitter
    if (auto* pe = go->GetComponent<scene::ParticleEmitter>()) {
        if (ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##pe", &pe->enabled);
            ImGui::Separator();
            widgets::DragVec3("Emit Position", pe->emitPosition);
            widgets::DragVec3("Emit Velocity", pe->emitVelocity);
            ImGui::DragFloat("Velocity Spread", &pe->velocitySpread, 0.01f, 0.0f, 20.0f);

            float cs[4] = { pe->colorStart.x, pe->colorStart.y, pe->colorStart.z, pe->colorStart.w };
            if (ImGui::ColorEdit4("Color Start", cs))
                pe->colorStart = { cs[0], cs[1], cs[2], cs[3] };
            float ce[4] = { pe->colorEnd.x, pe->colorEnd.y, pe->colorEnd.z, pe->colorEnd.w };
            if (ImGui::ColorEdit4("Color End", ce))
                pe->colorEnd = { ce[0], ce[1], ce[2], ce[3] };

            ImGui::DragFloat("Size Start",  &pe->sizeStart,  0.005f, 0.0f, 10.0f);
            ImGui::DragFloat("Size End",    &pe->sizeEnd,    0.005f, 0.0f, 10.0f);
            ImGui::DragFloat("Lifetime",    &pe->lifetime,   0.05f,  0.1f, 30.0f);
            ImGui::DragFloat("Emit Rate",   &pe->emitRate,   1.0f,   0.0f, 1000.0f);
            ImGui::DragInt  ("Max Particles", &pe->maxParticles, 1, 1, 10000);
        }
    }

    // AudioSourceComponent
    if (auto* asc = go->GetComponent<scene::AudioSourceComponent>()) {
        if (ImGui::CollapsingHeader("Audio Source", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled##asc",      &asc->enabled);
            ImGui::Checkbox("Play On Awake",     &asc->playOnAwake);
            ImGui::Checkbox("Loop",              &asc->loop);
            ImGui::Separator();
            char clipBuf[512]; std::snprintf(clipBuf, sizeof(clipBuf), "%s", asc->clipPath.c_str());
            if (ImGui::InputText("Clip Path", clipBuf, sizeof(clipBuf))) asc->clipPath = clipBuf;
            ImGui::SliderFloat("Volume", &asc->volume, 0.0f, 1.0f);
        }
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
