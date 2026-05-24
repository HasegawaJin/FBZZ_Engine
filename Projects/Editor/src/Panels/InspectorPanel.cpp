// FBZZ Engine
// InspectorPanel.cpp | fbzz::editor
// Selected Entity component inspector and editor
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Physics/Layer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/RigidBody.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace fbzz::editor {

namespace {

// コンポーネントヘッダー: [✓] ComponentName        [...]
// ... ボタン → Remove Component
template<typename T, typename DrawFn>
void DrawComponentSection(scene::GameObject* go, EditorContext& ctx, const char* label, DrawFn drawFn)
{
    auto* comp = go->GetComponent<T>();
    if (!comp) return;

    ImGui::PushID(label);

    // Enabled チェックボックス (ヘッダー左)
    ImGui::Checkbox("##en", &comp->enabled);
    ImGui::SameLine();

    // CollapsingHeader: AllowOverlap で右端ボタンとの競合を回避
    bool open = ImGui::CollapsingHeader(label,
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    // [...] ボタンをヘッダー右端に重ねて配置
    const float btnW = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("##comp_opts");

    bool removeRequested = false;
    if (ImGui::BeginPopup("##comp_opts")) {
        if (ImGui::MenuItem("Remove Component"))
            removeRequested = true;
        ImGui::EndPopup();
    }

    if (open) {
        ImGui::Spacing();
        drawFn(*comp, ctx);
        ImGui::Spacing();
    }

    ImGui::PopID();

    if (removeRequested)
        go->RemoveComponent<T>();
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

scene::MeshRenderer CreateDefaultMeshRenderer()
{
    scene::MeshRenderer mr;
    mr.meshPath = "primitive:cube";
    if (auto* resources = renderer::ResourceManager::Active())
        mr.mesh = renderer::PrimitiveMesh::Cube(*resources);
    return mr;
}

scene::MaterialComponent CreateDefaultMaterialComponent()
{
    scene::MaterialComponent mc;
    mc.shaderPath = "Assets/shaders/Material/Phong.hlsl";
    auto material = std::make_shared<renderer::Material>();
    material->shaderPath = mc.shaderPath;
    if (auto* resources = renderer::ResourceManager::Active()) {
        material->shader = resources->LoadShader(mc.shaderPath);
        material->params.roughness = 0.65f;
        material->Init(*resources);
    }
    mc.material = std::move(material);
    return mc;
}

scene::ColliderComponent CreateAabbCollider(const math::Vector3& halfExtents = { 0.5f, 0.5f, 0.5f })
{
    scene::ColliderComponent collider;
    collider.collider = std::make_shared<physics::AABBCollider>(halfExtents);
    return collider;
}

scene::RigidBodyComponent CreateDefaultRigidBody()
{
    scene::RigidBodyComponent rb;
    rb.rigidBody = std::make_shared<physics::RigidBody>();
    rb.rigidBody->SetMass(1.0f);
    return rb;
}

template<typename T>
void DrawAddComponentItem(scene::GameObject& go, const char* label)
{
    const bool hasComponent = go.GetComponent<T>() != nullptr;
    if (ImGui::MenuItem(label, nullptr, false, !hasComponent))
        go.AddComponent<T>();
}

void DrawAddComponentMenu(scene::GameObject& go)
{
    if (ImGui::Button("Add Component", { -1.0f, 0.0f }))
        ImGui::OpenPopup("##add_component");

    if (!ImGui::BeginPopup("##add_component")) return;

    const bool hasMeshRenderer = go.GetComponent<scene::MeshRenderer>() != nullptr;
    if (ImGui::MenuItem("Mesh Renderer", nullptr, false, !hasMeshRenderer)) {
        go.AddComponent<scene::MeshRenderer>(CreateDefaultMeshRenderer());
        if (!go.GetComponent<scene::MaterialComponent>())
            go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
    }

    const bool hasMaterial = go.GetComponent<scene::MaterialComponent>() != nullptr;
    if (ImGui::MenuItem("Material", nullptr, false, !hasMaterial))
        go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());

    DrawAddComponentItem<scene::LightComponent>(go, "Light");
    DrawAddComponentItem<scene::CameraComponent>(go, "Camera");
    DrawAddComponentItem<scene::ParticleEmitter>(go, "Particle Emitter");
    DrawAddComponentItem<scene::AudioSourceComponent>(go, "Audio Source");

    const bool hasCollider = go.GetComponent<scene::ColliderComponent>() != nullptr;
    if (ImGui::MenuItem("Collider", nullptr, false, !hasCollider))
        go.AddComponent<scene::ColliderComponent>(CreateAabbCollider());

    const bool hasRigidBody = go.GetComponent<scene::RigidBodyComponent>() != nullptr;
    if (ImGui::MenuItem("Rigid Body", nullptr, false, !hasRigidBody))
        go.AddComponent<scene::RigidBodyComponent>(CreateDefaultRigidBody());

    DrawAddComponentItem<scene::VolumeComponent>(go, "Volume");
    DrawAddComponentItem<scene::SkyRenderer>(go, "Sky Renderer");
    {
        const bool hasSMR = go.GetComponent<scene::SkinnedMeshRenderer>() != nullptr;
        if (ImGui::MenuItem("Skinned Mesh Renderer", nullptr, false, !hasSMR)) {
            go.AddComponent<scene::SkinnedMeshRenderer>();
            if (!go.GetComponent<scene::MaterialComponent>())
                go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
        }
    }
    {
        const bool hasAnimator = go.GetComponent<scene::AnimatorComponent>() != nullptr;
        if (ImGui::MenuItem("Animator", nullptr, false, !hasAnimator)) {
            go.AddComponent<scene::AnimatorComponent>();
            if (!go.GetComponent<scene::SkinnedMeshRenderer>())
                go.AddComponent<scene::SkinnedMeshRenderer>();
            if (!go.GetComponent<scene::MaterialComponent>())
                go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
        }
    }
    DrawAddComponentItem<scene::UICanvas>(go, "UI Canvas");
    DrawAddComponentItem<scene::UIImage>(go, "UI Image");
    DrawAddComponentItem<scene::UIButton>(go, "UI Button");
    DrawAddComponentItem<scene::UIText>(go, "UI Text");
    DrawAddComponentItem<scene::UILayoutGroup>(go, "UI Layout Group");
    DrawAddComponentItem<scene::UIAnimator>(go, "UI Animator");

    ImGui::EndPopup();
}

bool DragVec2(const char* label, math::Vector2& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f)
{
    float data[2] = { value.x, value.y };
    if (!ImGui::DragFloat2(label, data, speed, min, max)) return false;
    value = { data[0], data[1] };
    return true;
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

    auto& ps = ctx.projectSettings;

    {
        const float spacing   = ImGui::GetStyle().ItemSpacing.x;
        const float labelTagW = ImGui::CalcTextSize("Tag").x   + spacing;
        const float labelLayW = ImGui::CalcTextSize("Layer").x + spacing;
        const float comboW    = (ImGui::GetContentRegionAvail().x - labelTagW - labelLayW - spacing) * 0.5f;

        // Tag
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Tag");
        ImGui::SameLine();
        int tagIdx = 0;
        for (int i = 0; i < (int)ps.tags.size(); ++i)
            if (go->tag == ps.tags[i]) { tagIdx = i; break; }
        const char* tagLabel = ps.tags.empty() ? "(none)" : ps.tags[tagIdx].c_str();
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##tag", tagLabel)) {
            for (int i = 0; i < (int)ps.tags.size(); ++i) {
                bool selected = (i == tagIdx);
                if (ImGui::Selectable(ps.tags[i].c_str(), selected))
                    go->tag = ps.tags[i];
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Add Tag..."))
                ctx.requestOpenProjectSettings = true;
            ImGui::EndCombo();
        }

        // Layer
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Layer");
        ImGui::SameLine();
        int layerIdx = go->layer & 31;
        auto layerGetter = [](void* data, int idx) -> const char* {
            auto* names = static_cast<std::array<std::string, 32>*>(data);
            if (idx < 0 || idx >= 32) return "";
            return (*names)[idx].c_str();
        };
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##layer", &layerIdx, layerGetter, &ps.layerNames, 32))
            go->layer = layerIdx;
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& t = go->transform;
        ImGui::Spacing();

        const bool isUI = go->GetComponent<scene::UIImage>() || go->GetComponent<scene::UIText>();

        if (isUI) {
            // X / Y を横並びで表示
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

            // Rotation Z のみ
            math::Vector3 euler = widgets::QuatToEulerDeg(t.localRotation);
            float rotZ = euler.z;
            ImGui::Text("Rot");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##rz", &rotZ, 0.5f, -360.0f, 360.0f, "Z %.1f deg"))
                t.localRotation = widgets::EulerDegToQuat({ euler.x, euler.y, rotZ });

            // W / H (UIImage のみ)
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

            math::Vector3 euler = widgets::QuatToEulerDeg(t.localRotation);
            float rot[3] = { euler.x, euler.y, euler.z };
            if (ImGui::DragFloat3("Rotation", rot, 0.5f))
                t.localRotation = widgets::EulerDegToQuat({ rot[0], rot[1], rot[2] });

            float scale[3] = { t.localScale.x, t.localScale.y, t.localScale.z };
            if (ImGui::DragFloat3("Scale", scale, 0.01f, 0.001f, 1000.0f))
                t.localScale = { scale[0], scale[1], scale[2] };
        }

        ImGui::Spacing();
    }

    DrawComponentSection<scene::MeshRenderer>(go, ctx, "Mesh Renderer",
        [](scene::MeshRenderer& mr, EditorContext&) {
            static constexpr const char* kPrimitiveNames[] = {
                "Custom", "Cube", "Sphere", "Plane", "Cylinder", "Cone", "Torus", "Capsule"
            };
            static constexpr const char* kPrimitivePaths[] = {
                "", "primitive:cube", "primitive:sphere", "primitive:plane",
                "primitive:cylinder", "primitive:cone", "primitive:torus", "primitive:capsule"
            };
            constexpr int kPrimitiveCount = 8;

            int sel = 0;
            for (int i = 1; i < kPrimitiveCount; ++i)
                if (mr.meshPath == kPrimitivePaths[i]) { sel = i; break; }

            if (ImGui::Combo("Mesh", &sel, kPrimitiveNames, kPrimitiveCount)) {
                if (sel > 0) {
                    mr.meshPath = kPrimitivePaths[sel];
                    if (auto* res = renderer::ResourceManager::Active()) {
                        switch (sel) {
                        case 1: mr.mesh = renderer::PrimitiveMesh::Cube(*res);         break;
                        case 2: mr.mesh = renderer::PrimitiveMesh::Sphere(*res);       break;
                        case 3: mr.mesh = renderer::PrimitiveMesh::Plane(*res);        break;
                        case 4: mr.mesh = renderer::PrimitiveMesh::Cylinder(*res);     break;
                        case 5: mr.mesh = renderer::PrimitiveMesh::Cone(*res);         break;
                        case 6: mr.mesh = renderer::PrimitiveMesh::Torus(*res);        break;
                        case 7: mr.mesh = renderer::PrimitiveMesh::Capsule(*res);      break;
                        default: break;
                        }
                    }
                } else {
                    mr.meshPath.clear();
                    mr.mesh.reset();
                }
            }

            if (sel == 0) {
                char meshBuf[256];
                std::snprintf(meshBuf, sizeof(meshBuf), "%s", mr.meshPath.c_str());
                if (ImGui::InputText("Mesh Path", meshBuf, sizeof(meshBuf)))
                    mr.meshPath = meshBuf;

                auto loadCustomMesh = [&mr]() {
                    if (mr.meshPath.empty()) return;
                    if (auto model = asset::AssetManager::Load<asset::Model>(mr.meshPath)) {
                        if (!model->meshes.empty())
                            mr.mesh = model->meshes[0];
                    }
                };
                if (ImGui::IsItemDeactivatedAfterEdit()) loadCustomMesh();

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        mr.meshPath = static_cast<const char*>(p->Data);
                        for (char& c : mr.meshPath) if (c == '\\') c = '/';
                        loadCustomMesh();
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        });

    DrawComponentSection<scene::SkinnedMeshRenderer>(go, ctx, "Skinned Mesh Renderer",
        [](scene::SkinnedMeshRenderer& smr, EditorContext& ctx) {
            auto loadModel = [&smr]() {
                smr.model.reset();
                if (smr.modelPath.empty()) return;
                smr.model = asset::AssetManager::Load<asset::Model>(smr.modelPath);
            };

            char pathBuf[256];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", smr.modelPath.c_str());
            if (ImGui::InputText("Model", pathBuf, sizeof(pathBuf)))
                smr.modelPath = pathBuf;
            if (ImGui::IsItemDeactivatedAfterEdit()) loadModel();
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    smr.modelPath = static_cast<const char*>(p->Data);
                    for (char& c : smr.modelPath) if (c == '\\') c = '/';
                    loadModel();
                }
                ImGui::EndDragDropTarget();
            }

            if (smr.model) {
                const int meshCount = static_cast<int>(smr.model->meshes.size());
                ImGui::DragInt("Mesh Index", &smr.meshIndex, 1.0f, 0, std::max(0, meshCount - 1));
                ImGui::TextDisabled("%d mesh(es) | %s skeleton",
                    meshCount, smr.model->skeleton ? "has" : "no");

                if (!ctx.GetSelectedGO()->GetComponent<scene::MaterialComponent>()) {
                    ImGui::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, "! Material component required");
                    if (ImGui::Button("Add Material")) {
                        if (auto* go2 = ctx.GetSelectedGO())
                            go2->AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
                    }
                }
            }
        });

    DrawComponentSection<scene::AnimatorComponent>(go, ctx, "Animator",
        [](scene::AnimatorComponent& anim, EditorContext&) {
            // --- Clip Sources list ---
            ImGui::Text("Clip Sources");

            for (int i = 0; i < static_cast<int>(anim.clipSources.size()); ++i) {
                ImGui::PushID(i);
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", anim.clipSources[i].c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 24.0f);
                if (ImGui::InputText("##src", buf, sizeof(buf)))
                    anim.clipSources[i] = buf;
                if (ImGui::IsItemDeactivatedAfterEdit())
                    { anim.clips.clear(); anim.clipsLoaded = false; }
                // DragDrop target must be right after InputText, before SameLine
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        anim.clipSources[i] = static_cast<const char*>(p->Data);
                        for (char& c : anim.clipSources[i]) if (c == '\\') c = '/';
                        anim.clips.clear(); anim.clipsLoaded = false;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    anim.clipSources.erase(anim.clipSources.begin() + i);
                    anim.clips.clear(); anim.clipsLoaded = false;
                    ImGui::PopID(); break;
                }
                ImGui::PopID();
            }
            // "+ Add Source" also acts as drop zone: drag FBX directly onto it
            if (ImGui::Button("+ Add Source  (or drop FBX)", { -1.0f, 0.0f }))
                anim.clipSources.emplace_back();
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string path = static_cast<const char*>(p->Data);
                    for (char& c : path) if (c == '\\') c = '/';
                    anim.clipSources.push_back(std::move(path));
                    anim.clips.clear(); anim.clipsLoaded = false;
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::Separator();

            // --- Clip selector ---
            if (!anim.clips.empty()) {
                const int clipCount = static_cast<int>(anim.clips.size());
                int sel = std::clamp(anim.clipIndex, 0, clipCount - 1);
                std::vector<const char*> names;
                names.reserve(static_cast<size_t>(clipCount));
                for (const auto& c : anim.clips) names.push_back(c.name.c_str());
                if (ImGui::Combo("Clip", &sel, names.data(), clipCount)) {
                    anim.clipIndex = sel;
                    anim.clipName  = anim.clips[static_cast<size_t>(sel)].name;
                    anim.time = 0.0f;
                }
                const auto& cur = anim.clips[static_cast<size_t>(sel)];
                const double tps = cur.ticksPerSecond > 0.0 ? cur.ticksPerSecond : 30.0;
                const float dur  = static_cast<float>(cur.durationTicks / tps);
                const float t    = (dur > 0.0f) ? std::clamp(anim.time / dur, 0.0f, 1.0f) : 0.0f;
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%.2f / %.2fs", anim.time, dur);
                ImGui::ProgressBar(t, { -1.0f, 0.0f }, overlay);
                ImGui::TextDisabled("%d clip(s) | %d tracks", clipCount,
                                    static_cast<int>(cur.tracks.size()));
            } else if (anim.clipsLoaded) {
                ImGui::TextDisabled("No clips loaded");
            } else {
                ImGui::TextDisabled("(clips not loaded yet)");
            }

            ImGui::DragFloat("Time",  &anim.time,  0.01f, 0.0f, 100000.0f);
            ImGui::DragFloat("Speed", &anim.speed, 0.01f, -10.0f, 10.0f);
            ImGui::Checkbox("Loop",    &anim.loop);
            ImGui::Checkbox("Playing", &anim.playing);
        });

    DrawComponentSection<scene::MaterialComponent>(go, ctx, "Material",
        [](scene::MaterialComponent& mc, EditorContext&) {
            auto applyShader = [&mc]() {
                if (mc.material)
                    if (auto* res = renderer::ResourceManager::Active())
                        mc.material->shader = res->LoadShader(mc.shaderPath);
            };
            auto applyAlbedo = [&mc]() {
                if (mc.material)
                    if (auto* res = renderer::ResourceManager::Active()) {
                        mc.material->albedoTexture = mc.albedoTexPath.empty()
                            ? renderer::ResourceHandle<renderer::TextureTag>{}
                            : res->LoadTexture(mc.albedoTexPath);
                        mc.material->Upload(*res);
                    }
            };
            auto applyNormal = [&mc]() {
                if (mc.material)
                    if (auto* res = renderer::ResourceManager::Active()) {
                        mc.material->normalTexture = mc.normalTexPath.empty()
                            ? renderer::ResourceHandle<renderer::TextureTag>{}
                            : res->LoadTexture(mc.normalTexPath);
                        mc.material->Upload(*res);
                    }
            };

            // Shader
            char shaderBuf[256];
            std::snprintf(shaderBuf, sizeof(shaderBuf), "%s", mc.shaderPath.c_str());
            if (ImGui::InputText("Shader", shaderBuf, sizeof(shaderBuf)))
                mc.shaderPath = shaderBuf;
            if (ImGui::IsItemDeactivatedAfterEdit()) applyShader();
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    mc.shaderPath = static_cast<const char*>(p->Data);
                    for (char& c : mc.shaderPath) if (c == '\\') c = '/';
                    applyShader();
                }
                ImGui::EndDragDropTarget();
            }

            // Albedo Tex
            char albedoBuf[256];
            std::snprintf(albedoBuf, sizeof(albedoBuf), "%s", mc.albedoTexPath.c_str());
            if (ImGui::InputText("Albedo Tex", albedoBuf, sizeof(albedoBuf)))
                mc.albedoTexPath = albedoBuf;
            if (ImGui::IsItemDeactivatedAfterEdit()) applyAlbedo();
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    mc.albedoTexPath = static_cast<const char*>(p->Data);
                    for (char& c : mc.albedoTexPath) if (c == '\\') c = '/';
                    applyAlbedo();
                }
                ImGui::EndDragDropTarget();
            }

            // Normal Tex
            char normalBuf[256];
            std::snprintf(normalBuf, sizeof(normalBuf), "%s", mc.normalTexPath.c_str());
            if (ImGui::InputText("Normal Tex", normalBuf, sizeof(normalBuf)))
                mc.normalTexPath = normalBuf;
            if (ImGui::IsItemDeactivatedAfterEdit()) applyNormal();
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    mc.normalTexPath = static_cast<const char*>(p->Data);
                    for (char& c : mc.normalTexPath) if (c == '\\') c = '/';
                    applyNormal();
                }
                ImGui::EndDragDropTarget();
            }

            if (mc.material) {
                auto& p = mc.material->params;
                auto uploadMaterial = [&mc]() {
                    if (auto* resources = renderer::ResourceManager::Active())
                        mc.material->Upload(*resources);
                };
                float col[4] = { p.albedo.x, p.albedo.y, p.albedo.z, p.albedo.w };
                if (ImGui::ColorEdit4("Albedo Color", col)) {
                    p.albedo = { col[0], col[1], col[2], col[3] };
                    uploadMaterial();
                }
                if (ImGui::SliderFloat("Metallic",      &p.metallic,      0.0f, 1.0f))  uploadMaterial();
                if (ImGui::SliderFloat("Roughness",     &p.roughness,     0.0f, 1.0f))  uploadMaterial();
                if (ImGui::DragFloat("Emissive Scale",  &p.emissiveScale, 0.01f, 0.0f, 10.0f)) uploadMaterial();
            }
        });

    DrawComponentSection<scene::LightComponent>(go, ctx, "Light",
        [go](scene::LightComponent& lc, EditorContext&) {
            DrawLightFields(*go, lc);
        });

    DrawComponentSection<scene::CameraComponent>(go, ctx, "Camera",
        [](scene::CameraComponent& cc, EditorContext& c) {
            ImGui::Checkbox("Is Main", &cc.isMain);
            ImGui::DragFloat("FOV", &cc.fovY, 0.5f, 1.0f, 170.0f);
            ImGui::DragFloat("Near", &cc.nearZ, 0.001f, 0.001f, 10.0f);
            ImGui::DragFloat("Far", &cc.farZ, 1.0f, 1.0f, 10000.0f);
            const char* maskLabel = cc.cullingMask == fbzz::Layer::Everything ? "Everything"
                                  : cc.cullingMask == fbzz::Layer::Nothing    ? "Nothing"
                                  : "Mixed...";
            if (ImGui::BeginCombo("Culling Mask", maskLabel)) {
                bool all = cc.cullingMask == fbzz::Layer::Everything;
                if (ImGui::Checkbox("Everything", &all))
                    cc.cullingMask = all ? fbzz::Layer::Everything : fbzz::Layer::Nothing;
                ImGui::Separator();
                for (int i = 0; i < 32; ++i) {
                    bool on = fbzz::Layer::Contains(cc.cullingMask, i);
                    if (ImGui::Checkbox(c.projectSettings.layerNames[i].c_str(), &on)) {
                        if (on) cc.cullingMask |=  fbzz::Layer::Mask(i);
                        else    cc.cullingMask &= ~fbzz::Layer::Mask(i);
                    }
                }
                ImGui::EndCombo();
            }
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

    DrawComponentSection<scene::UICanvas>(go, ctx, "UI Canvas",
        [go](scene::UICanvas& canvas, EditorContext&) {
            if (go->GetParent())
                ImGui::TextDisabled("Only root GameObjects are rendered as canvases.");
            ImGui::DragFloat("Canvas Width",  &canvas.canvasWidth,  1.0f, 1.0f, 16384.0f);
            ImGui::DragFloat("Canvas Height", &canvas.canvasHeight, 1.0f, 1.0f, 16384.0f);
            ImGui::DragInt("Sort Order", &canvas.sortOrder);
            static constexpr const char* kModeNames[] = { "Screen Space", "World Space" };
            int modeIdx = static_cast<int>(canvas.renderMode);
            if (ImGui::Combo("Render Mode", &modeIdx, kModeNames, 2))
                canvas.renderMode = static_cast<scene::UIRenderMode>(modeIdx);
            if (canvas.renderMode == scene::UIRenderMode::WorldSpace)
                ImGui::DragFloat("World Scale", &canvas.worldScale, 0.0001f, 0.00001f, 1.0f, "%.5f");
        });

    DrawComponentSection<scene::UIImage>(go, ctx, "UI Image",
        [](scene::UIImage& image, EditorContext&) {
            // 位置・サイズは Transform で管理 (上の Transform セクションを参照)
            float color[4] = { image.color.x, image.color.y, image.color.z, image.color.w };
            if (ImGui::ColorEdit4("Color", color))
                image.color = { color[0], color[1], color[2], color[3] };
            DragVec2("UV Min", image.uvMin, 0.01f, 0.0f, 1.0f);
            DragVec2("UV Max", image.uvMax, 0.01f, 0.0f, 1.0f);
            char texBuf[512];
            std::snprintf(texBuf, sizeof(texBuf), "%s", image.texturePath.c_str());
            if (ImGui::InputText("Texture Path", texBuf, sizeof(texBuf)))
                image.texturePath = texBuf;
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    image.texturePath = static_cast<const char*>(p->Data);
                    for (char& c : image.texturePath) if (c == '\\') c = '/';
                }
                ImGui::EndDragDropTarget();
            }
        });

    DrawComponentSection<scene::UIButton>(go, ctx, "UI Button",
        [](scene::UIButton& button, EditorContext&) {
            ImGui::Checkbox("Interactable", &button.isInteractable);
            float normal[4] = { button.normalColor.x, button.normalColor.y, button.normalColor.z, button.normalColor.w };
            if (ImGui::ColorEdit4("Normal Color", normal))
                button.normalColor = { normal[0], normal[1], normal[2], normal[3] };
            float hover[4] = { button.hoverColor.x, button.hoverColor.y, button.hoverColor.z, button.hoverColor.w };
            if (ImGui::ColorEdit4("Hover Color", hover))
                button.hoverColor = { hover[0], hover[1], hover[2], hover[3] };
            float pressed[4] = { button.pressedColor.x, button.pressedColor.y, button.pressedColor.z, button.pressedColor.w };
            if (ImGui::ColorEdit4("Pressed Color", pressed))
                button.pressedColor = { pressed[0], pressed[1], pressed[2], pressed[3] };
            static constexpr const char* kStateNames[] = { "Normal", "Hovered", "Pressed" };
            widgets::ReadOnlyText("State", kStateNames[static_cast<int>(button.state)]);
        });

    DrawComponentSection<scene::UIText>(go, ctx, "UI Text",
        [](scene::UIText& text, EditorContext&) {
            // 位置は Transform で管理
            char textBuf[512];
            std::snprintf(textBuf, sizeof(textBuf), "%s", text.text.c_str());
            if (ImGui::InputText("Text", textBuf, sizeof(textBuf)))
                text.text = textBuf;
            ImGui::DragFloat("Font Size",      &text.fontSize,      1.0f, 1.0f, 512.0f);
            ImGui::DragFloat("Letter Spacing", &text.letterSpacing, 0.1f, 0.0f, 128.0f);
            float color[4] = { text.color.x, text.color.y, text.color.z, text.color.w };
            if (ImGui::ColorEdit4("Color", color))
                text.color = { color[0], color[1], color[2], color[3] };
        });

    DrawComponentSection<scene::UILayoutGroup>(go, ctx, "UI Layout Group",
        [](scene::UILayoutGroup& layout, EditorContext&) {
            static constexpr const char* kAxisNames[] = { "Horizontal", "Vertical" };
            int axisIdx = static_cast<int>(layout.axis);
            if (ImGui::Combo("Axis", &axisIdx, kAxisNames, 2))
                layout.axis = static_cast<scene::UILayoutAxis>(axisIdx);
            ImGui::DragFloat("Spacing", &layout.spacing, 1.0f, 0.0f, 1024.0f);
            ImGui::DragFloat("Pad Left",   &layout.paddingLeft,   1.0f, 0.0f, 512.0f);
            ImGui::DragFloat("Pad Right",  &layout.paddingRight,  1.0f, 0.0f, 512.0f);
            ImGui::DragFloat("Pad Top",    &layout.paddingTop,    1.0f, 0.0f, 512.0f);
            ImGui::DragFloat("Pad Bottom", &layout.paddingBottom, 1.0f, 0.0f, 512.0f);
            ImGui::Checkbox("Reverse Order", &layout.reverseOrder);
        });

    DrawComponentSection<scene::UIAnimator>(go, ctx, "UI Animator",
        [](scene::UIAnimator& anim, EditorContext&) {
            if (ImGui::TreeNodeEx("Color Tween", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Active##ct", &anim.colorTween.active);
                float from[4] = { anim.colorTween.from.x, anim.colorTween.from.y, anim.colorTween.from.z, anim.colorTween.from.w };
                if (ImGui::ColorEdit4("From##ct", from)) anim.colorTween.from = { from[0], from[1], from[2], from[3] };
                float to[4] = { anim.colorTween.to.x, anim.colorTween.to.y, anim.colorTween.to.z, anim.colorTween.to.w };
                if (ImGui::ColorEdit4("To##ct", to)) anim.colorTween.to = { to[0], to[1], to[2], to[3] };
                ImGui::DragFloat("Duration##ct", &anim.colorTween.duration, 0.05f, 0.01f, 60.0f);
                ImGui::Checkbox("Loop##ct",     &anim.colorTween.loop);
                ImGui::Checkbox("Ping Pong##ct",&anim.colorTween.pingPong);
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Position Tween", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Active##pt", &anim.positionTween.active);
                DragVec2("From##pt", anim.positionTween.from, 1.0f);
                DragVec2("To##pt",   anim.positionTween.to,   1.0f);
                ImGui::DragFloat("Duration##pt", &anim.positionTween.duration, 0.05f, 0.01f, 60.0f);
                ImGui::Checkbox("Loop##pt",     &anim.positionTween.loop);
                ImGui::Checkbox("Ping Pong##pt",&anim.positionTween.pingPong);
                ImGui::TreePop();
            }
        });

    if (auto* sc = go->GetComponent<scene::ScriptComponent>()) {
        if (sc->script) {
            const char* header = sc->script->GetTypeName();
            ImGui::PushID("ScriptComponent");

            ImGui::Checkbox("##en", &sc->script->enabled);
            ImGui::SameLine();

            bool open = ImGui::CollapsingHeader(header,
                ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

            const float btnW = ImGui::GetFrameHeight();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW);
            if (ImGui::SmallButton("..."))
                ImGui::OpenPopup("##script_opts");

            bool removeScript = false;
            if (ImGui::BeginPopup("##script_opts")) {
                if (ImGui::MenuItem("Remove Component"))
                    removeScript = true;
                ImGui::EndPopup();
            }

            if (open) {
                ImGui::Spacing();
                ImGuiReflector reflector;
                sc->script->Reflect(reflector);
                ImGui::Spacing();
            }

            ImGui::PopID();

            if (removeScript)
                go->RemoveComponent<scene::ScriptComponent>();
        }
    }

    ImGui::Spacing();
    DrawAddComponentMenu(*go);
}

} // namespace fbzz::editor
