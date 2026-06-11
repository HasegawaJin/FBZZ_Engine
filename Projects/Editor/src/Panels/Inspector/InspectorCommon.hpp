// FBZZ Engine
// InspectorCommon.hpp | fbzz::editor
// Inspector のカテゴリ分割ファイルで共有する描画ヘルパー
#pragma once

#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Physics/Layer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Util/StringUtils.hpp>
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
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>


namespace fbzz::editor {


// Hierarchy パネルからのドラッグ＆ドロップを受け取り、ドロップされた GameObject を返す。
// WHY: IK Solver の Bone 名・Target 名フィールドに Hierarchy から直接ドロップできるようにする。
//      nullptr の場合はドロップなし (BeginDragDropTarget が false を返すか payload 不正)。
inline scene::GameObject* AcceptHierarchyDrop(scene::Scene* scene)
{
    if (!ImGui::BeginDragDropTarget()) return nullptr;
    scene::GameObject* result = nullptr;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY")) {
        if (p->DataSize == sizeof(scene::EntityID) && scene) {
            scene::EntityID id;
            std::memcpy(&id, p->Data, sizeof(id));
            result = scene->GetGameObject(id);
        }
    }
    ImGui::EndDragDropTarget();
    return result;
}

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
inline void DrawLightFields(scene::GameObject& go, scene::LightComponent& lc)
{
    static constexpr const char* kTypeNames[] = { "Directional", "Point", "Spot" };
    int typeIdx = static_cast<int>(lc.type);
    if (ImGui::Combo("Type", &typeIdx, kTypeNames, 3))
        lc.type = static_cast<scene::LightComponent::Type>(typeIdx);

    widgets::ColorEdit3("Color", lc.color);
    ImGui::DragFloat("Intensity", &lc.intensity, 0.05f, 0.0f, 200.0f);

    if (lc.type != scene::LightComponent::Type::Directional) {
        float pos[3] = {
            go.transform.position.x,
            go.transform.position.y,
            go.transform.position.z
        };
        if (ImGui::DragFloat3("Position", pos, 0.1f))
            go.transform.position = { pos[0], pos[1], pos[2] };
        ImGui::DragFloat("Range", &lc.range, 0.1f, 0.0f, 500.0f);
    }

    if (lc.type == scene::LightComponent::Type::Spot) {
        ImGui::DragFloat("Inner Cone", &lc.innerCone, 0.5f, 0.0f, 89.0f);
        ImGui::DragFloat("Outer Cone", &lc.outerCone, 0.5f, 0.0f, 89.0f);
    }

    if (lc.type != scene::LightComponent::Type::Point) {
        auto fwd = go.transform.forward;
        float dir[3] = { fwd.x, fwd.y, fwd.z };
        ImGui::InputFloat3("Forward", dir, "%.3f", ImGuiInputTextFlags_ReadOnly);
    }
}
inline scene::MeshRenderer CreateDefaultMeshRenderer()
{
    scene::MeshRenderer mr;
    mr.meshPath = "primitive:cube";
    if (auto* resources = renderer::ResourceManager::Active())
        mr.mesh = renderer::PrimitiveMesh::Cube(*resources);
    return mr;
}
inline scene::MaterialComponent CreateDefaultMaterialComponent(bool skinned = false)
{
    scene::MaterialComponent mc;
    (void)skinned;
    return mc;
}
inline scene::AabbColliderComponent CreateAabbCollider(const math::Vector3& size = math::Vector3::ONE)
{
    scene::AabbColliderComponent collider;
    collider.size = size;
    collider.collider = std::make_shared<physics::AABBCollider>(size * 0.5f);
    return collider;
}
inline scene::BoxColliderComponent CreateBoxCollider(const math::Vector3& halfExtents = { 0.5f, 0.5f, 0.5f })
{
    scene::BoxColliderComponent collider;
    collider.size = halfExtents * 2.0f;
    collider.collider = std::make_shared<physics::OBBCollider>(halfExtents);
    return collider;
}
inline scene::SphereColliderComponent CreateSphereCollider(float radius = 0.5f)
{
    scene::SphereColliderComponent collider;
    collider.radius = radius;
    collider.collider = std::make_shared<physics::SphereCollider>(radius);
    return collider;
}
inline scene::CapsuleColliderComponent CreateCapsuleCollider(float radius = 0.5f, float halfHeight = 1.0f)
{
    scene::CapsuleColliderComponent collider;
    collider.radius = radius;
    collider.halfHeight = halfHeight;
    collider.collider = std::make_shared<physics::CapsuleCollider>(radius, halfHeight);
    return collider;
}
inline math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}
inline math::Vector3 ColliderWorldCenter(const scene::GameObject& go, const scene::ColliderComponent& col)
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
inline std::string SanitizeTerrainAssetName(const std::string& name)
{
    std::string result = name.empty() ? "Terrain" : name;
    for (char& c : result) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok) c = '_';
    }
    return result;
}
inline std::string UniqueTerrainAssetPath(const EditorContext& ctx, const std::string& objectName)
{
    const std::string assetRoot = ctx.projectRoot.empty()
        ? "Assets"
        : ctx.projectRoot + "/Assets";
    const std::string terrainDir = assetRoot + "/Terrain";
    util::FileSystem::EnsureDirectory(terrainDir);

    const std::string base = terrainDir + "/" + SanitizeTerrainAssetName(objectName);
    std::string path = base + ".fbzzterrain";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".fbzzterrain";
    return NormalizeAssetPath(path);
}
inline std::string TerrainAssetDiskPath(const EditorContext& ctx, const std::string& assetPath)
{
    return ToProjectAssetDiskPath(ctx.projectRoot, assetPath);
}

// WHAT: .fzmat の assets/ 相対パスを保存 API に渡せるディスクパスへ変換する。
// WHY: MaterialComponent はポータブルな Assets 起点パスだけを保持するため、Editor の保存時だけ projectRoot を補完する。
inline std::string MaterialAssetDiskPath(const EditorContext& ctx, const std::string& assetPath)
{
    return ToProjectAssetDiskPath(ctx.projectRoot, assetPath);
}
inline std::shared_ptr<renderer::Mesh> MeshFromModelPath(const std::string& path, int meshIndex)
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
inline std::shared_ptr<renderer::Mesh> SourceMeshFromGameObject(scene::GameObject& go,
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
inline std::vector<math::Vector3> MeshPositions(const renderer::Mesh& mesh)
{
    std::vector<math::Vector3> positions;
    positions.reserve(mesh.cpuVertices.size());
    for (const auto& vertex : mesh.cpuVertices)
        positions.push_back(vertex.position);
    return positions;
}
inline bool BuildMeshCollider(scene::MeshColliderComponent& col, const std::shared_ptr<renderer::Mesh>& mesh)
{
    if (!mesh || mesh->cpuVertices.empty() || mesh->cpuIndices.empty()) return false;
    col.collider = std::make_shared<physics::TriangleMeshCollider>(MeshPositions(*mesh), mesh->cpuIndices);
    return true;
}
inline bool BuildConvexHullCollider(scene::ConvexHullColliderComponent& col, const std::shared_ptr<renderer::Mesh>& mesh)
{
    if (!mesh || mesh->cpuVertices.empty()) return false;
    col.collider = std::make_shared<physics::ConvexHullCollider>(MeshPositions(*mesh));
    return true;
}
inline scene::RigidBodyComponent CreateDefaultRigidBody()
{
    scene::RigidBodyComponent rb;
    rb.rigidBody = std::make_shared<physics::RigidBody>();
    rb.rigidBody->SetMass(1.0f);
    return rb;
}
inline bool ComponentMatchesFilter(const char* label, const char* filter)
{
    if (filter[0] == '\0') return true;
    return util::StringUtils::ContainsCI(label, filter);
}

template<typename DrawItems>
inline bool AddComponentCategory(const char* label, const char* filter, DrawItems drawItems)
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
inline void DrawAddComponentMenu(scene::GameObject& go, char (&filterBuffer)[64])
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
                go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
        });
        shown |= addItem(category, "Material", !go.GetComponent<scene::MaterialComponent>(), [&]() {
            go.AddComponent<scene::MaterialComponent>(
                CreateDefaultMaterialComponent(go.GetComponent<scene::SkinnedMeshRenderer>() != nullptr));
        });
        shown |= addItem(category, "Light", !go.GetComponent<scene::LightComponent>(), [&]() {
            go.AddComponent<scene::LightComponent>();
        });
        shown |= addItem(category, "Camera", !go.GetComponent<scene::CameraComponent>(), [&]() {
            go.AddComponent<scene::CameraComponent>();
        });
        shown |= addItem(category, "Lifetime", !go.GetComponent<scene::LifetimeComponent>(), [&]() {
            go.AddComponent<scene::LifetimeComponent>();
        });
        shown |= addItem(category, "Particle Emitter", !go.GetComponent<scene::ParticleEmitter>(), [&]() {
            go.AddComponent<scene::ParticleEmitter>();
        });
        shown |= addItem(category, "Trail", !go.GetComponent<scene::TrailComponent>(), [&]() {
            go.AddComponent<scene::TrailComponent>();
        });
        shown |= addItem(category, "Mesh Trail", !go.GetComponent<scene::MeshTrailComponent>(), [&]() {
            go.AddComponent<scene::MeshTrailComponent>();
        });
        shown |= addItem(category, "Sky Renderer", !go.GetComponent<scene::SkyRenderer>(), [&]() {
            go.AddComponent<scene::SkyRenderer>();
        });
        shown |= addItem(category, "Decal", !go.GetComponent<scene::DecalComponent>(), [&]() {
            go.AddComponent<scene::DecalComponent>();
        });
        // Terrain は 1 GO に 1 つ。MeshColliderComponent も同時に追加してすぐ物理が有効になる。
        shown |= addItem(category, "Terrain", !go.GetComponent<scene::TerrainComponent>(), [&]() {
            scene::TerrainComponent tc{};
            tc.columns   = 33;
            tc.rows      = 33;
            tc.cellSize  = 2.0f;
            tc.maxHeight = 10.0f;
            tc.chunkSize = 32;
            tc.materialPath = DefaultTerrainMaterialPath();
            tc.InitFlat(0.0f);
            tc.heightDirty   = true;
            tc.colliderDirty = true;
            go.AddComponent<scene::TerrainComponent>(std::move(tc));
            // 物理コライダーのキャリアとして MeshColliderComponent を追加
            // (meshPath = "" → PhysicsSystem が TerrainComponent から自動構築する)
            if (!go.GetComponent<scene::MeshColliderComponent>())
                go.AddComponent<scene::MeshColliderComponent>();
        });
        shown |= addItem(category, "Water", !go.GetComponent<scene::WaterComponent>(), [&]() {
            scene::WaterComponent water{};
            water.resolutionX = 64;
            water.resolutionZ = 64;
            water.extentX = 80.0f;
            water.extentZ = 80.0f;
            water.materialPath = DefaultWaterMaterialPath();
            water.meshDirty = true;
            water.foamDirty = true;
            water.texDirty = true;
            go.AddComponent<scene::WaterComponent>(std::move(water));
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
                go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
        });
        shown |= addItem(category, "IK Solver", !go.GetComponent<scene::IKSolverComponent>(), [&]() {
            go.AddComponent<scene::IKSolverComponent>();
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
        const auto scriptTypeNames = scene::ScriptFactory::RegisteredTypeNames();
        for (const std::string& typeName : scriptTypeNames) {
            shown |= addItem(category, typeName.c_str(), true, [&]() {
                auto script = scene::ScriptFactory::Create(typeName);
                if (!script) return;

                auto* sc = go.GetComponent<scene::ScriptComponent>();
                if (!sc)
                    sc = &go.AddComponent<scene::ScriptComponent>();

                scene::ScriptEntry& entry = sc->scripts.emplace_back();
                entry.script = std::move(script);
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
inline bool DragVec2(const char* label, math::Vector2& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f)
{
    float data[2] = { value.x, value.y };
    if (!ImGui::DragFloat2(label, data, speed, min, max)) return false;
    value = { data[0], data[1] };
    return true;
}



void DrawTransformInspector(scene::GameObject* go, EditorContext& ctx);
void DrawRenderingInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawAnimationInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawMaterialInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawLightingInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawEffectsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawAudioInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawPhysicsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawEnvironmentInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawUIInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawTerrainWaterInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawScriptInspectors(scene::GameObject* go, EditorContext& ctx);

} // namespace fbzz::editor
