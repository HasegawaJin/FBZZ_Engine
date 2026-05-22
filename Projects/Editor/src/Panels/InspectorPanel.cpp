// FBZZ Engine
// InspectorPanel.cpp | fbzz::editor
// Selected Entity component inspector and editor
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
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Physics/ColliderVolume.hpp>
#include <imgui.h>
#include <cstdio>

namespace fbzz::editor {

namespace {

template<typename T, typename DrawFn>
void DrawComponentSection(scene::GameObject* go, EditorContext& ctx, const char* label, DrawFn drawFn)
{
    auto* comp = go->GetComponent<T>();
    if (!comp) return;

    ImGui::PushID(label);
    if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enabled", &comp->enabled);
        ImGui::Separator();
        drawFn(*comp, ctx);
    }
    ImGui::PopID();
}

void DrawLightFields(scene::GameObject& go, scene::LightComponent& lc)
{
    static constexpr const char* kTypeNames[] = { "Directional", "Point", "Spot" };
    int typeIdx = static_cast<int>(lc.type);
    if (ImGui::Combo("Type", &typeIdx, kTypeNames, 3))
        lc.type = static_cast<scene::LightComponent::Type>(typeIdx);

    widgets::ColorEdit3("Color", lc.color);
    ImGui::DragFloat("Intensity", &lc.intensity, 0.05f, 0.0f, 200.0f);

    if (lc.type != scene::LightComponent::Type::Directional) {
        float pos[3] = {
            go.transform.localPosition.x,
            go.transform.localPosition.y,
            go.transform.localPosition.z
        };
        if (ImGui::DragFloat3("Position", pos, 0.1f))
            go.transform.localPosition = { pos[0], pos[1], pos[2] };
        ImGui::DragFloat("Range", &lc.range, 0.1f, 0.0f, 500.0f);
    }

    if (lc.type == scene::LightComponent::Type::Spot) {
        ImGui::DragFloat("Inner Cone", &lc.innerCone, 0.5f, 0.0f, 89.0f);
        ImGui::DragFloat("Outer Cone", &lc.outerCone, 0.5f, 0.0f, 89.0f);
    }

    if (lc.type != scene::LightComponent::Type::Point) {
        auto fwd = go.transform.Forward();
        float dir[3] = { fwd.x, fwd.y, fwd.z };
        ImGui::InputFloat3("Forward", dir, "%.3f", ImGuiInputTextFlags_ReadOnly);
    }
}

} // namespace

void InspectorPanel::OnRenderContent(EditorContext& ctx)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go) {
        ImGui::TextDisabled("Nothing selected");
        return;
    }

    char nameBuf[256];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", go->name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        go->name = nameBuf;

    ImGui::Separator();

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

    DrawComponentSection<scene::MeshRenderer>(go, ctx, "Mesh Renderer",
        [](scene::MeshRenderer& mr, EditorContext&) {
            widgets::ReadOnlyText("Mesh", mr.meshPath.c_str());
            widgets::ReadOnlyText("Shader", mr.shaderPath.c_str());

            char albedoBuf[256];
            std::snprintf(albedoBuf, sizeof(albedoBuf), "%s", mr.albedoTexPath.c_str());
            if (ImGui::InputText("Albedo Tex", albedoBuf, sizeof(albedoBuf)))
                mr.albedoTexPath = albedoBuf;

            char normalBuf[256];
            std::snprintf(normalBuf, sizeof(normalBuf), "%s", mr.normalTexPath.c_str());
            if (ImGui::InputText("Normal Tex", normalBuf, sizeof(normalBuf)))
                mr.normalTexPath = normalBuf;

            if (mr.material) {
                auto& p = mr.material->params;
                float col[4] = { p.albedo.x, p.albedo.y, p.albedo.z, p.albedo.w };
                if (ImGui::ColorEdit4("Albedo Color", col)) {
                    p.albedo = { col[0], col[1], col[2], col[3] };
                    mr.material->Upload();
                }
                if (ImGui::SliderFloat("Metallic", &p.metallic, 0.0f, 1.0f)) mr.material->Upload();
                if (ImGui::SliderFloat("Roughness", &p.roughness, 0.0f, 1.0f)) mr.material->Upload();
                if (ImGui::DragFloat("Emissive Scale", &p.emissiveScale, 0.01f, 0.0f, 10.0f)) mr.material->Upload();
            }
        });

    DrawComponentSection<scene::LightComponent>(go, ctx, "Light",
        [go](scene::LightComponent& lc, EditorContext&) {
            DrawLightFields(*go, lc);
        });

    DrawComponentSection<scene::CameraComponent>(go, ctx, "Camera",
        [](scene::CameraComponent& cc, EditorContext&) {
            ImGui::Checkbox("Is Main", &cc.isMain);
            ImGui::DragFloat("FOV", &cc.fovY, 0.5f, 1.0f, 170.0f);
            ImGui::DragFloat("Near", &cc.nearZ, 0.001f, 0.001f, 10.0f);
            ImGui::DragFloat("Far", &cc.farZ, 1.0f, 1.0f, 10000.0f);
        });

    DrawComponentSection<scene::ParticleEmitter>(go, ctx, "Particle Emitter",
        [](scene::ParticleEmitter& pe, EditorContext&) {
            widgets::DragVec3("Emit Position", pe.emitPosition);
            widgets::DragVec3("Emit Velocity", pe.emitVelocity);
            ImGui::DragFloat("Velocity Spread", &pe.velocitySpread, 0.01f, 0.0f, 20.0f);

            float cs[4] = { pe.colorStart.x, pe.colorStart.y, pe.colorStart.z, pe.colorStart.w };
            if (ImGui::ColorEdit4("Color Start", cs))
                pe.colorStart = { cs[0], cs[1], cs[2], cs[3] };
            float ce[4] = { pe.colorEnd.x, pe.colorEnd.y, pe.colorEnd.z, pe.colorEnd.w };
            if (ImGui::ColorEdit4("Color End", ce))
                pe.colorEnd = { ce[0], ce[1], ce[2], ce[3] };

            ImGui::DragFloat("Size Start", &pe.sizeStart, 0.005f, 0.0f, 10.0f);
            ImGui::DragFloat("Size End", &pe.sizeEnd, 0.005f, 0.0f, 10.0f);
            ImGui::DragFloat("Lifetime", &pe.lifetime, 0.05f, 0.1f, 30.0f);
            ImGui::DragFloat("Emit Rate", &pe.emitRate, 1.0f, 0.0f, 1000.0f);
            ImGui::DragInt("Max Particles", &pe.maxParticles, 1, 1, 10000);
        });

    DrawComponentSection<scene::AudioSourceComponent>(go, ctx, "Audio Source",
        [](scene::AudioSourceComponent& asc, EditorContext&) {
            ImGui::Checkbox("Play On Awake", &asc.playOnAwake);
            ImGui::Checkbox("Loop", &asc.loop);
            char clipBuf[512];
            std::snprintf(clipBuf, sizeof(clipBuf), "%s", asc.clipPath.c_str());
            if (ImGui::InputText("Clip Path", clipBuf, sizeof(clipBuf)))
                asc.clipPath = clipBuf;
            ImGui::SliderFloat("Volume", &asc.volume, 0.0f, 1.0f);
        });

    DrawComponentSection<scene::ColliderComponent>(go, ctx, "Collider",
        [](scene::ColliderComponent& col, EditorContext&) {
            ImGui::Checkbox("Is Trigger", &col.isTrigger);
            const char* colliderName = "None";
            if (col.collider) {
                switch (col.collider->GetType()) {
                case physics::ColliderType::SPHERE: colliderName = "Sphere"; break;
                case physics::ColliderType::AABB: colliderName = "AABB"; break;
                case physics::ColliderType::CAPSULE: colliderName = "Capsule"; break;
                }
            }
            widgets::ReadOnlyText("Shape", colliderName);
            ImGui::DragFloat("Restitution", &col.material.restitution, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Static Friction", &col.material.staticFriction, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Dynamic Friction", &col.material.dynamicFriction, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Density", &col.material.density, 0.01f, 0.0f, 100000.0f);
        });

    DrawComponentSection<scene::RigidBodyComponent>(go, ctx, "Rigid Body",
        [](scene::RigidBodyComponent& rb, EditorContext&) {
            if (!rb.rigidBody) {
                ImGui::TextDisabled("No physics::RigidBody assigned");
                return;
            }

            auto& body = *rb.rigidBody;
            bool isStatic = body.IsStatic();
            if (ImGui::Checkbox("Static", &isStatic)) {
                body.m_isStatic = isStatic;
                body.SetMass(body.GetMass());
            }

            float mass = body.GetMass();
            if (ImGui::DragFloat("Mass", &mass, 0.05f, 0.0f, 100000.0f))
                body.SetMass(mass);

            math::Vector3 velocity = body.GetVelocity();
            if (widgets::DragVec3("Velocity", velocity, 0.05f))
                body.SetVelocity(velocity);

            math::Vector3 angularVelocity = body.GetAngularVelocity();
            if (widgets::DragVec3("Angular Velocity", angularVelocity, 0.05f))
                body.SetAngularVelocity(angularVelocity);

            ImGui::DragFloat("Charge", &body.m_charge, 0.01f, -1000.0f, 1000.0f);
            ImGui::Checkbox("Gravity Source", &body.m_isGravitationalSource);
            ImGui::DragFloat("Gravity Mass", &body.m_gravitationalMass, 0.05f, 0.0f, 100000.0f);
        });

    DrawComponentSection<scene::VolumeComponent>(go, ctx, "Volume",
        [](scene::VolumeComponent& volume, EditorContext&) {
            static constexpr const char* kVolumeNames[] = {
                "Gravity", "Vortex", "Buoyancy", "Explosion", "Time Dilation", "Magnetic"
            };
            int typeIdx = static_cast<int>(volume.type);
            if (ImGui::Combo("Type", &typeIdx, kVolumeNames, 6))
                volume.type = static_cast<physics::VolumeType>(typeIdx);

            widgets::DragVec3("Gravity", volume.gravity, 0.05f);
            widgets::DragVec3("Magnetic Field", volume.magneticField, 0.05f);
            ImGui::DragFloat("Swirl", &volume.swirlStrength, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Inward", &volume.inwardStrength, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Lift", &volume.liftStrength, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Buoyancy", &volume.buoyancy, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Drag", &volume.drag, 0.01f, 0.0f, 100.0f);
            ImGui::DragFloat("Explosion Impulse", &volume.explosionImpulse, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Time Scale", &volume.timeScale, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Duration", &volume.duration, 0.05f, -1.0f, 1000.0f);
        });

    DrawComponentSection<scene::SkyRenderer>(go, ctx, "Sky Renderer",
        [](scene::SkyRenderer& sr, EditorContext&) {
            widgets::DragVec3("Rayleigh", sr.rayleighScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Mie Scattering", &sr.mieScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Sun Intensity", &sr.sunIntensity, 0.1f, 0.0f, 1000.0f);
            ImGui::SliderFloat("Mie G", &sr.mieG, -0.99f, 0.99f);
        });

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
}

} // namespace fbzz::editor
