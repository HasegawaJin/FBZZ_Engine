// FBZZ Engine
// InspectorPanel.cpp | fbzz::editor
// Selected Entity component inspector and editor
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
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
#include <Engine/Scene/WaterAssetSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
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
#include <cstring>
#include <memory>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace fbzz::editor {

namespace {


// Hierarchy パネルからのドラッグ＆ドロップを受け取り、ドロップされた GameObject を返す。
// WHY: IK Solver の Bone 名・Target 名フィールドに Hierarchy から直接ドロップできるようにする。
//      nullptr の場合はドロップなし (BeginDragDropTarget が false を返すか payload 不正)。
scene::GameObject* AcceptHierarchyDrop(scene::Scene* scene)
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

scene::MaterialComponent CreateDefaultMaterialComponent(bool skinned = false)
{
    scene::MaterialComponent mc;
    mc.shaderPath = skinned
        ? "Assets/shaders/Material/Skinned/SkinnedPBR.hlsl"
        : "Assets/shaders/Material/Surface/Phong.hlsl";
    auto material = std::make_shared<renderer::Material>();
    material->shaderPath = mc.shaderPath;
    if (auto* resources = renderer::ResourceManager::Active()) {
        material->shader = resources->LoadShader(mc.shaderPath);
        static renderer::ShaderDescriptor s_fallback;
        const renderer::ShaderDescriptor* desc = &s_fallback;
        if (auto* sh = resources->Get(material->shader))
            desc = &sh->GetDescriptor();
        material->Init(*resources, desc->cbufferSize);
        mc.InitFromDescriptor(*desc);
        if (const auto* v = desc->FindVar("roughness"))
        {
            float rough = 0.65f;
            std::memcpy(mc.paramData.data() + v->offset, &rough, sizeof(float));
        }
        material->paramData = mc.paramData;
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

    // WHY: Inspector で OS の絶対パスを保存すると、プロジェクトを移動しただけで
    //      シーン復元に失敗する。Assets 配下のファイルは必ず Assets 起点に丸める。
    if (path.rfind("Assets/", 0) == 0) return path;

    const std::string marker = "/Assets/";
    const size_t assetsPos = path.find(marker);
    if (assetsPos != std::string::npos)
        return path.substr(assetsPos + 1);

    return path;
}

std::string SanitizeTerrainAssetName(const std::string& name)
{
    std::string result = name.empty() ? "Terrain" : name;
    for (char& c : result) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok) c = '_';
    }
    return result;
}

std::string UniqueTerrainAssetPath(const EditorContext& ctx, const std::string& objectName)
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

std::string TerrainAssetDiskPath(const EditorContext& ctx, const std::string& assetPath)
{
    if (assetPath.rfind("Assets/", 0) == 0 && !ctx.projectRoot.empty())
        return ctx.projectRoot + "/" + assetPath;
    return assetPath;
}

// WHAT: "Assets/Water/..." → プロジェクトルートからの絶対パスに変換する。
std::string WaterAssetDiskPath(const EditorContext& ctx, const std::string& assetPath)
{
    if (assetPath.rfind("Assets/", 0) == 0 && !ctx.projectRoot.empty())
        return ctx.projectRoot + "/" + assetPath;
    return assetPath;
}

// WHAT: 重複しないデフォルトの .fbzzwater パスを生成する。
std::string UniqueWaterAssetPath(const EditorContext& ctx, const std::string& objectName)
{
    std::string safe = objectName;
    for (char& c : safe) { if (c == ' ' || c == '/' || c == '\\') c = '_'; }

    const std::string assetRoot = ctx.projectRoot.empty()
        ? "Assets"
        : ctx.projectRoot + "/Assets";
    const std::string waterDir = assetRoot + "/Water";
    util::FileSystem::EnsureDirectory(waterDir);

    const std::string base = waterDir + "/" + safe;
    std::string path = base + ".fbzzwater";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".fbzzwater";
    return NormalizeAssetPath(path);
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
        shown |= addItem(category, "Particle Emitter", !go.GetComponent<scene::ParticleEmitter>(), [&]() {
            go.AddComponent<scene::ParticleEmitter>();
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
    // ------------------------------------------------------------------
    // ロック解決
    // ロック中は m_lockedEntityId のオブジェクトを表示する。
    // ロック先が破棄されていた場合は自動解除する。
    // ------------------------------------------------------------------
    scene::GameObject* selectedGo = ctx.GetSelectedGO();
    scene::GameObject* go = nullptr;

    if (m_locked) {
        if (ctx.activeScene)
            go = ctx.activeScene->GetGameObject(m_lockedEntityId);
        if (!go) {
            // 破棄 / シーン切り替えで無効になった場合は自動解除
            m_locked = false;
            m_lockedEntityId = {};
        }
    } else {
        go = selectedGo;
    }

    // ------------------------------------------------------------------
    // ロックボタン (右端に配置)
    // WHY: ボタン押下で m_locked が変化するため、PushStyleColor / PopStyleColor の
    //      対応を保証するには押下前の状態を wasLocked に固定しておく必要がある。
    //      m_locked を Push 判定と Pop 判定の両方で使うと片方が空振りしてクラッシュする。
    // ------------------------------------------------------------------
    {
        // ボタン描画前の状態を保存して Push/Pop を必ず対称にする
        const bool wasLocked = m_locked;
        const char* label    = wasLocked ? "Unlock" : "Lock";
        const float padX     = ImGui::GetStyle().FramePadding.x;
        const float btnW     = ImGui::CalcTextSize(label).x + padX * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);

        if (wasLocked)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.85f, 1.0f));

        if (ImGui::Button(label)) {
            if (wasLocked) {
                m_locked         = false;
                m_lockedEntityId = {};
            } else if (go) {
                m_locked         = true;
                m_lockedEntityId = go->GetID();
            }
        }

        if (wasLocked) ImGui::PopStyleColor();  // wasLocked で対称を保証

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(wasLocked
                ? "Unlock — follow selection changes"
                : "Lock Inspector to current selection");

        // ロック中はロック先の名前をバナー表示
        if (wasLocked && go) {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
            ImGui::TextDisabled("Locked: %s", go->name.c_str());
        }
    }

    ImGui::Spacing();

    if (!go) {
        ImGui::TextDisabled("Nothing selected");
        return;
    }

    // ── Save as Prefab ───────────────────────────────────────────────────────
    // WHY: Hierarchy のコンテキストメニューを使わずに Inspector から直接 Prefab 化できる動線。
    //      Unity の Inspector ヘッダーと同様に最上部に配置する。
    {
        static constexpr const char* kSaveLabel = "Save as Prefab";
        const float btnW = ImGui::CalcTextSize(kSaveLabel).x
                         + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);
        if (ImGui::SmallButton(kSaveLabel) && !ctx.selectedEntities.empty() && ctx.activeScene) {
            // 保存先: <projectRoot>/Assets/Prefabs/<name>.fbzzprefab (重複時は連番付き)
            const std::string assetRoot = ctx.projectRoot.empty()
                ? "Assets"
                : ctx.projectRoot + "/Assets";
            const std::string prefabDir = assetRoot + "/Prefabs";
            util::FileSystem::EnsureDirectory(prefabDir);

            // ファイル名として使えない文字をアンダースコアに置換する
            std::string safeName;
            for (char c : go->name) {
                const bool ok = std::isalnum(static_cast<unsigned char>(c))
                             || c == '_' || c == '-' || c == ' ';
                safeName += ok ? c : '_';
            }
            if (safeName.empty()) safeName = "Prefab";

            const std::string base = prefabDir + "/" + safeName;
            std::string savePath = base + ".fbzzprefab";
            for (int i = 1; util::FileSystem::Exists(savePath) && i < 10000; ++i)
                savePath = base + " " + std::to_string(i) + ".fbzzprefab";

            if (PrefabSerializer::SaveSelection(*ctx.activeScene, ctx.selectedEntities, savePath))
                ctx.requestAssetBrowserRefresh = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save selected object as prefab to Assets/Prefabs");
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
        ImGui::SetNextItemWidth(-1.0f);
        const std::string currentLayerLabel = ps.layerNames[layerIdx].empty()
            ? ("User Layer " + std::to_string(layerIdx))
            : ps.layerNames[layerIdx];
        if (ImGui::BeginCombo("##layer", currentLayerLabel.c_str())) {
            for (int i = 0; i < 32; ++i) {
                const std::string layerLabel = ps.layerNames[i].empty()
                    ? ("User Layer " + std::to_string(i))
                    : ps.layerNames[i];
                const bool selected = i == layerIdx;
                if (ImGui::Selectable(layerLabel.c_str(), selected))
                    go->layer = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
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
                    mr.meshPath = NormalizeAssetPath(meshBuf);

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
                        mr.meshPath = NormalizeAssetPath(static_cast<const char*>(p->Data));
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
                smr.modelPath = NormalizeAssetPath(pathBuf);
            if (ImGui::IsItemDeactivatedAfterEdit()) loadModel();
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    smr.modelPath = NormalizeAssetPath(static_cast<const char*>(p->Data));
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
                            go2->AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
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
                    anim.clipSources[i] = NormalizeAssetPath(buf);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    { anim.clips.clear(); anim.clipsLoaded = false; }
                // DragDrop target must be right after InputText, before SameLine
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        anim.clipSources[i] = NormalizeAssetPath(static_cast<const char*>(p->Data));
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
                    std::string path = NormalizeAssetPath(static_cast<const char*>(p->Data));
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

            ImGui::DragFloat("Speed", &anim.speed, 0.01f, -10.0f, 10.0f);
            ImGui::Checkbox("Playing", &anim.playing);
            // Time / Loop はステートマシン未使用時のみ表示する（ステートマシン使用時は per-state で管理）
            if (anim.states.empty()) {
                ImGui::DragFloat("Time", &anim.time, 0.01f, 0.0f, 100000.0f);
                ImGui::Checkbox("Loop", &anim.loop);
            }

            // ── ステートマシン UI ────────────────────────────────────────────────
            if (!anim.states.empty() || true) {
                ImGui::Separator();
                ImGui::TextColored({ 0.9f, 0.7f, 0.2f, 1.0f }, "State Machine");

                // ── ランタイム状態表示 ─────────────────────────────────────────
                if (!anim.currentStateName.empty()) {
                    ImGui::Text("Current: %s", anim.currentStateName.c_str());
                    const float nt = anim.GetNormalizedTime();
                    char overlay[64];
                    std::snprintf(overlay, sizeof(overlay), "%.2f", nt);
                    ImGui::ProgressBar(nt, { -1.0f, 0.0f }, overlay);
                    if (!anim.blendToState.empty()) {
                        ImGui::TextDisabled(" -> %s  (blend: %.0f%%)",
                            anim.blendToState.c_str(),
                            anim.blendWeight * 100.0f);
                    }
                    ImGui::Separator();
                }

                // ── Default State コンボ ──────────────────────────────────────
                if (!anim.states.empty()) {
                    int defIdx = 0;
                    std::vector<const char*> stateNames;
                    stateNames.reserve(anim.states.size());
                    for (int si = 0; si < static_cast<int>(anim.states.size()); ++si) {
                        stateNames.push_back(anim.states[static_cast<size_t>(si)].name.c_str());
                        if (anim.states[static_cast<size_t>(si)].name == anim.defaultStateName)
                            defIdx = si;
                    }
                    if (ImGui::Combo("Default State", &defIdx,
                                     stateNames.data(), static_cast<int>(stateNames.size()))) {
                        anim.defaultStateName  = anim.states[static_cast<size_t>(defIdx)].name;
                        anim.currentStateName  = "";  // 再初期化トリガー
                    }
                }

                // ── Parameters ────────────────────────────────────────────────
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Parameters")) {
                    static const char* kParamTypes[] = { "Float", "Int", "Bool", "Trigger" };
                    int removeParamIdx = -1;

                    for (int pi = 0; pi < static_cast<int>(anim.parameters.size()); ++pi) {
                        auto& param = anim.parameters[static_cast<size_t>(pi)];
                        ImGui::PushID(pi);

                        // 型コンボ（幅を絞る）
                        ImGui::SetNextItemWidth(70.0f);
                        int typeIdx = static_cast<int>(param.type);
                        if (ImGui::Combo("##ptype", &typeIdx, kParamTypes, 4))
                            param.type = static_cast<scene::ParamType>(typeIdx);
                        ImGui::SameLine();

                        // 名前入力
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "%s", param.name.c_str());
                        ImGui::SetNextItemWidth(100.0f);
                        if (ImGui::InputText("##pname", buf, sizeof(buf)))
                            param.name = buf;
                        ImGui::SameLine();

                        // 値ウィジェット
                        switch (param.type) {
                        case scene::ParamType::Float:
                            ImGui::SetNextItemWidth(80.0f);
                            ImGui::DragFloat("##pval", &param.floatValue, 0.01f);
                            break;
                        case scene::ParamType::Int:
                            ImGui::SetNextItemWidth(80.0f);
                            ImGui::DragInt("##pval", &param.intValue);
                            break;
                        case scene::ParamType::Bool:
                            ImGui::Checkbox("##pval", &param.boolValue);
                            break;
                        case scene::ParamType::Trigger:
                            if (ImGui::SmallButton("Fire"))
                                param.boolValue = true;
                            break;
                        }
                        ImGui::SameLine();

                        if (ImGui::SmallButton("x"))
                            removeParamIdx = pi;

                        ImGui::PopID();
                    }
                    if (removeParamIdx >= 0)
                        anim.parameters.erase(anim.parameters.begin() + removeParamIdx);

                    // "+ Add Parameter" ボタン（型コンボ付き）
                    static int s_newParamType = 0;
                    ImGui::SetNextItemWidth(70.0f);
                    ImGui::Combo("##newptype", &s_newParamType, kParamTypes, 4);
                    ImGui::SameLine();
                    if (ImGui::Button("+ Add Parameter")) {
                        scene::AnimatorParameter p;
                        p.name = "NewParam";
                        p.type = static_cast<scene::ParamType>(s_newParamType);
                        anim.parameters.push_back(std::move(p));
                    }
                }

                // ── States ────────────────────────────────────────────────────
                ImGui::Separator();
                if (ImGui::CollapsingHeader("States")) {
                    // 利用可能なクリップ名リスト（Clip コンボ用）
                    std::vector<const char*> clipNames;
                    clipNames.push_back("(none)");
                    for (const auto& c : anim.clips)
                        clipNames.push_back(c.name.c_str());

                    // 利用可能なステート名リスト（遷移先コンボ用）
                    std::vector<const char*> stateNamesForTrans;
                    for (const auto& s : anim.states)
                        stateNamesForTrans.push_back(s.name.c_str());

                    static const char* kOpNames[] = {
                        "Greater", "Less", "Equal", "NotEqual", "True", "False"
                    };

                    int removeStateIdx = -1;
                    for (int si = 0; si < static_cast<int>(anim.states.size()); ++si) {
                        auto& st = anim.states[static_cast<size_t>(si)];
                        ImGui::PushID(si);

                        const bool isCurrent = (st.name == anim.currentStateName);
                        if (isCurrent)
                            ImGui::PushStyleColor(ImGuiCol_Header, { 0.3f, 0.6f, 0.3f, 1.0f });

                        const bool open = ImGui::CollapsingHeader(st.name.c_str());

                        if (isCurrent) ImGui::PopStyleColor();

                        if (open) {
                            ImGui::Indent();

                            // State 名入力
                            char nameBuf[64];
                            std::snprintf(nameBuf, sizeof(nameBuf), "%s", st.name.c_str());
                            if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                                // defaultStateName / currentStateName も追随して更新する
                                if (anim.defaultStateName == st.name)
                                    anim.defaultStateName = nameBuf;
                                if (anim.currentStateName == st.name)
                                    anim.currentStateName = nameBuf;
                                st.name = nameBuf;
                            }

                            // Clip コンボ
                            int clipSel = 0;
                            for (int ci = 1; ci < static_cast<int>(clipNames.size()); ++ci)
                                if (st.clipName == clipNames[static_cast<size_t>(ci)])
                                    { clipSel = ci; break; }
                            if (ImGui::Combo("Clip", &clipSel,
                                             clipNames.data(),
                                             static_cast<int>(clipNames.size()))) {
                                st.clipName = (clipSel == 0)
                                    ? ""
                                    : clipNames[static_cast<size_t>(clipSel)];
                            }
                            ImGui::DragFloat("Speed##st", &st.speed, 0.01f, -10.0f, 10.0f);
                            ImGui::Checkbox("Loop##st", &st.loop);

                            // ── Transitions ──────────────────────────────────
                            ImGui::Separator();
                            ImGui::Text("Transitions");
                            int removeTrIdx = -1;
                            for (int ti = 0; ti < static_cast<int>(st.transitions.size()); ++ti) {
                                auto& tr = st.transitions[static_cast<size_t>(ti)];
                                ImGui::PushID(ti);

                                // 遷移先コンボ
                                int toIdx = 0;
                                for (int xi = 0; xi < static_cast<int>(stateNamesForTrans.size()); ++xi)
                                    if (tr.toStateName == stateNamesForTrans[static_cast<size_t>(xi)])
                                        { toIdx = xi; break; }
                                ImGui::SetNextItemWidth(120.0f);
                                if (ImGui::Combo("->##to", &toIdx,
                                                 stateNamesForTrans.data(),
                                                 static_cast<int>(stateNamesForTrans.size())))
                                    tr.toStateName = stateNamesForTrans[static_cast<size_t>(toIdx)];

                                ImGui::SameLine();
                                ImGui::Checkbox("ExitTime", &tr.hasExitTime);
                                if (tr.hasExitTime) {
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(60.0f);
                                    ImGui::DragFloat("##et", &tr.exitTime, 0.01f, 0.0f, 1.0f);
                                }
                                ImGui::SetNextItemWidth(80.0f);
                                ImGui::DragFloat("Duration", &tr.transitionDuration, 0.01f, 0.0f, 5.0f);

                                // 条件リスト
                                ImGui::Indent();
                                int removeCondIdx = -1;
                                for (int ci = 0; ci < static_cast<int>(tr.conditions.size()); ++ci) {
                                    auto& cond = tr.conditions[static_cast<size_t>(ci)];
                                    ImGui::PushID(ci);

                                    // パラメーター名コンボ
                                    std::vector<const char*> paramNamesList;
                                    for (const auto& pp : anim.parameters)
                                        paramNamesList.push_back(pp.name.c_str());
                                    int pIdx = 0;
                                    for (int xi = 0; xi < static_cast<int>(paramNamesList.size()); ++xi)
                                        if (cond.paramName == paramNamesList[static_cast<size_t>(xi)])
                                            { pIdx = xi; break; }
                                    ImGui::SetNextItemWidth(90.0f);
                                    if (!paramNamesList.empty() &&
                                        ImGui::Combo("##cp", &pIdx,
                                                     paramNamesList.data(),
                                                     static_cast<int>(paramNamesList.size())))
                                        cond.paramName = paramNamesList[static_cast<size_t>(pIdx)];
                                    ImGui::SameLine();

                                    // 演算子コンボ
                                    int opIdx = static_cast<int>(cond.op);
                                    ImGui::SetNextItemWidth(70.0f);
                                    if (ImGui::Combo("##cop", &opIdx, kOpNames, 6))
                                        cond.op = static_cast<scene::ConditionOp>(opIdx);
                                    ImGui::SameLine();

                                    // 閾値（Greater/Less/Equal/NotEqual のとき表示）
                                    if (opIdx < 4) {
                                        ImGui::SetNextItemWidth(60.0f);
                                        ImGui::DragFloat("##cth", &cond.threshold, 0.01f);
                                        ImGui::SameLine();
                                    }
                                    if (ImGui::SmallButton("x##cond"))
                                        removeCondIdx = ci;

                                    ImGui::PopID();
                                }
                                if (removeCondIdx >= 0)
                                    tr.conditions.erase(tr.conditions.begin() + removeCondIdx);

                                if (ImGui::SmallButton("+ Condition")) {
                                    scene::AnimatorCondition c;
                                    if (!anim.parameters.empty())
                                        c.paramName = anim.parameters[0].name;
                                    tr.conditions.push_back(std::move(c));
                                }
                                ImGui::Unindent();

                                ImGui::SameLine();
                                if (ImGui::SmallButton("x##tr"))
                                    removeTrIdx = ti;

                                ImGui::PopID();
                            }
                            if (removeTrIdx >= 0)
                                st.transitions.erase(st.transitions.begin() + removeTrIdx);

                            if (ImGui::Button("+ Add Transition")) {
                                scene::AnimationTransition tr;
                                if (!anim.states.empty())
                                    tr.toStateName = anim.states[0].name;
                                st.transitions.push_back(std::move(tr));
                            }

                            ImGui::Separator();
                            if (ImGui::SmallButton("Remove State"))
                                removeStateIdx = si;

                            ImGui::Unindent();
                        }
                        ImGui::PopID();
                    }
                    if (removeStateIdx >= 0)
                        anim.states.erase(anim.states.begin() + removeStateIdx);

                    if (ImGui::Button("+ Add State")) {
                        scene::AnimationState newSt;
                        newSt.name = "NewState";
                        if (anim.defaultStateName.empty())
                            anim.defaultStateName = newSt.name;
                        anim.states.push_back(std::move(newSt));
                    }
                }
            }
        });

    // -----------------------------------------------------------------------
    // IK Solver
    // IKChain は可変長配列のため IReflector では表現できず、直接 ImGui で描画する。
    // -----------------------------------------------------------------------
    DrawComponentSection<scene::IKSolverComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "IK Solver",
        [go](scene::IKSolverComponent& ik, EditorContext& ctx) {

            // ── Hip Height Correction ────────────────────────────────────────
            // WHY: チェーンより上位にある設定のため最初に表示し、設定忘れを防ぐ。
            {
                char hipBuf[256];
                std::snprintf(hipBuf, sizeof(hipBuf), "%s", ik.hipBoneName.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Clear").x - 24.0f);
                if (ImGui::InputText("Hip Bone", hipBuf, sizeof(hipBuf)))
                    ik.hipBoneName = hipBuf;
                if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                    ik.hipBoneName = dropped->name;
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear"))
                    ik.hipBoneName.clear();
                if (!ik.hipBoneName.empty())
                    ImGui::TextDisabled("  Hip height correction enabled");
            }

            ImGui::Separator();

            // ── IK Chains ────────────────────────────────────────────────────
            int removeIdx = -1;

            for (int ci = 0; ci < static_cast<int>(ik.chains.size()); ++ci) {
                auto& chain = ik.chains[static_cast<size_t>(ci)];
                ImGui::PushID(ci);

                // ヘッダー行: [▶] [✓] Chain 0  (TipBone)           [Remove]
                // WHY: Unity の Constraint コンポーネントと同様に enabled を
                //      折りたたみ矢印の横に置き、開かずに ON/OFF できるようにする。
                const char* tipLabel = chain.tipBoneName.empty() ? "—" : chain.tipBoneName.c_str();
                char header[64];
                std::snprintf(header, sizeof(header), "##chain%d", ci);

                const float removeW   = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                const float checkboxW = ImGui::GetFrameHeight();

                bool open = ImGui::TreeNodeEx(header,
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap,
                    "Chain %d  (%s)", ci, tipLabel);

                ImGui::SameLine(ImGui::GetContentRegionMax().x - removeW - checkboxW
                                - ImGui::GetStyle().ItemSpacing.x);
                ImGui::Checkbox("##en", &chain.enabled);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable / Disable this chain");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9f, 0.10f, 0.10f, 1.0f));
                if (ImGui::SmallButton("Remove")) removeIdx = ci;
                ImGui::PopStyleColor(3);

                if (open) {
                    // 無効チェーンは薄く表示
                    if (!chain.enabled)
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);

                    // ── Bones ─────────────────────────────────────────────────
                    ImGui::SeparatorText("Bones");
                    ImGui::TextDisabled("Drag from Hierarchy or type name");
                    {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "%s", chain.rootBoneName.c_str());
                        if (ImGui::InputText("Root", buf, sizeof(buf)))
                            chain.rootBoneName = buf;
                        if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                            chain.rootBoneName = dropped->name;

                        std::snprintf(buf, sizeof(buf), "%s", chain.midBoneName.c_str());
                        if (ImGui::InputText("Mid",  buf, sizeof(buf)))
                            chain.midBoneName = buf;
                        if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                            chain.midBoneName = dropped->name;

                        std::snprintf(buf, sizeof(buf), "%s", chain.tipBoneName.c_str());
                        if (ImGui::InputText("Tip",  buf, sizeof(buf)))
                            chain.tipBoneName = buf;
                        if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene))
                            chain.tipBoneName = dropped->name;
                    }

                    // ── Targets ───────────────────────────────────────────────
                    // WHY: Target/Pole は名前文字列で保持し、Resolve ボタンで EntityID を
                    //      解決する。解決状態を色付きドットで即座に確認できる。
                    ImGui::SeparatorText("Targets");
                    {
                        char buf[256];
                        const float resolveW = ImGui::CalcTextSize("Resolve").x
                                             + ImGui::GetStyle().FramePadding.x * 2.0f;
                        const float dotW     = ImGui::GetFrameHeight();

                        // WHY: guid を追加することでリネーム後も参照が壊れなくなる。
                        //      手入力時は guid をクリアし名前フォールバックで解決させる。
                        //      ドロップ・Resolve 時は dropped/found の instanceId を記録する。
                        auto DrawObjectField = [&](const char* label,
                                                   const char* idStr,
                                                   std::string& name,
                                                   std::string& guid,
                                                   scene::EntityID& eid)
                        {
                            const bool resolved = eid.IsValid();
                            // 解決状態ドット (緑=OK / 赤=未解決)
                            const ImVec4 dotColor = resolved
                                ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                                : ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
                            ImGui::TextColored(dotColor, resolved ? "●" : "○");
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(resolved ? "Resolved" : "Not resolved — click Resolve");
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(
                                ImGui::GetContentRegionAvail().x - resolveW
                                - ImGui::GetStyle().ItemSpacing.x);
                            std::snprintf(buf, sizeof(buf), "%s", name.c_str());
                            if (ImGui::InputText(idStr, buf, sizeof(buf))) {
                                name = buf;
                                guid.clear(); // 手入力時は GUID をクリアして名前で再解決させる
                            }
                            if (auto* dropped = AcceptHierarchyDrop(ctx.activeScene)) {
                                name = dropped->name;
                                guid = dropped->instanceId;
                                eid  = dropped->GetID();
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton(label)) {
                                if (ctx.activeScene) {
                                    auto* found = ctx.activeScene->Find(name);
                                    eid  = found ? found->GetID()    : scene::EntityID::INVALID;
                                    guid = found ? found->instanceId : std::string{};
                                }
                            }
                        };

                        DrawObjectField("Resolve##t", "##tgt",  chain.targetName, chain.targetGuid, chain.targetEntity);
                        ImGui::SameLine();
                        ImGui::TextUnformatted("Target");

                        DrawObjectField("Resolve##p", "##pole", chain.poleName,   chain.poleGuid,   chain.poleEntity);
                        ImGui::SameLine();
                        ImGui::TextUnformatted("Pole");
                    }

                    // ── Settings ──────────────────────────────────────────────
                    ImGui::SeparatorText("Settings");
                    ImGui::DragFloat("Weight",        &chain.weight,       0.01f,  0.0f, 1.0f, "%.2f");
                    ImGui::DragFloat("Max Extension", &chain.maxExtension, 0.005f, 0.5f, 1.0f, "%.3f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Limits how far the chain can stretch (ratio of total bone length)");
                    ImGui::DragFloat("Softness",      &chain.softness,     0.005f, 0.0f, 0.5f, "%.3f");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Exponential ease-out before max extension (0 = off)");
                    ImGui::Checkbox("Is Leg", &chain.isLeg);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Include in Hip height correction (requires Hip Bone set above)");

                    // ── Ground Snap ───────────────────────────────────────────
                    // WHY: Ground Snap は足 IK 専用の機能群なので独立したセクションに集約する。
                    //      targetOffset は Ground Snap 時は footSurfaceOffset に統合済みのため非表示。
                    ImGui::SeparatorText("Ground Snap");
                    ImGui::Checkbox("Enable##gs", &chain.useGroundSnap);
                    if (chain.useGroundSnap) {
                        ImGui::DragFloat("Ray Up Ratio",   &chain.rayUpRatio,        0.01f,  0.1f, 2.0f, "%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Ray starts this many * leg-length above the FK foot position");
                        ImGui::DragFloat("Ray Down Ratio", &chain.rayDownRatio,      0.01f,  0.5f, 4.0f, "%.2f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Total downward ray length in leg-length multiples");
                        ImGui::DragFloat("Surface Offset", &chain.footSurfaceOffset, 0.001f, 0.0f, 0.5f, "%.3f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Distance to lift the ankle above the ground hit point");
                        // 斜面での足首傾き補正 (Ground Snap と一体で使うため同セクション)
                        widgets::DragVec3("Foot Normal Axis", chain.footNormalAxis, 0.01f, -1.0f, 1.0f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Bone-local axis pointing through the foot sole.\n"
                                              "Mixamo: (0,-1,0)   Blender Z-up: (0,0,-1)\n"
                                              "Zero = no slope tilt correction");
                    } else {
                        // Ground Snap OFF のときのみ targetOffset が IK ゴールに加算される
                        widgets::DragVec3("Target Offset", chain.targetOffset, 0.001f, 0.0f, 0.0f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("World-space offset added to the target position\n"
                                              "(hidden when Ground Snap is ON — use Surface Offset instead)");
                    }

                    if (!chain.enabled)
                        ImGui::PopStyleVar();

                    ImGui::TreePop();
                }

                ImGui::PopID();
                ImGui::Spacing();
            }

            // チェーン削除
            if (removeIdx >= 0)
                ik.chains.erase(ik.chains.begin() + removeIdx);

            // チェーン追加
            if (ImGui::Button("+ Add Chain", { -1.0f, 0.0f })) {
                scene::IKChain chain;
                chain.enabled = true;
                ik.chains.push_back(std::move(chain));
            }
        });

    DrawComponentSection<scene::MaterialComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Material",
        [](scene::MaterialComponent& mc, EditorContext&) {

            // テクスチャパス入力 + ドロップターゲット共通ヘルパー
            auto texField = [](const char* label, std::string& path) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                if (ImGui::InputText(label, buf, sizeof(buf)))
                    path = NormalizeAssetPath(buf);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    }
                    ImGui::EndDragDropTarget();
                }
            };

            // ShaderVarDesc 1 変数分の ImGui ウィジェットを描画し paramData を直接編集する。
            // 命名規則でウィジェット種別を決定する (メタコメント不要)。
            auto drawShaderVar = [](std::vector<uint8_t>& data,
                                    const renderer::ShaderVarDesc& v)
            {
                if (v.varType != renderer::ShaderVarType::Float) return;
                if (v.offset + v.size > static_cast<uint32_t>(data.size())) return;
                float* ptr = reinterpret_cast<float*>(data.data() + v.offset);

                // 小文字化した名前で色かどうかを判定する
                std::string lc = v.name;
                for (auto& c : lc) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const bool isColor = lc.find("color")   != std::string::npos
                                  || lc.find("albedo")  != std::string::npos
                                  || lc.find("emissive")!= std::string::npos;

                switch (v.columns) {
                case 1: {
                    const bool isNorm = lc.find("metallic")  != std::string::npos
                                     || lc.find("roughness") != std::string::npos
                                     || lc.find("strength")  != std::string::npos
                                     || lc.find("cutoff")    != std::string::npos
                                     || lc.find("occlusion") != std::string::npos;
                    if (isNorm)
                        ImGui::SliderFloat(v.name.c_str(), ptr, 0.0f, 1.0f);
                    else
                        ImGui::DragFloat(v.name.c_str(), ptr, 0.01f);
                    break;
                }
                case 2: ImGui::DragFloat2(v.name.c_str(), ptr, 0.01f); break;
                case 3:
                    if (isColor) ImGui::ColorEdit3(v.name.c_str(), ptr);
                    else         ImGui::DragFloat3(v.name.c_str(), ptr, 0.01f);
                    break;
                case 4:
                    if (isColor) ImGui::ColorEdit4(v.name.c_str(), ptr);
                    else         ImGui::DragFloat4(v.name.c_str(), ptr, 0.01f);
                    break;
                default: break;
                }
            };

            // ── Rendering ───────────────────────────────────────────────────
            ImGui::SeparatorText("Rendering");
            {
                // Blend Mode
                static const char* blendLabels[] = { "Opaque", "Alpha Blend", "Additive" };
                int blendIdx = static_cast<int>(mc.blendMode);
                if (ImGui::Combo("Blend Mode", &blendIdx, blendLabels, 3))
                    mc.blendMode = static_cast<renderer::BlendMode>(blendIdx);

                // Double Sided
                ImGui::Checkbox("Double Sided", &mc.doubleSided);

                // Render Queue
                // WHY: Unity と同じ数値帯を Inspector でも明示し、Custom VFX 用の差し込み値だけ手入力にする。
                static const char* queueLabels[] = {
                    "Background (1000)",
                    "Geometry (2000)",
                    "Alpha Test (2450)",
                    "Transparent (3000)",
                    "Custom",
                    "Overlay (4000)"
                };
                static const int32_t queueValues[] = {
                    renderer::RenderQueue::BACKGROUND,
                    renderer::RenderQueue::GEOMETRY,
                    renderer::RenderQueue::ALPHA_TEST,
                    renderer::RenderQueue::TRANSPARENT_QUEUE,
                    renderer::RenderQueue::CUSTOM,
                    renderer::RenderQueue::OVERLAY
                };
                int queuePreset = 4;
                for (int i = 0; i < 6; ++i) {
                    if (mc.renderQueue == queueValues[i]) {
                        queuePreset = i;
                        break;
                    }
                }
                if (ImGui::Combo("Render Queue", &queuePreset, queueLabels, 6))
                    mc.renderQueue = queueValues[queuePreset];
                if (queuePreset == 4)
                    ImGui::DragInt("Custom Queue", &mc.renderQueue, 1.0f,
                        renderer::RenderQueue::CUSTOM_MIN, renderer::RenderQueue::CUSTOM_MAX);
                ImGui::TextDisabled("Lower values draw first. Custom: 3100-3999.");
            }

            // ── Shader ──────────────────────────────────────────────────────
            ImGui::SeparatorText("Shader");
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", mc.shaderPath.c_str());
                if (ImGui::InputText("Shader (HLSL)", buf, sizeof(buf)))
                    mc.shaderPath = NormalizeAssetPath(buf);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        mc.shaderPath = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    }
                    ImGui::EndDragDropTarget();
                }
            }

            // Descriptor 取得 (シェーダー未ロード時は nullptr)
            const renderer::ShaderDescriptor* desc = nullptr;
            if (mc.material && mc.material->shader.IsValid())
                if (auto* res = renderer::ResourceManager::Active())
                    if (auto* sh = res->Get(mc.material->shader))
                        desc = &sh->GetDescriptor();

            // Descriptor に合わせて paramData・texturePaths を初期化
            if (desc && mc.paramData.size() != desc->cbufferSize)
                mc.InitFromDescriptor(*desc);

            // ── Textures ────────────────────────────────────────────────────
            ImGui::SeparatorText("Textures");
            if (desc && !desc->textures.empty())
            {
                for (const auto& t : desc->textures)
                {
                    // スロット番号とテクスチャ名をラベルに使う
                    std::string label = t.name + "  (t" + std::to_string(t.slot) + ")";
                    if (t.slot < mc.texturePaths.size())
                        texField(label.c_str(), mc.texturePaths[t.slot]);
                }
            }
            else
            {
                // シェーダー未ロード時は汎用スロットを表示
                mc.texturePaths.resize(5);
                texField("Albedo  (t0)",        mc.texturePaths[0]);
                texField("Normal  (t1)",        mc.texturePaths[1]);
                texField("MetalRough (t2)",     mc.texturePaths[2]);
                texField("Emissive  (t3)",      mc.texturePaths[3]);
                texField("AO  (t4)",            mc.texturePaths[4]);
            }

            // ── Parameters (Descriptor 駆動) ────────────────────────────────
            if (desc && !desc->vars.empty())
            {
                ImGui::SeparatorText("Parameters");
                for (const auto& v : desc->vars)
                    drawShaderVar(mc.paramData, v);
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

    DrawComponentSection<scene::DecalComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Decal",
        [](scene::DecalComponent& dc, EditorContext&) {

            auto texField = [](const char* label, std::string& path) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                if (ImGui::InputText(label, buf, sizeof(buf)))
                    path = NormalizeAssetPath(buf);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    }
                    ImGui::EndDragDropTarget();
                }
            };

            ImGui::SeparatorText("Textures");
            texField("Albedo (t0)",   dc.albedoTexPath);
            texField("Normal (t1)",   dc.normalTexPath);
            texField("Emissive (t3)", dc.emissiveTexPath);

            ImGui::SeparatorText("Surface");
            ImGui::ColorEdit4("Albedo Color",     dc.albedoColor);
            ImGui::SliderFloat("Normal Strength", &dc.normalStrength, 0.0f, 2.0f);

            ImGui::SeparatorText("Emissive");
            ImGui::ColorEdit3("Emissive Color", dc.emissiveColor);
            ImGui::DragFloat("Emissive Scale",  &dc.emissiveScale, 0.01f, 0.0f, 100.0f);

            ImGui::SeparatorText("Lifetime");
            ImGui::DragFloat("Lifetime (s)",  &dc.lifetime, 0.1f, -1.0f, 3600.0f, dc.lifetime < 0.0f ? "Permanent" : "%.1f s");
            ImGui::DragFloat("Fade Time (s)", &dc.fadeTime, 0.05f, 0.0f, 60.0f);
            ImGui::BeginDisabled();
            ImGui::DragFloat("Age (s)", &dc.age, 0.0f, 0.0f, 0.0f, "%.2f s");
            ImGui::EndDisabled();

            ImGui::SeparatorText("Receiver Layer Mask");
            // ビット 0〜7 を個別チェックボックスで表示。残りは hex 入力で直接編集。
            static constexpr const char* kLayerNames[] = {
                "Default", "TransparentFX", "Ignore Raycast", "User Layer 3",
                "Water",   "UI",            "User Layer 6",   "User Layer 7"
            };
            for (int i = 0; i < 8; ++i) {
                bool checked = (dc.receiverLayerMask & (1u << i)) != 0;
                if (ImGui::Checkbox(kLayerNames[i], &checked)) {
                    if (checked) dc.receiverLayerMask |=  (1u << i);
                    else         dc.receiverLayerMask &= ~(1u << i);
                }
                if (i % 2 == 0) ImGui::SameLine(160.0f);
            }
            ImGui::InputScalar("Mask (hex)", ImGuiDataType_U32, &dc.receiverLayerMask,
                               nullptr, nullptr, "%08X",
                               ImGuiInputTextFlags_CharsHexadecimal);
        });

    DrawComponentSection<scene::UICanvas>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Canvas",
        [go](scene::UICanvas& canvas, EditorContext& ctx) {
            if (go->GetParent())
                ImGui::TextDisabled("Only root GameObjects are rendered as canvases.");
            ImGui::DragFloat("Canvas Width",  &canvas.canvasWidth,  1.0f, 1.0f, 16384.0f);
            ImGui::DragFloat("Canvas Height", &canvas.canvasHeight, 1.0f, 1.0f, 16384.0f);
            ImGui::DragInt("Sort Order", &canvas.sortOrder);
            static constexpr const char* kModeNames[] = {
                "Screen Space Overlay",
                "World Space",
                "Screen Space Camera"
            };
            int modeIdx = static_cast<int>(canvas.renderMode);
            if (ImGui::Combo("Render Mode", &modeIdx, kModeNames, 3))
                canvas.renderMode = static_cast<scene::UIRenderMode>(modeIdx);

            if (canvas.renderMode != scene::UIRenderMode::WorldSpace) {
                static constexpr const char* kScaleNames[] = {
                    "Constant Pixel Size",
                    "Scale With Screen Size"
                };
                int scaleIdx = static_cast<int>(canvas.scaleMode);
                if (ImGui::Combo("Scale Mode", &scaleIdx, kScaleNames, 2))
                    canvas.scaleMode = static_cast<scene::UICanvasScaleMode>(scaleIdx);
                if (canvas.scaleMode == scene::UICanvasScaleMode::ScaleWithScreenSize) {
                    ImGui::DragFloat("Reference Width",  &canvas.referenceWidth,  1.0f, 1.0f, 16384.0f);
                    ImGui::DragFloat("Reference Height", &canvas.referenceHeight, 1.0f, 1.0f, 16384.0f);
                    ImGui::SliderFloat("Match Width/Height", &canvas.matchWidthOrHeight, 0.0f, 1.0f);
                }
                if (ImGui::Button("Set Active UI Canvas"))
                    ctx.activeUICanvas = go->GetID();
            }

            if (canvas.renderMode == scene::UIRenderMode::WorldSpace)
                ImGui::DragFloat("World Scale", &canvas.worldScale, 0.0001f, 0.00001f, 1.0f, "%.5f");
        });

    DrawComponentSection<scene::UIImage>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Image",
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
                image.texturePath = NormalizeAssetPath(texBuf);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    image.texturePath = NormalizeAssetPath(static_cast<const char*>(p->Data));
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

    DrawComponentSection<scene::TerrainComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain",
        [go](scene::TerrainComponent& tc, EditorContext& ctx) {
            // ── 外部 Terrain Asset ─────────────────────────────────────────────
            ImGui::SeparatorText("Asset");
            {
                char pathBuf[512];
                std::snprintf(pathBuf, sizeof(pathBuf), "%s", tc.terrainAssetPath.c_str());
                if (ImGui::InputText("Asset Path", pathBuf, sizeof(pathBuf)))
                    tc.terrainAssetPath = NormalizeAssetPath(pathBuf);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        std::string path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                        if (util::FileSystem::GetExtension(path) == ".fbzzterrain")
                            tc.terrainAssetPath = path;
                    }
                    ImGui::EndDragDropTarget();
                }

                if (tc.terrainAssetPath.empty()) {
                    if (ImGui::Button("Create Terrain Asset")) {
                        const std::string path = UniqueTerrainAssetPath(ctx, go ? go->name : "Terrain");
                        if (scene::TerrainAssetSerializer::Save(tc, TerrainAssetDiskPath(ctx, path))) {
                            tc.terrainAssetPath = path;
                            ctx.requestAssetBrowserRefresh = true;
                        }
                    }
                } else {
                    if (ImGui::Button("Save Asset")) {
                        scene::TerrainAssetSerializer::Save(tc, TerrainAssetDiskPath(ctx, tc.terrainAssetPath));
                        ctx.requestAssetBrowserRefresh = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Load Asset")) {
                        const bool enabled = tc.enabled;
                        const std::string path = tc.terrainAssetPath;
                        if (scene::TerrainAssetSerializer::Load(TerrainAssetDiskPath(ctx, path), tc)) {
                            tc.enabled = enabled;
                            tc.terrainAssetPath = path;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Unlink")) {
                        tc.terrainAssetPath.clear();
                    }
                }
            }

            // ── グリッド設定 ──────────────────────────────────────────────────
            ImGui::SeparatorText("Grid");
            if (ImGui::DragInt("Columns",    &tc.columns,   1.0f, 2, 4097))
                tc.heightDirty = true;
            if (ImGui::DragInt("Rows",       &tc.rows,      1.0f, 2, 4097))
                tc.heightDirty = true;
            if (ImGui::DragFloat("Cell Size",   &tc.cellSize,  0.01f, 0.01f, 100.0f))
                tc.heightDirty = true;
            if (ImGui::DragFloat("Max Height",  &tc.maxHeight, 0.5f, 0.5f, 1000.0f))
                tc.heightDirty = true;
            ImGui::DragInt("Chunk Size", &tc.chunkSize, 1.0f, 8, 256);

            // ── テクスチャレイヤー ─────────────────────────────────────────────
            ImGui::SeparatorText("Layers");
            for (int i = 0; i < static_cast<int>(tc.layers.size()); ++i) {
                auto& layer = tc.layers[static_cast<size_t>(i)];
                ImGui::PushID(i);
                const bool layerOpen = ImGui::TreeNodeEx(
                    "LayerHeader",
                    ImGuiTreeNodeFlags_DefaultOpen,
                    "Layer %d", i);

                if (layerOpen) {
                // Diffuse パス (ドラッグ&ドロップ対応)
                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", layer.diffusePath.c_str());
                    if (ImGui::InputText("Diffuse##d", buf, sizeof(buf)))
                        layer.diffusePath = NormalizeAssetPath(buf);
                    if (ImGui::IsItemDeactivatedAfterEdit()) tc.splatDirty = true;
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            layer.diffusePath = NormalizeAssetPath(static_cast<const char*>(p->Data));
                            tc.splatDirty = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
                // Normal パス
                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", layer.normalPath.c_str());
                    if (ImGui::InputText("Normal##n", buf, sizeof(buf)))
                        layer.normalPath = NormalizeAssetPath(buf);
                    if (ImGui::IsItemDeactivatedAfterEdit()) tc.splatDirty = true;
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            layer.normalPath = NormalizeAssetPath(static_cast<const char*>(p->Data));
                            tc.splatDirty = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
                // AO/Roughness パス
                // R=AO, G=Roughness の packed texture。未設定時は下の数値パラメータを使う。
                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", layer.aoRoughnessPath.c_str());
                    if (ImGui::InputText("AO Roughness##ar", buf, sizeof(buf)))
                        layer.aoRoughnessPath = NormalizeAssetPath(buf);
                    if (ImGui::IsItemDeactivatedAfterEdit()) tc.splatDirty = true;
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            layer.aoRoughnessPath = NormalizeAssetPath(static_cast<const char*>(p->Data));
                            tc.splatDirty = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
                ImGui::DragFloat("Tiling X##tx", &layer.tilingX, 0.1f, 0.1f, 100.0f);
                ImGui::DragFloat("Tiling Z##tz", &layer.tilingZ, 0.1f, 0.1f, 100.0f);
                ImGui::DragFloat("Normal Str##ns", &layer.normalStrength, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("Roughness##rough", &layer.roughness, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("AO##ao", &layer.ambientOcclusion, 0.01f, 0.0f, 1.0f);
                if (ImGui::TreeNodeEx("Auto Blend##auto", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox("Enable##abe", &layer.autoBlendEnabled);
                    ImGui::DragFloat("Strength##abs", &layer.autoBlendStrength, 0.01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Min Height##abhmin", &layer.autoMinHeight, 0.1f, -10000.0f, 10000.0f);
                    ImGui::DragFloat("Max Height##abhmax", &layer.autoMaxHeight, 0.1f, -10000.0f, 10000.0f);
                    ImGui::DragFloat("Height Fade##abhf", &layer.autoHeightFade, 0.05f, 0.001f, 1000.0f);
                    ImGui::DragFloat("Min Slope##absmin", &layer.autoMinSlope, 0.01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Max Slope##absmax", &layer.autoMaxSlope, 0.01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Slope Fade##absf", &layer.autoSlopeFade, 0.01f, 0.001f, 1.0f);
                    ImGui::TreePop();
                }

                if (ImGui::SmallButton("Remove##rm")) {
                    tc.layers.erase(tc.layers.begin() + i);
                    tc.splatDirty = true;
                    ImGui::TreePop();
                    ImGui::PopID();
                    break; // イテレーション中の削除なのでループを抜ける
                }
                ImGui::Separator();
                ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (static_cast<int>(tc.layers.size()) < 4) {
                if (ImGui::Button("+ Add Layer")) {
                    tc.layers.emplace_back();
                    tc.splatDirty = true;
                }
            }

            // ── コライダー ─────────────────────────────────────────────────────
            ImGui::SeparatorText("Collider");
            if (ImGui::Button("Rebuild Collider Now")) {
                tc.colliderDirty = true;
            }

            // ── デバッグ情報 ───────────────────────────────────────────────────
            ImGui::SeparatorText("Info");
            ImGui::Text("Vertices : %d", tc.columns * tc.rows);
            ImGui::Text("Triangles: %d", (tc.columns - 1) * (tc.rows - 1) * 2);
            ImGui::TextColored(tc.heightDirty   ? ImVec4{1,0.5f,0.2f,1} : ImVec4{0.5f,1,0.5f,1},
                               "Height Dirty : %s", tc.heightDirty   ? "Yes" : "No");
            ImGui::TextColored(tc.colliderDirty ? ImVec4{1,0.5f,0.2f,1} : ImVec4{0.5f,1,0.5f,1},
                               "Collider Dirty: %s", tc.colliderDirty ? "Yes" : "No");
        });

    DrawComponentSection<scene::WaterComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Water",
        [go](scene::WaterComponent& water, EditorContext& ctx) {
            // ── Asset 管理セクション ────────────────────────────────────────────
            // WHY: TerrainComponent と同様に .fbzzwater への外部化と
            //      Create / Save / Load / Unlink の操作を Inspector から行えるようにする。
            ImGui::SeparatorText("Asset (.fbzzwater)");

            // アセットパス入力（ドラッグ&ドロップ対応）
            char assetBuf[256];
            std::snprintf(assetBuf, sizeof(assetBuf), "%s", water.waterAssetPath.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f);
            if (ImGui::InputText("##water_asset_path", assetBuf, sizeof(assetBuf)))
                water.waterAssetPath = NormalizeAssetPath(assetBuf);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    if (dropped.size() > 10 &&
                        dropped.substr(dropped.size() - 10) == ".fbzzwater")
                    {
                        water.waterAssetPath = dropped;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (water.waterAssetPath.empty()) {
                // 外部アセット未設定: Create ボタンで新規作成する
                if (ImGui::Button("Create Water Asset")) {
                    const std::string assetPath = UniqueWaterAssetPath(ctx, go ? go->name : "Water");
                    const std::string diskPath  = WaterAssetDiskPath(ctx, assetPath);
                    if (scene::WaterAssetSerializer::Save(water, diskPath)) {
                        water.waterAssetPath = assetPath;
                        ctx.requestAssetBrowserRefresh = true;
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(inline mode)");
            } else {
                // 外部アセット設定済み: Save / Load / Unlink
                if (ImGui::Button("Save")) {
                    scene::WaterAssetSerializer::Save(water, WaterAssetDiskPath(ctx, water.waterAssetPath));
                    ctx.requestAssetBrowserRefresh = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Load")) {
                    scene::WaterAssetSerializer::Load(WaterAssetDiskPath(ctx, water.waterAssetPath), water);
                }
                ImGui::SameLine();
                if (ImGui::Button("Unlink")) {
                    water.waterAssetPath.clear();
                }
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Geometry");
            int resX = static_cast<int>(water.resolutionX);
            int resZ = static_cast<int>(water.resolutionZ);
            if (ImGui::DragInt("Resolution X", &resX, 1.0f, 1, 512)) {
                water.resolutionX = static_cast<uint32_t>(resX);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragInt("Resolution Z", &resZ, 1.0f, 1, 512)) {
                water.resolutionZ = static_cast<uint32_t>(resZ);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragFloat("Extent X", &water.extentX, 0.5f, 0.1f, 10000.0f)) {
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragFloat("Extent Z", &water.extentZ, 0.5f, 0.1f, 10000.0f)) {
                water.meshDirty = true;
                water.foamDirty = true;
            }
            {
                int chunks = static_cast<int>(water.chunkCount);
                if (ImGui::DragInt("Chunk Count", &chunks, 1.0f, 1, 64)) {
                    water.chunkCount = static_cast<uint32_t>(chunks < 1 ? 1 : chunks);
                    water.meshDirty = true;
                }
                ImGui::TextDisabled("(%d x %d chunks = %d draw calls)", chunks, chunks, chunks * chunks);
            }

            ImGui::SeparatorText("Physics");
            if (ImGui::Button("Setup Buoyancy Volume")) {
                // WHY: Water の浮力は Trigger Collider + VolumeComponent の組み合わせで動く。
                //      手作業で 2 Component のサイズと種別を合わせるミスを避けるため、代表的な設定を一括で作る。
                auto* box = go->GetComponent<scene::BoxColliderComponent>();
                if (!box) {
                    box = &go->AddComponent<scene::BoxColliderComponent>();
                }
                box->enabled = true;
                box->isTrigger = true;
                box->center = { 0.0f, -water.deepDepth * 0.5f, 0.0f };
                box->size = {
                    water.extentX,
                    std::max(water.deepDepth, 0.1f),
                    water.extentZ
                };

                auto* volume = go->GetComponent<scene::VolumeComponent>();
                if (!volume) {
                    volume = &go->AddComponent<scene::VolumeComponent>();
                }
                volume->enabled = true;
                volume->type = physics::VolumeType::Buoyancy;
                volume->buoyancy = 15.0f;
                volume->drag = 2.0f;
                volume->duration = -1.0f;
                volume->elapsed = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Sync Collider Size")) {
                if (auto* box = go->GetComponent<scene::BoxColliderComponent>()) {
                    box->isTrigger = true;
                    box->center = { 0.0f, -water.deepDepth * 0.5f, 0.0f };
                    box->size = {
                        water.extentX,
                        std::max(water.deepDepth, 0.1f),
                        water.extentZ
                    };
                }
            }

            ImGui::SeparatorText("Color");
            widgets::ColorEdit3("Shallow Color", water.shallowColor);
            widgets::ColorEdit3("Deep Color", water.deepColor);
            ImGui::DragFloat("Shallow Depth", &water.shallowDepth, 0.05f, 0.01f, 100.0f);
            ImGui::DragFloat("Deep Depth", &water.deepDepth, 0.05f, 0.01f, 1000.0f);
            ImGui::DragFloat("Opacity", &water.opacity, 0.01f, 0.0f, 1.0f);

            ImGui::SeparatorText("Surface");
            ImGui::DragFloat("Reflectivity", &water.reflectivity, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Fresnel Bias", &water.fresnelBias, 0.001f, 0.0f, 1.0f);
            ImGui::DragFloat("Fresnel Power", &water.fresnelPower, 0.1f, 0.1f, 16.0f);
            ImGui::DragFloat("Refraction", &water.refractionStrength, 0.001f, 0.0f, 0.1f);

            ImGui::SeparatorText("Textures");
            auto texturePath = [](const char* label, std::string& path, bool& dirtyFlag) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", path.c_str());
                if (ImGui::InputText(label, buf, sizeof(buf))) {
                    path = NormalizeAssetPath(buf);
                    dirtyFlag = true;
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                        dirtyFlag = true;
                    }
                    ImGui::EndDragDropTarget();
                }
            };
            texturePath("Normal Map 1", water.normalMap1Path, water.texDirty);
            texturePath("Normal Map 2", water.normalMap2Path, water.texDirty);
            texturePath("Foam Texture", water.foamTexPath, water.texDirty);
            texturePath("Flow Map", water.flowMapPath, water.texDirty);
            DragVec2("Normal Scroll 1", water.normalMap1Scroll, 0.001f, -10.0f, 10.0f);
            DragVec2("Normal Scroll 2", water.normalMap2Scroll, 0.001f, -10.0f, 10.0f);
            ImGui::DragFloat("Normal Tiling 1", &water.normalMap1Tiling, 0.1f, 0.01f, 100.0f);
            ImGui::DragFloat("Normal Tiling 2", &water.normalMap2Tiling, 0.1f, 0.01f, 100.0f);
            ImGui::DragFloat("Normal Strength", &water.normalStrength, 0.01f, 0.0f, 5.0f);

            ImGui::SeparatorText("Foam / Flow");
            if (ImGui::DragFloat("Foam Threshold", &water.foamThreshold, 0.01f, -100.0f, 100.0f))
                water.foamDirty = true;
            if (ImGui::DragFloat("Foam Fade", &water.foamFade, 0.01f, 0.001f, 100.0f))
                water.foamDirty = true;
            ImGui::DragFloat("Foam Strength", &water.foamStrength, 0.01f, 0.0f, 5.0f);
            ImGui::DragFloat("Foam Tiling", &water.foamTiling, 0.1f, 0.01f, 100.0f);
            ImGui::Checkbox("Enable Flow Map", &water.enableFlowMap);
            ImGui::DragFloat("Flow Speed", &water.flowSpeed, 0.01f, -10.0f, 10.0f);
            ImGui::DragFloat("Flow Tiling", &water.flowTiling, 0.1f, 0.01f, 100.0f);

            ImGui::SeparatorText("Gerstner Waves");
            ImGui::Checkbox("Enable Waves", &water.enableGerstnerWaves);
            for (int i = 0; i < static_cast<int>(water.waves.size()); ++i) {
                auto& wave = water.waves[static_cast<size_t>(i)];
                ImGui::PushID(i);
                if (ImGui::TreeNodeEx("Wave", ImGuiTreeNodeFlags_DefaultOpen, "Wave %d", i)) {
                    DragVec2("Direction", wave.direction, 0.01f, -1.0f, 1.0f);
                    ImGui::DragFloat("Amplitude", &wave.amplitude, 0.01f, 0.0f, 100.0f);
                    ImGui::DragFloat("Wavelength", &wave.wavelength, 0.1f, 0.01f, 10000.0f);
                    ImGui::DragFloat("Steepness", &wave.steepness, 0.01f, 0.0f, 1.0f);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            ImGui::SeparatorText("Caustics");
            ImGui::Checkbox("Enable Caustics", &water.enableCaustics);
            if (water.enableCaustics) {
                ImGui::DragFloat("Caustics Intensity", &water.causticsIntensity, 0.01f, 0.0f, 5.0f);
                ImGui::DragFloat("Caustics Tiling", &water.causticsTiling, 0.01f, 0.01f, 100.0f);
                ImGui::DragFloat("Caustics Speed", &water.causticsSpeed, 0.001f, 0.0f, 5.0f);
                texturePath("Caustics Texture", water.causticsTexPath, water.texDirty);
                ImGui::TextDisabled("(empty = procedural fallback)");
            }

            ImGui::SeparatorText("Environment");
            texturePath("Env Cubemap", water.envCubemapPath, water.texDirty);
            ImGui::TextDisabled("(empty = no environment reflection)");
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
