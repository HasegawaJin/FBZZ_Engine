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
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <imgui.h>
#include <algorithm>
#include <any>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace fbzz::editor {

namespace {


template<typename T, typename DrawFn>
void DrawComponentSection(scene::GameObject* go,
                          EditorContext& ctx,
                          std::any& compClipboard,
                          const std::type_info*& compClipboardType,
                          const char* label,
                          DrawFn drawFn)
{
    auto* comp = go->GetComponent<T>();
    if (!comp) return;

    ImGui::PushID(label);

    ImGui::Checkbox("##en", &comp->enabled);
    ImGui::SameLine();


    bool open = ImGui::CollapsingHeader(label,
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    const float btnW = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - btnW);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("##comp_opts");

    bool removeRequested = false;
    if (ImGui::BeginPopup("##comp_opts")) {
        if (ImGui::MenuItem("Reset")) {
            const bool wasEnabled = comp->enabled;
            *comp = T{};
            comp->enabled = wasEnabled;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Component"))
        {
            compClipboard     = *comp;
            compClipboardType = &typeid(T);
        }
        const bool canPaste = compClipboardType && *compClipboardType == typeid(T);
        if (ImGui::MenuItem("Paste Component Values", nullptr, false, canPaste))
        {
            const bool wasEnabled = comp->enabled;
            *comp = std::any_cast<T>(compClipboard);
            comp->enabled = wasEnabled;
        }
        ImGui::Separator();
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
    mc.shaderPath = "Assets/shaders/Material/Surface/Phong.hlsl";
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

renderer::Material& EnsureMaterial(scene::MaterialComponent& mc)
{
    if (!mc.material)
        mc.material = std::make_shared<renderer::Material>();
    else if (mc.material.use_count() > 1) {
        auto cloned = std::make_shared<renderer::Material>(*mc.material);
        cloned->paramsBuffer = renderer::ResourceHandle<renderer::ConstantBufferTag>{};
        mc.material = std::move(cloned);
    }
    return *mc.material;
}

scene::AabbColliderComponent CreateAabbCollider(const math::Vector3& size = math::Vector3::ONE)
{
    scene::AabbColliderComponent collider;
    collider.size = size;
    collider.collider = std::make_shared<physics::AABBCollider>(size * 0.5f);
    return collider;
}

scene::BoxColliderComponent CreateBoxCollider(const math::Vector3& halfExtents = { 0.5f, 0.5f, 0.5f })
{
    scene::BoxColliderComponent collider;
    collider.size = halfExtents * 2.0f;
    collider.collider = std::make_shared<physics::OBBCollider>(halfExtents);
    return collider;
}

scene::SphereColliderComponent CreateSphereCollider(float radius = 0.5f)
{
    scene::SphereColliderComponent collider;
    collider.radius = radius;
    collider.collider = std::make_shared<physics::SphereCollider>(radius);
    return collider;
}

scene::CapsuleColliderComponent CreateCapsuleCollider(float radius = 0.5f, float halfHeight = 1.0f)
{
    scene::CapsuleColliderComponent collider;
    collider.radius = radius;
    collider.halfHeight = halfHeight;
    collider.collider = std::make_shared<physics::CapsuleCollider>(radius, halfHeight);
    return collider;
}

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 ColliderWorldCenter(const scene::GameObject& go, const scene::ColliderComponent& col)
{
    return go.transform.position + go.transform.rotation * ComponentScale(col.center, go.transform.worldScale);
}

template<typename T>
void SyncColliderPreview(scene::GameObject& go, T& col)
{
    if (!col.collider) return;

    const math::Vector3 worldCenter = ColliderWorldCenter(go, col);
    if (auto* mesh = col.collider->GetType() == physics::ColliderType::TRIANGLE_MESH
            ? static_cast<physics::TriangleMeshCollider*>(col.collider.get())
            : nullptr) {
        math::Vector3 scale = go.transform.worldScale;
        if constexpr (std::is_same_v<T, scene::MeshColliderComponent>) {
            if (!col.useTransformScale)
                scale = math::Vector3::ONE;
        }
        mesh->UpdateWithScale(worldCenter, go.transform.rotation, scale);
    } else if (auto* hull = col.collider->GetType() == physics::ColliderType::CONVEX_HULL
            ? static_cast<physics::ConvexHullCollider*>(col.collider.get())
            : nullptr) {
        math::Vector3 scale = go.transform.worldScale;
        if constexpr (std::is_same_v<T, scene::ConvexHullColliderComponent>) {
            if (!col.useTransformScale)
                scale = math::Vector3::ONE;
        }
        hull->UpdateWithScale(worldCenter, go.transform.rotation, scale);
    } else {
        col.collider->Update(worldCenter, go.transform.rotation);
    }
}

std::string NormalizeAssetPath(std::string path)
{
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

std::shared_ptr<renderer::Mesh> MeshFromModelPath(const std::string& path, int meshIndex)
{
    if (path.empty()) return {};
    std::string filePath = path;
    int resolvedIndex = meshIndex;
    const size_t slashPos = path.find_last_of('/');
    const size_t searchFrom = slashPos != std::string::npos ? slashPos : 0;
    const size_t colonPos = path.find(':', searchFrom);
    if (colonPos != std::string::npos) {
        std::string suffix = path.substr(colonPos + 1);
        bool allDigits = !suffix.empty();
        for (char c : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            filePath = path.substr(0, colonPos);
            resolvedIndex = std::atoi(suffix.c_str());
        }
    }

    auto model = asset::AssetManager::Load<asset::Model>(filePath);
    if (!model || resolvedIndex < 0 || resolvedIndex >= static_cast<int>(model->meshes.size()))
        return {};
    return model->meshes[static_cast<size_t>(resolvedIndex)];
}

std::shared_ptr<renderer::Mesh> SourceMeshFromGameObject(scene::GameObject& go,
                                                         std::string& outPath,
                                                         int& outMeshIndex)
{
    if (auto* mr = go.GetComponent<scene::MeshRenderer>(); mr && mr->mesh) {
        outPath = mr->meshPath;
        outMeshIndex = 0;
        return mr->mesh;
    }

    if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>()) {
        if (!smr->model && !smr->modelPath.empty())
            smr->model = asset::AssetManager::Load<asset::Model>(smr->modelPath);
        if (smr->model && smr->meshIndex >= 0 &&
            smr->meshIndex < static_cast<int>(smr->model->meshes.size())) {
            outPath = smr->modelPath;
            outMeshIndex = smr->meshIndex;
            return smr->model->meshes[static_cast<size_t>(smr->meshIndex)];
        }
    }

    return {};
}

std::vector<math::Vector3> MeshPositions(const renderer::Mesh& mesh)
{
    std::vector<math::Vector3> positions;
    positions.reserve(mesh.cpuVertices.size());
    for (const auto& vertex : mesh.cpuVertices)
        positions.push_back(vertex.position);
    return positions;
}

bool BuildMeshCollider(scene::MeshColliderComponent& col, const std::shared_ptr<renderer::Mesh>& mesh)
{
    if (!mesh || mesh->cpuVertices.empty() || mesh->cpuIndices.empty()) return false;
    col.collider = std::make_shared<physics::TriangleMeshCollider>(
        MeshPositions(*mesh), mesh->cpuIndices);
    return true;
}

bool BuildConvexHullCollider(scene::ConvexHullColliderComponent& col, const std::shared_ptr<renderer::Mesh>& mesh)
{
    if (!mesh || mesh->cpuVertices.empty()) return false;
    col.collider = std::make_shared<physics::ConvexHullCollider>(MeshPositions(*mesh));
    return true;
}

scene::RigidBodyComponent CreateDefaultRigidBody()
{
    scene::RigidBodyComponent rb;
    rb.rigidBody = std::make_shared<physics::RigidBody>();
    rb.rigidBody->SetMass(1.0f);
    return rb;
}

bool ComponentMatchesFilter(const char* label, const char* filter)
{
    if (filter[0] == '\0') return true;
    const char* p = label;
    const char* f = filter;
    while (*p) {
        const char* pi = p;
        const char* fi = f;
        while (*pi && *fi && (std::tolower((unsigned char)*pi) == std::tolower((unsigned char)*fi)))
            { ++pi; ++fi; }
        if (*fi == '\0') return true;
        ++p;
    }
    return false;
}

bool AddComponentCategory(const char* label, const char* filter, auto drawItems)
{
    if (filter[0] == '\0') {
        if (ImGui::BeginMenu(label)) {
            drawItems(label, "");
            ImGui::EndMenu();
        }
        return true;
    }

    return drawItems(label, filter);
}

void DrawAddComponentMenu(scene::GameObject& go, char (&filterBuffer)[64])
{
    if (ImGui::Button("Add Component", { -1.0f, 0.0f }))
        ImGui::OpenPopup("##add_component");

    if (!ImGui::BeginPopup("##add_component")) return;

    if (ImGui::IsWindowAppearing()) {
        filterBuffer[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##comp_search", "Search...", filterBuffer, sizeof(filterBuffer));
    ImGui::Separator();

    const char* filter  = filterBuffer;
    bool        anyShown = false;
    bool        didAdd = false;

    auto addItem = [&](const char* category, const char* label, bool enabled, auto action) -> bool {
        char path[128];
        std::snprintf(path, sizeof(path), "%s/%s", category, label);
        char colonPath[128];
        std::snprintf(colonPath, sizeof(colonPath), "%s: %s", category, label);
        if (!ComponentMatchesFilter(path, filter) &&
            !ComponentMatchesFilter(colonPath, filter) &&
            !ComponentMatchesFilter(label, filter))
            return false;
        if (ImGui::MenuItem(filter[0] == '\0' ? label : path, nullptr, false, enabled)) {
            action();
            didAdd = true;
            ImGui::CloseCurrentPopup();
        }
        return true;
    };

    anyShown |= AddComponentCategory("Rendering", filter, [&](const char* category, const char*) {
        bool shown = false;
        shown |= addItem(category, "Mesh Renderer", !go.GetComponent<scene::MeshRenderer>(), [&]() {
            go.AddComponent<scene::MeshRenderer>(CreateDefaultMeshRenderer());
            if (!go.GetComponent<scene::MaterialComponent>())
                go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
        });
        shown |= addItem(category, "Skinned Mesh Renderer", !go.GetComponent<scene::SkinnedMeshRenderer>(), [&]() {
            go.AddComponent<scene::SkinnedMeshRenderer>();
            if (!go.GetComponent<scene::MaterialComponent>())
                go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
        });
        shown |= addItem(category, "Material", !go.GetComponent<scene::MaterialComponent>(), [&]() {
            go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
        });
        shown |= addItem(category, "Light", !go.GetComponent<scene::LightComponent>(), [&]() {
            go.AddComponent<scene::LightComponent>();
        });
        shown |= addItem(category, "Camera", !go.GetComponent<scene::CameraComponent>(), [&]() {
            go.AddComponent<scene::CameraComponent>();
        });
        shown |= addItem(category, "Particle Emitter", !go.GetComponent<scene::ParticleEmitter>(), [&]() {
            go.AddComponent<scene::ParticleEmitter>();
        });
        shown |= addItem(category, "Sky Renderer", !go.GetComponent<scene::SkyRenderer>(), [&]() {
            go.AddComponent<scene::SkyRenderer>();
        });
        return shown;
    });

    anyShown |= AddComponentCategory("Physics", filter, [&](const char* category, const char*) {
        bool shown = false;
        shown |= addItem(category, "Rigidbody", !go.GetComponent<scene::RigidBodyComponent>(), [&]() {
            go.AddComponent<scene::RigidBodyComponent>(CreateDefaultRigidBody());
        });
        shown |= addItem(category, "AABB Collider", !go.GetComponent<scene::AabbColliderComponent>(), [&]() {
            go.AddComponent<scene::AabbColliderComponent>(CreateAabbCollider());
        });
        shown |= addItem(category, "Box Collider", !go.GetComponent<scene::BoxColliderComponent>(), [&]() {
            go.AddComponent<scene::BoxColliderComponent>(CreateBoxCollider());
        });
        shown |= addItem(category, "Sphere Collider", !go.GetComponent<scene::SphereColliderComponent>(), [&]() {
            go.AddComponent<scene::SphereColliderComponent>(CreateSphereCollider());
        });
        shown |= addItem(category, "Capsule Collider", !go.GetComponent<scene::CapsuleColliderComponent>(), [&]() {
            go.AddComponent<scene::CapsuleColliderComponent>(CreateCapsuleCollider());
        });
        shown |= addItem(category, "Mesh Collider", !go.GetComponent<scene::MeshColliderComponent>(), [&]() {
            scene::MeshColliderComponent col;
            auto mesh = SourceMeshFromGameObject(go, col.meshPath, col.meshIndex);
            BuildMeshCollider(col, mesh);
            go.AddComponent<scene::MeshColliderComponent>(std::move(col));
        });
        shown |= addItem(category, "Convex Hull Collider", !go.GetComponent<scene::ConvexHullColliderComponent>(), [&]() {
            scene::ConvexHullColliderComponent col;
            auto mesh = SourceMeshFromGameObject(go, col.meshPath, col.meshIndex);
            BuildConvexHullCollider(col, mesh);
            go.AddComponent<scene::ConvexHullColliderComponent>(std::move(col));
        });
        shown |= addItem(category, "Volume", !go.GetComponent<scene::VolumeComponent>(), [&]() {
            go.AddComponent<scene::VolumeComponent>();
        });
        return shown;
    });

    anyShown |= AddComponentCategory("Animation", filter, [&](const char* category, const char*) {
        bool shown = false;
        shown |= addItem(category, "Animator", !go.GetComponent<scene::AnimatorComponent>(), [&]() {
            go.AddComponent<scene::AnimatorComponent>();
            if (!go.GetComponent<scene::SkinnedMeshRenderer>())
                go.AddComponent<scene::SkinnedMeshRenderer>();
            if (!go.GetComponent<scene::MaterialComponent>())
                go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
        });
        return shown;
    });

    anyShown |= AddComponentCategory("Audio", filter, [&](const char* category, const char*) {
        return addItem(category, "Audio Source", !go.GetComponent<scene::AudioSourceComponent>(), [&]() {
            go.AddComponent<scene::AudioSourceComponent>();
        });
    });

    anyShown |= AddComponentCategory("UI", filter, [&](const char* category, const char*) {
        bool shown = false;
        shown |= addItem(category, "UICanvas", !go.GetComponent<scene::UICanvas>(), [&]() {
            go.AddComponent<scene::UICanvas>();
        });
        shown |= addItem(category, "UIImage", !go.GetComponent<scene::UIImage>(), [&]() {
            go.AddComponent<scene::UIImage>();
        });
        shown |= addItem(category, "UIButton", !go.GetComponent<scene::UIButton>(), [&]() {
            go.AddComponent<scene::UIButton>();
        });
        shown |= addItem(category, "UIText", !go.GetComponent<scene::UIText>(), [&]() {
            go.AddComponent<scene::UIText>();
        });
        shown |= addItem(category, "UILayout Group", !go.GetComponent<scene::UILayoutGroup>(), [&]() {
            go.AddComponent<scene::UILayoutGroup>();
        });
        shown |= addItem(category, "UIAnimator", !go.GetComponent<scene::UIAnimator>(), [&]() {
            go.AddComponent<scene::UIAnimator>();
        });
        return shown;
    });

    anyShown |= AddComponentCategory("Scripts", filter, [&](const char* category, const char*) {
        bool shown = false;
        const bool hasScriptComponent = go.GetComponent<scene::ScriptComponent>() != nullptr;
        const auto scriptTypeNames = scene::ScriptFactory::RegisteredTypeNames();
        for (const std::string& typeName : scriptTypeNames) {
            shown |= addItem(category, typeName.c_str(), !hasScriptComponent, [&]() {
                auto script = scene::ScriptFactory::Create(typeName);
                if (!script) return;

                scene::ScriptComponent sc;
                sc.script = std::move(script);
                go.AddComponent<scene::ScriptComponent>(std::move(sc));
            });
        }
        return shown;
    });

    if (!anyShown)
        ImGui::TextDisabled("No results");

    ImGui::EndPopup();

    if (didAdd)
        filterBuffer[0] = '\0';
}

bool DragVec2(const char* label, math::Vector2& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f)
{
    float data[2] = { value.x, value.y };
    if (!ImGui::DragFloat2(label, data, speed, min, max)) return false;
    value = { data[0], data[1] };
    return true;
}

} // namespace

void DrawBoxCollider(scene::BoxColliderComponent& col, scene::GameObject& go);
void DrawAabbCollider(scene::AabbColliderComponent& col, scene::GameObject& go);
void DrawSphereCollider(scene::SphereColliderComponent& col, scene::GameObject& go);
void DrawCapsuleCollider(scene::CapsuleColliderComponent& col, scene::GameObject& go);
void DrawMeshCollider(scene::MeshColliderComponent& col, scene::GameObject& go);
void DrawConvexHullCollider(scene::ConvexHullColliderComponent& col, scene::GameObject& go);

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

    DrawComponentSection<scene::MeshRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Mesh Renderer",
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

    DrawComponentSection<scene::SkinnedMeshRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Skinned Mesh Renderer",
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

    DrawComponentSection<scene::AnimatorComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Animator",
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

    DrawComponentSection<scene::MaterialComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Material",
        [](scene::MaterialComponent& mc, EditorContext&) {
            auto applyShader = [&mc]() {
                auto& material = EnsureMaterial(mc);
                material.shaderPath = mc.shaderPath;
                if (auto* res = renderer::ResourceManager::Active()) {
                    material.shader = mc.shaderPath.empty()
                        ? renderer::ResourceHandle<renderer::ShaderTag>{}
                        : res->LoadShader(mc.shaderPath);
                    material.Init(*res);
                    material.Upload(*res);
                }
            };
            auto applyAlbedo = [&mc]() {
                auto& material = EnsureMaterial(mc);
                if (auto* res = renderer::ResourceManager::Active()) {
                    material.albedoTexture = mc.albedoTexPath.empty()
                        ? renderer::ResourceHandle<renderer::TextureTag>{}
                        : res->LoadTexture(mc.albedoTexPath);
                    material.Upload(*res);
                }
            };
            auto applyNormal = [&mc]() {
                auto& material = EnsureMaterial(mc);
                if (auto* res = renderer::ResourceManager::Active()) {
                    material.normalTexture = mc.normalTexPath.empty()
                        ? renderer::ResourceHandle<renderer::TextureTag>{}
                        : res->LoadTexture(mc.normalTexPath);
                    material.Upload(*res);
                }
            };

            if (!mc.material)
                applyShader();

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

    DrawComponentSection<scene::LightComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Light",
        [go](scene::LightComponent& lc, EditorContext&) {
            DrawLightFields(*go, lc);
        });

    DrawComponentSection<scene::CameraComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Camera",
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

    DrawComponentSection<scene::ParticleEmitter>(go, ctx, m_componentClipboard, m_componentClipboardType, "Particle Emitter",
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

    DrawComponentSection<scene::AudioSourceComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Audio Source",
        [](scene::AudioSourceComponent& asc, EditorContext&) {
            ImGui::Checkbox("Play On Awake", &asc.playOnAwake);
            ImGui::Checkbox("Loop", &asc.loop);
            char clipBuf[512];
            std::snprintf(clipBuf, sizeof(clipBuf), "%s", asc.clipPath.c_str());
            if (ImGui::InputText("Clip Path", clipBuf, sizeof(clipBuf)))
                asc.clipPath = clipBuf;
            ImGui::SliderFloat("Volume", &asc.volume, 0.0f, 1.0f);
        });

    DrawComponentSection<scene::AabbColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "AABB Collider",
        [go](scene::AabbColliderComponent& col, EditorContext&) {
            DrawAabbCollider(col, *go);
        });

    DrawComponentSection<scene::BoxColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Box Collider",
        [go](scene::BoxColliderComponent& col, EditorContext&) {
            DrawBoxCollider(col, *go);
        });

    DrawComponentSection<scene::SphereColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sphere Collider",
        [go](scene::SphereColliderComponent& col, EditorContext&) {
            DrawSphereCollider(col, *go);
        });

    DrawComponentSection<scene::CapsuleColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Capsule Collider",
        [go](scene::CapsuleColliderComponent& col, EditorContext&) {
            DrawCapsuleCollider(col, *go);
        });

    DrawComponentSection<scene::MeshColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Mesh Collider",
        [&go](scene::MeshColliderComponent& col, EditorContext&) {
            DrawMeshCollider(col, *go);
        });

    DrawComponentSection<scene::ConvexHullColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Convex Hull Collider",
        [&go](scene::ConvexHullColliderComponent& col, EditorContext&) {
            DrawConvexHullCollider(col, *go);
        });

    DrawComponentSection<scene::RigidBodyComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Rigid Body",
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

            auto freezePosition = body.GetFreezePosition();
            if (ImGui::Checkbox("Freeze Position X", &freezePosition.x))
                body.SetFreezePosition(freezePosition);
            ImGui::SameLine();
            if (ImGui::Checkbox("Y##FreezePosition", &freezePosition.y))
                body.SetFreezePosition(freezePosition);
            ImGui::SameLine();
            if (ImGui::Checkbox("Z##FreezePosition", &freezePosition.z))
                body.SetFreezePosition(freezePosition);

            auto freezeRotation = body.GetFreezeRotation();
            if (ImGui::Checkbox("Freeze Rotation X", &freezeRotation.x))
                body.SetFreezeRotation(freezeRotation);
            ImGui::SameLine();
            if (ImGui::Checkbox("Y##FreezeRotation", &freezeRotation.y))
                body.SetFreezeRotation(freezeRotation);
            ImGui::SameLine();
            if (ImGui::Checkbox("Z##FreezeRotation", &freezeRotation.z))
                body.SetFreezeRotation(freezeRotation);

            ImGui::DragFloat("Charge", &body.m_charge, 0.01f, -1000.0f, 1000.0f);
            ImGui::Checkbox("Gravity Source", &body.m_isGravitationalSource);
            ImGui::DragFloat("Gravity Mass", &body.m_gravitationalMass, 0.05f, 0.0f, 100000.0f);
        });

    DrawComponentSection<scene::VolumeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Volume",
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

    DrawComponentSection<scene::SkyRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sky Renderer",
        [](scene::SkyRenderer& sr, EditorContext&) {
            widgets::DragVec3("Rayleigh", sr.rayleighScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Mie Scattering", &sr.mieScattering, 0.0001f, 0.0f, 1.0f);
            ImGui::DragFloat("Sun Intensity", &sr.sunIntensity, 0.1f, 0.0f, 1000.0f);
            ImGui::SliderFloat("Mie G", &sr.mieG, -0.99f, 0.99f);
        });

    DrawComponentSection<scene::UICanvas>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Canvas",
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

    DrawComponentSection<scene::UIImage>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Image",
        [](scene::UIImage& image, EditorContext&) {
            // 菴咲ｽｮ繝ｻ繧ｵ繧､繧ｺ縺ｯ Transform 縺ｧ邂｡逅・(荳翫・ Transform 繧ｻ繧ｯ繧ｷ繝ｧ繝ｳ繧貞盾辣ｧ)
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

    DrawComponentSection<scene::UIButton>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Button",
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

    DrawComponentSection<scene::UIText>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Text",
        [](scene::UIText& text, EditorContext&) {
            // 菴咲ｽｮ縺ｯ Transform 縺ｧ邂｡逅・
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

    DrawComponentSection<scene::UILayoutGroup>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Layout Group",
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

    DrawComponentSection<scene::UIAnimator>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Animator",
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
    DrawAddComponentMenu(*go, m_addComponentFilter);
}

void DrawColliderCommon(scene::ColliderComponent& col)
{
    widgets::DragVec3("Center", col.center, 0.01f, -1000.0f, 1000.0f);
    ImGui::Checkbox("Is Trigger", &col.isTrigger);
    ImGui::DragFloat("Restitution", &col.material.restitution, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Static Friction", &col.material.staticFriction, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Dynamic Friction", &col.material.dynamicFriction, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Density", &col.material.density, 0.01f, 0.0f, 100000.0f);
}

void DrawAabbCollider(scene::AabbColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    widgets::DragVec3("Size", col.size, 0.01f, 0.001f, 1000.0f);
    auto* box = col.collider && col.collider->GetType() == physics::ColliderType::AABB
        ? static_cast<physics::AABBCollider*>(col.collider.get())
        : nullptr;
    if (!box) {
        col.collider = std::make_shared<physics::AABBCollider>(col.size * 0.5f);
        box = static_cast<physics::AABBCollider*>(col.collider.get());
    }
    box->m_halfExtents = col.size * 0.5f;
    SyncColliderPreview(go, col);
}

void DrawBoxCollider(scene::BoxColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    widgets::DragVec3("Size", col.size, 0.01f, 0.001f, 1000.0f);
    auto* box = col.collider && col.collider->GetType() == physics::ColliderType::OBB
        ? static_cast<physics::OBBCollider*>(col.collider.get())
        : nullptr;
    if (!box) {
        col.collider = std::make_shared<physics::OBBCollider>(col.size * 0.5f);
        box = static_cast<physics::OBBCollider*>(col.collider.get());
    }
    box->m_halfExtents = col.size * 0.5f;
    SyncColliderPreview(go, col);
}

void DrawSphereCollider(scene::SphereColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    auto* sphere = col.collider && col.collider->GetType() == physics::ColliderType::SPHERE
        ? static_cast<physics::SphereCollider*>(col.collider.get())
        : nullptr;
    if (!sphere) {
        col.collider = std::make_shared<physics::SphereCollider>(col.radius);
        sphere = static_cast<physics::SphereCollider*>(col.collider.get());
    }
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    sphere->m_radius = col.radius;
    SyncColliderPreview(go, col);
}

void DrawCapsuleCollider(scene::CapsuleColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    auto* capsule = col.collider && col.collider->GetType() == physics::ColliderType::CAPSULE
        ? static_cast<physics::CapsuleCollider*>(col.collider.get())
        : nullptr;
    if (!capsule) {
        col.collider = std::make_shared<physics::CapsuleCollider>(col.radius, col.halfHeight);
        capsule = static_cast<physics::CapsuleCollider*>(col.collider.get());
    }
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    ImGui::DragFloat("Half Height", &col.halfHeight, 0.01f, 0.001f, 1000.0f);
    capsule->m_radius = col.radius;
    capsule->m_halfHeight = col.halfHeight;
    SyncColliderPreview(go, col);
}

void DrawMeshCollider(scene::MeshColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    ImGui::Checkbox("Use Transform Scale", &col.useTransformScale);
    char pathBuf[256];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s", col.meshPath.c_str());
    if (ImGui::InputText("Model Path", pathBuf, sizeof(pathBuf))) {
        col.meshPath = NormalizeAssetPath(pathBuf);
        col.collider.reset();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            col.meshPath = NormalizeAssetPath(static_cast<const char*>(p->Data));
            col.collider.reset();
            BuildMeshCollider(col, MeshFromModelPath(col.meshPath, col.meshIndex));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::DragInt("Mesh Index", &col.meshIndex, 1.0f, 0, 1024);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        col.collider.reset();
        BuildMeshCollider(col, MeshFromModelPath(col.meshPath, col.meshIndex));
    }
    if (ImGui::Button("Rebuild From MeshRenderer")) {
        col.collider.reset();
        auto mesh = SourceMeshFromGameObject(go, col.meshPath, col.meshIndex);
        BuildMeshCollider(col, mesh);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rebuild From FBX")) {
        col.collider.reset();
        BuildMeshCollider(col, MeshFromModelPath(col.meshPath, col.meshIndex));
    }
    if (!col.collider)
        ImGui::TextDisabled("No mesh collider data. Drop FBX or rebuild from renderer.");
    SyncColliderPreview(go, col);
}

void DrawConvexHullCollider(scene::ConvexHullColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    ImGui::Checkbox("Use Transform Scale", &col.useTransformScale);
    char pathBuf[256];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s", col.meshPath.c_str());
    if (ImGui::InputText("Model Path", pathBuf, sizeof(pathBuf))) {
        col.meshPath = NormalizeAssetPath(pathBuf);
        col.collider.reset();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            col.meshPath = NormalizeAssetPath(static_cast<const char*>(p->Data));
            col.collider.reset();
            BuildConvexHullCollider(col, MeshFromModelPath(col.meshPath, col.meshIndex));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::DragInt("Mesh Index", &col.meshIndex, 1.0f, 0, 1024);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        col.collider.reset();
        BuildConvexHullCollider(col, MeshFromModelPath(col.meshPath, col.meshIndex));
    }
    if (ImGui::Button("Rebuild From MeshRenderer")) {
        col.collider.reset();
        auto mesh = SourceMeshFromGameObject(go, col.meshPath, col.meshIndex);
        BuildConvexHullCollider(col, mesh);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rebuild From FBX")) {
        col.collider.reset();
        BuildConvexHullCollider(col, MeshFromModelPath(col.meshPath, col.meshIndex));
    }
    if (!col.collider)
        ImGui::TextDisabled("No hull data. Drop FBX or rebuild from renderer.");
    SyncColliderPreview(go, col);
}

} // namespace fbzz::editor
