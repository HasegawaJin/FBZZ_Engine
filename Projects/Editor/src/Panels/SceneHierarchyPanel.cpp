// FBZZ Engine
// SceneHierarchyPanel.cpp | fbzz::editor
// Scene GameObject hierarchy and selection editing
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/FoliageComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <cstring>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

// ViewportPicking.cpp と同じロジック。
// fzasset を読んでシーンに GO 階層を構築し、root EntityID を返す。
scene::EntityID SpawnFzAssetHierarchy(scene::Scene& scene, const std::string& assetPath)
{
    namespace fs = std::filesystem;

    std::string text;
    if (!util::FileSystem::ReadText(assetPath, text)) return scene::EntityID::INVALID;
    std::istringstream ss(text);
    const auto parsed = toml::parse(ss);
    if (!parsed) return scene::EntityID::INVALID;

    const auto& tbl     = parsed.table();
    const auto* meshArr = tbl["meshes"].as_array();
    const auto* matArr  = tbl["materials"].as_array();
    if (!meshArr || meshArr->empty()) return scene::EntityID::INVALID;

    const int  meshCount = static_cast<int>(meshArr->size());
    const fs::path assetDir =
        util::FileSystem::PathFromUtf8(assetPath).parent_path();
    const std::string stemName =
        util::FileSystem::PathFromUtf8(assetPath).stem().string();

    auto* model = asset::AssetManager::Load<asset::Model>(assetPath);

    std::vector<std::string> matPaths(static_cast<size_t>(meshCount));
    if (matArr) {
        const int n = std::min(meshCount, static_cast<int>(matArr->size()));
        for (int i = 0; i < n; ++i) {
            if (const auto v = (*matArr)[i].value<std::string>())
                matPaths[i] = util::FileSystem::NormalizePathSeparators(
                    util::FileSystem::PathToUtf8(assetDir / *v));
        }
    }

    auto& root = scene.CreateGameObject(stemName);

    auto addChild = [&](int mi) {
        auto& child = scene.CreateGameObject(stemName + "_Mesh" + std::to_string(mi));
        child.SetParent(&root);

        scene::SkinnedMeshRenderer smr;
        smr.modelPath = assetPath;
        smr.meshIndex = mi;
        smr.model     = model;
        child.AddComponent<scene::SkinnedMeshRenderer>(std::move(smr));

        scene::MaterialComponent mc;
        if (mi < static_cast<int>(matPaths.size()))
            mc.materialPath = matPaths[mi];
        child.AddComponent<scene::MaterialComponent>(std::move(mc));
    };

    if (meshCount == 1) {
        scene::SkinnedMeshRenderer smr;
        smr.modelPath = assetPath;
        smr.meshIndex = -1;
        smr.model     = model;
        root.AddComponent<scene::SkinnedMeshRenderer>(std::move(smr));

        scene::MaterialComponent mc;
        if (!matPaths.empty()) mc.materialPath = matPaths[0];
        root.AddComponent<scene::MaterialComponent>(std::move(mc));
    } else {
        for (int i = 0; i < meshCount; ++i)
            addChild(i);
    }

    return root.GetID();
}

void ExecuteSceneEditWithUndo(EditorContext& ctx,
                              const char* description,
                              const std::function<void()>& edit)
{
    if (!ctx.activeScene || !edit) return;

    const bool canRecordUndo =
        ctx.undoStack != nullptr && ctx.undoStack->IsRecordingEnabled();
    if (!canRecordUndo) {
        edit();
        if (ctx.markSceneDirty) ctx.markSceneDirty();
        return;
    }

    const std::string before = SceneIO::Serialize(*ctx.activeScene);
    const std::size_t historyRevisionBefore = ctx.undoStack->GetRevision();
    edit();
    const std::string after = SceneIO::Serialize(*ctx.activeScene);

    // Reparent 等が専用コマンドを追加済みなら、全シーンコマンドとの二重登録を避ける。
    if (before == after ||
        ctx.undoStack->GetRevision() != historyRevisionBefore) {
        if (before != after && ctx.markSceneDirty) ctx.markSceneDirty();
        return;
    }

    scene::Scene* scene = ctx.activeScene;
    EditorContext* context = &ctx;
    const auto markDirty = ctx.markSceneDirty;
    auto restore = [scene, context, markDirty](const std::string& snapshot) {
        if (SceneIO::Deserialize(*scene, snapshot)) {
            context->selectedEntities.clear();
            context->activeUICanvas = {};
            if (markDirty) markDirty();
        }
    };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        description,
        [restore, after]() { restore(after); },
        [restore, before]() { restore(before); }));
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

void SetParentWithUndo(EditorContext& ctx,
                       scene::EntityID childId,
                       scene::EntityID newParentId,
                       const char* description)
{
    if (!ctx.activeScene) return;

    scene::GameObject* child = ctx.activeScene->GetGameObject(childId);
    if (!child) return;

    const scene::GameObject* oldParent = child->GetParent();
    const std::string childInstanceId = child->instanceId;
    const std::string oldParentInstanceId = oldParent ? oldParent->instanceId : std::string{};
    scene::GameObject* newParent = newParentId.IsValid()
        ? ctx.activeScene->GetGameObject(newParentId)
        : nullptr;
    const std::string newParentInstanceId = newParent ? newParent->instanceId : std::string{};
    const bool changed = newParent ? child->SetParent(newParent) : child->ClearParent();
    if (!changed) return;

    scene::Scene* scene = ctx.activeScene;
    const auto markDirty = ctx.markSceneDirty;
    auto apply = [scene, childInstanceId, markDirty](const std::string& parentInstanceId) {
        if (auto* target = scene->FindByGuid(childInstanceId)) {
            if (!parentInstanceId.empty()) {
                if (auto* parent = scene->FindByGuid(parentInstanceId))
                    target->SetParent(parent);
            } else {
                target->ClearParent();
            }
            if (markDirty) markDirty();
        }
    };

    if (ctx.undoStack && ctx.undoStack->IsRecordingEnabled()) {
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            description,
            [apply, newParentInstanceId]() { apply(newParentInstanceId); },
            [apply, oldParentInstanceId]() { apply(oldParentInstanceId); }));
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

enum class PrimitiveTemplate {
    Cube,
    Sphere,
    Plane,
    Quad,
    Cylinder,
    Cone,
    Torus,
    Capsule
};

renderer::Mesh* CreatePrimitiveMesh(PrimitiveTemplate type)
{
    auto* resources = renderer::ResourceManager::Active();
    if (!resources) return nullptr;

    switch (type) {
    case PrimitiveTemplate::Cube:     return renderer::PrimitiveMesh::Cube(*resources);
    case PrimitiveTemplate::Sphere:   return renderer::PrimitiveMesh::Sphere(*resources);
    case PrimitiveTemplate::Plane:    return renderer::PrimitiveMesh::Plane(*resources);
    case PrimitiveTemplate::Quad:     return renderer::PrimitiveMesh::Quad(*resources);
    case PrimitiveTemplate::Cylinder: return renderer::PrimitiveMesh::Cylinder(*resources);
    case PrimitiveTemplate::Cone:     return renderer::PrimitiveMesh::Cone(*resources);
    case PrimitiveTemplate::Torus:    return renderer::PrimitiveMesh::Torus(*resources);
    case PrimitiveTemplate::Capsule:  return renderer::PrimitiveMesh::Capsule(*resources);
    default:                          return nullptr;
    }
}

const char* GetPrimitivePath(PrimitiveTemplate type)
{
    switch (type) {
    case PrimitiveTemplate::Cube:     return "primitive:cube";
    case PrimitiveTemplate::Sphere:   return "primitive:sphere";
    case PrimitiveTemplate::Plane:    return "primitive:plane";
    case PrimitiveTemplate::Quad:     return "primitive:quad";
    case PrimitiveTemplate::Cylinder: return "primitive:cylinder";
    case PrimitiveTemplate::Cone:     return "primitive:cone";
    case PrimitiveTemplate::Torus:    return "primitive:torus";
    case PrimitiveTemplate::Capsule:  return "primitive:capsule";
    }
    return "";
}

scene::BoxColliderComponent CreateTemplateBoxCollider(const math::Vector3& halfExtents)
{
    scene::BoxColliderComponent collider;
    collider.size = halfExtents * 2.0f;
    collider.collider = std::make_unique<physics::OBBCollider>(halfExtents);
    return collider;
}

scene::SphereColliderComponent CreateTemplateSphereCollider()
{
    scene::SphereColliderComponent collider;
    collider.radius = 0.5f;
    collider.collider = std::make_unique<physics::SphereCollider>(0.5f);
    return collider;
}

scene::CapsuleColliderComponent CreateTemplateCapsuleCollider()
{
    scene::CapsuleColliderComponent collider;
    collider.radius = 0.25f;
    collider.halfHeight = 0.25f;
    collider.collider = std::make_unique<physics::CapsuleCollider>(0.25f, 0.25f);
    return collider;
}

void AddTemplateCollider(scene::GameObject& go, PrimitiveTemplate type)
{
    switch (type) {
    case PrimitiveTemplate::Sphere:
        go.AddComponent<scene::SphereColliderComponent>(CreateTemplateSphereCollider());
        break;
    case PrimitiveTemplate::Capsule:
        go.AddComponent<scene::CapsuleColliderComponent>(CreateTemplateCapsuleCollider());
        break;
    case PrimitiveTemplate::Plane:
        go.AddComponent<scene::BoxColliderComponent>(
            CreateTemplateBoxCollider(math::Vector3{ 0.5f, 0.01f, 0.5f }));
        break;
    case PrimitiveTemplate::Quad:
        go.AddComponent<scene::BoxColliderComponent>(
            CreateTemplateBoxCollider(math::Vector3{ 0.5f, 0.5f, 0.01f }));
        break;
    case PrimitiveTemplate::Cube:
    case PrimitiveTemplate::Cylinder:
    case PrimitiveTemplate::Cone:
    case PrimitiveTemplate::Torus:
        go.AddComponent<scene::BoxColliderComponent>(
            CreateTemplateBoxCollider(math::Vector3{ 0.5f, 0.5f, 0.5f }));
        break;
    }
}

void CreatePrimitiveObject(EditorContext& ctx, const char* name, PrimitiveTemplate type)
{
    auto& go = ctx.activeScene->CreateGameObject(name);

    scene::MeshRenderer mr;
    mr.meshPath = GetPrimitivePath(type);
    mr.mesh = CreatePrimitiveMesh(type);
    go.AddComponent<scene::MeshRenderer>(mr);

    scene::MaterialComponent mc;
    mc.materialPath = "Assets/Materials/Surface/Lit.fzmat";
    go.AddComponent<scene::MaterialComponent>(std::move(mc));

    if (type == PrimitiveTemplate::Quad) {
        auto script = scene::ScriptFactory::Create("QuadBillboardComponent");
        if (script) {
            scene::ScriptComponent sc;
            scene::ScriptEntry entry;
            entry.script = std::move(script);
            sc.scripts.emplace_back(std::move(entry));
            go.AddComponent<scene::ScriptComponent>(std::move(sc));
        }
    }

    AddTemplateCollider(go, type);

    ctx.selectedEntities = { go.GetID() };
}

void CreateLightObject(EditorContext& ctx, const char* name, scene::LightComponent::Type type)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    scene::LightComponent light;
    light.type = type;
    if (type == scene::LightComponent::Type::Point) {
        light.intensity = 4.0f;
        light.range = 8.0f;
    } else if (type == scene::LightComponent::Type::Spot) {
        light.intensity = 5.0f;
        light.range = 12.0f;
    }
    go.AddComponent<scene::LightComponent>(light);
    ctx.selectedEntities = { go.GetID() };
}

void CreateCameraObject(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Camera");
    go.transform.position = { 0.0f, 2.0f, -5.0f };
    go.AddComponent<scene::CameraComponent>();
    ctx.selectedEntities = { go.GetID() };
}

// デカール投影ボリューム: X/Z が投影面サイズ、Y が投影深度
void CreateDecalObject(EditorContext& ctx, const char* name, float sizeXZ, float depth)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    go.transform.scale = { sizeXZ, depth, sizeXZ };
    go.AddComponent<scene::DecalComponent>();
    ctx.selectedEntities = { go.GetID() };
}

// -----------------------------------------------------------------------
// UI オブジェクト生成ヘルパー
// WHY: Unity の GameObject/UI メニューに倣い、よく使う UI 要素を
//      1 操作で配置できるようにする。コンポーネントの組み合わせを
//      ここで確定させることで、ユーザーが手動で Add Component する手間を省く。
// -----------------------------------------------------------------------

// UICanvas ルートを生成する。ScreenSpace を既定値とする。
void CreateUICanvasObject(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Canvas");
    go.AddComponent<scene::UICanvas>();
    ctx.selectedEntities = { go.GetID() };
    ctx.activeUICanvas = go.GetID();
}

// UIImage のみのシンプルな画像要素。サイズは transform.scale.xy で制御する。
void CreateUIImageObject(EditorContext& ctx, const char* name,
                         const math::Vector4& color, float w, float h)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    go.transform.scale = { w, h, 1.0f };
    scene::UIImage img;
    img.color = color;
    go.AddComponent<scene::UIImage>(img);
    ctx.selectedEntities = { go.GetID() };
}

// UIText テキスト要素。デフォルト文字列と白色で生成する。
void CreateUITextObject(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Text");
    scene::UIText txt;
    txt.text     = "Text";
    txt.fontSize = 42.0f;
    txt.color    = { 1.0f, 1.0f, 1.0f, 1.0f };
    go.AddComponent<scene::UIText>(txt);
    ctx.selectedEntities = { go.GetID() };
}

// UIButton: 背景 Image + Button コンポーネントを親に、
//           ラベル Text を子として持つ Unity 標準構成で生成する。
void CreateUIButtonObject(EditorContext& ctx)
{
    auto& go = ctx.activeScene->CreateGameObject("Button");
    go.transform.scale = { 160.0f, 40.0f, 1.0f };

    scene::UIImage img;
    img.color = { 0.90f, 0.90f, 0.90f, 1.0f };
    go.AddComponent<scene::UIImage>(img);
    go.AddComponent<scene::UIButton>();

    // ラベル: 暗めテキストで中央配置 (位置は inspector で調整)
    auto& label = ctx.activeScene->CreateGameObject("Label");
    scene::UIText txt;
    txt.text     = "Button";
    txt.fontSize = 24.0f;
    txt.color    = { 0.20f, 0.20f, 0.20f, 1.0f };
    label.AddComponent<scene::UIText>(txt);
    label.SetParent(&go);

    ctx.selectedEntities = { go.GetID() };
}

// UILayoutGroup 水平 / 垂直レイアウト
void CreateUILayoutGroupObject(EditorContext& ctx, const char* name, scene::UILayoutAxis axis)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    scene::UILayoutGroup layout;
    layout.axis    = axis;
    layout.spacing = 8.0f;
    go.AddComponent<scene::UILayoutGroup>(layout);
    ctx.selectedEntities = { go.GetID() };
}

void RemoveSelection(EditorContext& ctx, scene::EntityID id)
{
    auto& selected = ctx.selectedEntities;
    selected.erase(std::remove(selected.begin(), selected.end(), id), selected.end());
}

void PruneSelection(EditorContext& ctx)
{
    auto& selected = ctx.selectedEntities;
    selected.erase(std::remove_if(selected.begin(), selected.end(),
        [&ctx](scene::EntityID id) { return !ctx.activeScene->IsValid(id); }),
        selected.end());
}

void DestroySelected(EditorContext& ctx, const std::vector<scene::EntityID>& ids)
{
    for (scene::EntityID id : ids)
        ctx.activeScene->DestroyGameObject(id);
    PruneSelection(ctx);
}

// srcId の GO とその子孫を再帰的に複製する。parentId が有効なら複製先に親付けする。
scene::EntityID DuplicateHierarchyRecursive(EditorContext& ctx,
                                             scene::EntityID srcId,
                                             scene::EntityID parentId,
                                             bool addCloneSuffix)
{
    auto* src = ctx.activeScene->GetGameObject(srcId);
    if (!src) return scene::EntityID::INVALID;

    auto& dst = ctx.activeScene->CreateGameObject(
        src->name + (addCloneSuffix ? " (Clone)" : ""));
    dst.tag       = src->tag;
    dst.layer     = src->layer;
    dst.transform = src->transform;
    ctx.activeScene->DuplicateComponents(srcId, dst.GetID());

    if (parentId.IsValid()) {
        if (auto* parent = ctx.activeScene->GetGameObject(parentId))
            dst.SetParent(parent);
    }

    for (int i = 0; i < src->GetChildCount(); ++i) {
        if (auto* child = src->GetChild(i))
            DuplicateHierarchyRecursive(ctx, child->GetID(), dst.GetID(), false);
    }
    return dst.GetID();
}

// parentId が有効な場合は新規 GO を parentId の子として生成する。
void DrawCreateObjectMenu(EditorContext& ctx, std::function<void()>& deferred,
                          scene::EntityID parentId = {})
{
    if (ImGui::MenuItem("Empty")) {
        deferred = [&ctx]() {
            auto& newGo = ctx.activeScene->CreateGameObject("GameObject");
            ctx.selectedEntities = { newGo.GetID() };
        };
    }

    if (ImGui::BeginMenu("3D Object")) {
        if (ImGui::MenuItem("Cube"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Cube", PrimitiveTemplate::Cube); };
        if (ImGui::MenuItem("Sphere"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Sphere", PrimitiveTemplate::Sphere); };
        if (ImGui::MenuItem("Plane"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Plane", PrimitiveTemplate::Plane); };
        if (ImGui::MenuItem("Quad"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Quad", PrimitiveTemplate::Quad); };
        if (ImGui::MenuItem("Cylinder"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Cylinder", PrimitiveTemplate::Cylinder); };
        if (ImGui::MenuItem("Cone"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Cone", PrimitiveTemplate::Cone); };
        if (ImGui::MenuItem("Torus"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Torus", PrimitiveTemplate::Torus); };
        if (ImGui::MenuItem("Capsule"))
            deferred = [&ctx]() { CreatePrimitiveObject(ctx, "Capsule", PrimitiveTemplate::Capsule); };
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Light")) {
        if (ImGui::MenuItem("Directional Light"))
            deferred = [&ctx]() { CreateLightObject(ctx, "Directional Light", scene::LightComponent::Type::Directional); };
        if (ImGui::MenuItem("Point Light"))
            deferred = [&ctx]() { CreateLightObject(ctx, "Point Light", scene::LightComponent::Type::Point); };
        if (ImGui::MenuItem("Spot Light"))
            deferred = [&ctx]() { CreateLightObject(ctx, "Spot Light", scene::LightComponent::Type::Spot); };
        ImGui::EndMenu();
    }

    if (ImGui::MenuItem("Camera"))
        deferred = [&ctx]() { CreateCameraObject(ctx); };

    // WHY: Unity と同様に UI 系オブジェクトを右クリック 1 操作で配置できる動線を用意する。
    //      Canvas 配下への自動ペアレントは行わず、ユーザーがヒエラルキーで自由に構成できるよう
    //      root レベルに生成する（3D Object / Light と同じ方針）。
    if (ImGui::BeginMenu("UI")) {
        if (ImGui::MenuItem("Canvas"))
            deferred = [&ctx]() { CreateUICanvasObject(ctx); };
        ImGui::Separator();
        if (ImGui::MenuItem("Image"))
            deferred = [&ctx]() {
                CreateUIImageObject(ctx, "Image",
                    { 1.0f, 1.0f, 1.0f, 1.0f }, 100.0f, 100.0f);
            };
        if (ImGui::MenuItem("Text"))
            deferred = [&ctx]() { CreateUITextObject(ctx); };
        if (ImGui::MenuItem("Button"))
            deferred = [&ctx]() { CreateUIButtonObject(ctx); };
        if (ImGui::MenuItem("Panel"))
            // 半透明グレーで canvas 全体を覆う背景パネル
            deferred = [&ctx]() {
                CreateUIImageObject(ctx, "Panel",
                    { 0.20f, 0.20f, 0.20f, 0.80f }, 1920.0f, 1080.0f);
            };
        ImGui::Separator();
        if (ImGui::BeginMenu("Layout Group")) {
            if (ImGui::MenuItem("Horizontal"))
                deferred = [&ctx]() {
                    CreateUILayoutGroupObject(ctx, "Horizontal Layout Group",
                        scene::UILayoutAxis::Horizontal);
                };
            if (ImGui::MenuItem("Vertical"))
                deferred = [&ctx]() {
                    CreateUILayoutGroupObject(ctx, "Vertical Layout Group",
                        scene::UILayoutAxis::Vertical);
                };
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Decal")) {
        if (ImGui::MenuItem("Decal (2m x 2m)"))
            deferred = [&ctx]() { CreateDecalObject(ctx, "Decal", 2.0f, 0.5f); };
        if (ImGui::MenuItem("Decal (1m x 1m)"))
            deferred = [&ctx]() { CreateDecalObject(ctx, "Decal (Small)", 1.0f, 0.3f); };
        if (ImGui::MenuItem("Decal (5m x 5m)"))
            deferred = [&ctx]() { CreateDecalObject(ctx, "Decal (Large)", 5.0f, 1.0f); };
        ImGui::EndMenu();
    }

    // Assets/Prefabs にある .fbzzprefab ファイルをメニューからインスタンス化できる。
    // WHY: AssetBrowser からのドラッグ操作なしで Prefab を配置できる動線を用意する。
    //      ListAll は存在しないディレクトリに対して空リストを返すため、事前チェック不要。
    if (ImGui::BeginMenu("Prefab")) {
        const std::string assetRoot = ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
        const std::string prefabDir = assetRoot + "/Prefabs";

        bool anyFound = false;
        for (const auto& path : util::FileSystem::ListAll(prefabDir)) {
            // 拡張子を小文字で比較して .fbzzprefab だけを列挙する
            const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
            if (ext != ".fbzzprefab") continue;

            anyFound = true;
            const std::string name = util::FileSystem::GetFilename(path);
            if (ImGui::MenuItem(name.c_str())) {
                deferred = [&ctx, path]() {
                    std::vector<scene::EntityID> roots;
                    if (PrefabSerializer::Instantiate(*ctx.activeScene, path, roots))
                        ctx.selectedEntities = roots;
                };
            }
        }
        if (!anyFound)
            ImGui::TextDisabled("(No prefabs found in Assets/Prefabs)");

        ImGui::EndMenu();
    }
}

bool ReadEntityPayload(const ImGuiPayload* payload, scene::EntityID& outId)
{
    if (!payload || payload->DataSize != sizeof(scene::EntityID)) return false;
    std::memcpy(&outId, payload->Data, sizeof(outId));
    return true;
}

bool ContainsEntity(const std::vector<scene::EntityID>& entities, scene::EntityID id)
{
    return std::find(entities.begin(), entities.end(), id) != entities.end();
}

// 子孫を再帰的に visited に追加するだけ（ImGui 呼び出しなし）。
// 親が閉じているとき子が第2ループで誤って root 描画されるのを防ぐ。
std::string SanitizeAssetName(const std::string& name)
{
    std::string result = name.empty() ? "Prefab" : name;
    for (char& c : result) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok) c = '_';
    }
    return result;
}

std::string UniquePrefabPath(const EditorContext& ctx, const std::string& objectName)
{
    const std::string assetRoot = ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
    const std::string prefabDir = assetRoot + "/Prefabs";
    util::FileSystem::EnsureDirectory(prefabDir);

    const std::string base = prefabDir + "/" + SanitizeAssetName(objectName);
    std::string path = base + ".fbzzprefab";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".fbzzprefab";
    return path;
}

void SaveSelectedAsPrefab(EditorContext& ctx, const std::string& objectName)
{
    if (!ctx.activeScene || ctx.selectedEntities.empty()) return;

    const std::string path = UniquePrefabPath(ctx, objectName);
    if (PrefabSerializer::SaveSelection(*ctx.activeScene, ctx.selectedEntities, path)) {
        if (ctx.undoStack) {
            std::string content;
            util::FileSystem::ReadText(path, content);
            EditorContext* context = &ctx;
            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                "Create Prefab",
                [path, content, context]() {
                    util::FileSystem::WriteText(path, content);
                    context->requestAssetBrowserRefresh = true;
                },
                [path, context]() {
                    util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(path));
                    context->requestAssetBrowserRefresh = true;
                }));
        }
        ctx.requestAssetBrowserRefresh = true;
    }
}

bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath)
{
    if (!payload || payload->DataSize <= 0) return false;
    outPath.assign(static_cast<const char*>(payload->Data),
                   static_cast<size_t>(payload->DataSize - 1));
    return !outPath.empty();
}

void MarkDescendantsVisited(scene::GameObject& go,
                            std::vector<scene::EntityID>& visited)
{
    for (int i = 0; i < go.GetChildCount(); ++i) {
        auto* child = go.GetChild(i);
        if (!child || ContainsEntity(visited, child->GetID())) continue;
        visited.push_back(child->GetID());
        MarkDescendantsVisited(*child, visited);
    }
}

void DrawHierarchyNode(EditorContext& ctx,
                       scene::GameObject& go,
                       size_t rootIndex,          // roots 配列内のインデックス（Order メニュー用, root のみ有効）
                       size_t rootCount,
                       std::vector<scene::EntityID>& visited,
                       std::function<void()>& deferred,
                       scene::EntityID* pendingExpand,
                       scene::EntityID& lastClicked,
                       std::vector<scene::EntityID>& outVisible,
                       const std::vector<scene::EntityID>& prevVisible)
{
    const scene::EntityID id = go.GetID();
    if (ContainsEntity(visited, id)) return;
    visited.push_back(id);
    outVisible.push_back(id);

    const bool hasChildren    = go.GetChildCount() > 0;
    const bool selected       = ContainsEntity(ctx.selectedEntities, id);
    const bool isRoot         = go.GetParent() == nullptr;
    const bool isActive       = go.activeInHierarchy();
    const bool isLocked       = ctx.IsLocked(id);
    const bool isEditorHidden = ctx.editorHiddenGuids.count(go.instanceId) > 0;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_FramePadding;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected)     flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(static_cast<int>(id.index));

    // SetParent 後の次フレームで強制 open
    if (pendingExpand && *pendingExpand == id) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        *pendingExpand = scene::EntityID{};
    }

    // エディタ専用非表示はシアン（runtime 非アクティブより優先）、非アクティブはグレー、ロック中はオレンジ
    if (isEditorHidden)  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(80, 180, 200, 255));
    else if (!isActive)  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(130, 130, 130, 255));
    else if (isLocked)   ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 175, 80, 255));
    const bool opened = ImGui::TreeNodeEx(go.name.c_str(), flags);
    if (isEditorHidden || !isActive || isLocked) ImGui::PopStyleColor();

    // --- 右端 visibility/editor-hide/lock アイコン (DrawList で直接描画) ---
    {
        const ImVec2 nodeMin = ImGui::GetItemRectMin();
        const ImVec2 nodeMax = ImGui::GetItemRectMax();
        const float  h       = nodeMax.y - nodeMin.y;
        const float  btnW    = h + 2.0f;
        // SpanAvailWidth のためnodeMax.x = ウィンドウコンテンツ右端
        const float  rx      = nodeMax.x;

        // アイコン配置: lock | vis | editorHide (右から)
        const ImVec2 ehMin   = { rx - btnW * 3.0f, nodeMin.y };
        const ImVec2 ehMax   = { rx - btnW * 2.0f, nodeMax.y };
        const ImVec2 visMin  = { rx - btnW * 2.0f, nodeMin.y };
        const ImVec2 visMax  = { rx - btnW,         nodeMax.y };
        const ImVec2 lockMin = { rx - btnW,          nodeMin.y };
        const ImVec2 lockMax = { rx,                 nodeMax.y };

        // ヒット判定
        const bool ehHov   = ImGui::IsMouseHoveringRect(ehMin,   ehMax,   false);
        const bool visHov  = ImGui::IsMouseHoveringRect(visMin,  visMax,  false);
        const bool lockHov = ImGui::IsMouseHoveringRect(lockMin, lockMax, false);
        const bool ehClick   = ehHov   && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool visClick  = visHov  && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool lockClick = lockHov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        if (ehClick) deferred = [&ctx, id]() {
            if (auto* g = ctx.activeScene->GetGameObject(id)) {
                const std::string guid = g->instanceId;
                auto it = ctx.editorHiddenGuids.find(guid);
                if (it != ctx.editorHiddenGuids.end()) {
                    // 解除: 非表示前の activeSelf を復元
                    g->SetActive(it->second);
                    ctx.editorHiddenGuids.erase(it);
                } else {
                    // 非表示: 現在の activeSelf を保存してから SetActive(false)
                    ctx.editorHiddenGuids[guid] = g->activeSelf();
                    g->SetActive(false);
                }
            }
        };
        if (visClick)  deferred = [&ctx, id]() {
            if (auto* g = ctx.activeScene->GetGameObject(id)) g->SetActive(!g->activeSelf());
        };
        if (lockClick) ctx.ToggleLock(id);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // エディタ専用非表示アイコン (editor-hide: ホバー時/非表示中のみ表示)
        if (ehHov || isEditorHidden) {
            if (ehHov) dl->AddRectFilled(ehMin, ehMax, IM_COL32(80, 80, 80, 160), 2.0f);
            const char* ehChar = isEditorHidden ? "E" : "e";
            ImVec2 ets = ImGui::CalcTextSize(ehChar);
            dl->AddText({ ehMin.x + (btnW - ets.x) * 0.5f, ehMin.y + (h - ets.y) * 0.5f },
                        isEditorHidden ? IM_COL32(80, 200, 220, 240) : IM_COL32(120, 120, 120, 140),
                        ehChar);
        }
        if (ehHov) ImGui::SetTooltip(isEditorHidden
            ? "Editor-only hidden (click to show)\nEntity is active at runtime & saved as active"
            : "Click to hide in editor only\nWill be active at runtime & saved as active");

        // visibility アイコン
        if (visHov) dl->AddRectFilled(visMin, visMax, IM_COL32(80, 80, 80, 160), 2.0f);
        const char* visChar = isActive ? "o" : "-";
        ImVec2 vts = ImGui::CalcTextSize(visChar);
        dl->AddText({ visMin.x + (btnW - vts.x) * 0.5f, visMin.y + (h - vts.y) * 0.5f },
                    isActive ? IM_COL32(200, 200, 200, 200) : IM_COL32(100, 100, 100, 200), visChar);

        // lock アイコン (常時描画: ロック中はオレンジ、非ロック+ホバーは薄く)
        if (lockHov || isLocked) {
            if (lockHov) dl->AddRectFilled(lockMin, lockMax, IM_COL32(80, 80, 80, 160), 2.0f);
            ImVec2 lts = ImGui::CalcTextSize("L");
            dl->AddText({ lockMin.x + (btnW - lts.x) * 0.5f, lockMin.y + (h - lts.y) * 0.5f },
                        isLocked ? IM_COL32(255, 175, 50, 240) : IM_COL32(120, 120, 120, 140), "L");
        }

        // ノードのクリック/ダブルクリック判定 (アイコン領域は除外)
        const bool iconAreaClick = ehClick || visClick || lockClick;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()
            && !isLocked && !iconAreaClick) {
            ctx.selectedAssetPath.clear();
            const bool shiftHeld = ImGui::GetIO().KeyShift;
            const bool ctrlHeld  = ImGui::GetIO().KeyCtrl;
            if (shiftHeld && lastClicked.IsValid()) {
                // Shift+クリック: prevVisible の順番で lastClicked〜id の範囲を選択
                auto it1 = std::find(prevVisible.begin(), prevVisible.end(), lastClicked);
                auto it2 = std::find(prevVisible.begin(), prevVisible.end(), id);
                if (it1 != prevVisible.end() && it2 != prevVisible.end()) {
                    if (!ctrlHeld) ctx.selectedEntities.clear();
                    if (it1 > it2) std::swap(it1, it2);
                    for (auto it = it1; it <= it2; ++it)
                        if (!ContainsEntity(ctx.selectedEntities, *it))
                            ctx.selectedEntities.push_back(*it);
                }
            } else {
                if (!ctrlHeld) ctx.selectedEntities.clear();
                auto it = std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id);
                if (it != ctx.selectedEntities.end())
                    ctx.selectedEntities.erase(it);
                else
                    ctx.selectedEntities.push_back(id);
                lastClicked = id;
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
            && !isLocked && !iconAreaClick) {
            ctx.focusTargetPosition    = go.transform.position;
            ctx.requestFocusOnSelected = true;
        }
    }

    if (!isLocked && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("FBZZ_HIERARCHY_ENTITY", &id, sizeof(id));
        ImGui::TextUnformatted(go.name.c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        scene::EntityID draggedId;
        if (ReadEntityPayload(ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), draggedId) &&
            draggedId != id) {
            deferred = [&ctx, draggedId, id, pendingExpand]() {
                auto* dragged = ctx.activeScene->GetGameObject(draggedId);
                auto* target  = ctx.activeScene->GetGameObject(id);
                if (dragged && target) {
                    SetParentWithUndo(ctx, draggedId, id, "Reparent GameObject");
                    ctx.selectedEntities = { draggedId };
                    if (pendingExpand) *pendingExpand = id;  // 次フレームで親を open
                }
            };
        }
        std::string assetPath;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath)) {
            const std::string ext = util::FileSystem::GetExtension(assetPath);
            if (ext == ".asset") {
                deferred = [&ctx, assetPath, id, pendingExpand]() {
                    const scene::EntityID rootId =
                        SpawnFzAssetHierarchy(*ctx.activeScene, assetPath);
                    if (rootId == scene::EntityID::INVALID) return;
                    auto* root   = ctx.activeScene->GetGameObject(rootId);
                    auto* parent = ctx.activeScene->GetGameObject(id);
                    if (root && parent) root->SetParent(parent);
                    ctx.selectedEntities = { rootId };
                    if (pendingExpand) *pendingExpand = id;
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                };
            } else if (ext == ".fbzzprefab") {
                deferred = [&ctx, assetPath, id, pendingExpand]() {
                    std::vector<scene::EntityID> roots;
                    if (!PrefabSerializer::Instantiate(*ctx.activeScene, assetPath, roots)) return;
                    for (scene::EntityID rootId : roots) {
                        auto* root = ctx.activeScene->GetGameObject(rootId);
                        auto* parent = ctx.activeScene->GetGameObject(id);
                        if (root && parent) root->SetParent(parent);
                    }
                    ctx.selectedEntities = roots;
                    if (pendingExpand) *pendingExpand = id;
                };
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem()) {
        // 右クリックした GO が既に複数選択中なら選択を維持する。
        // そうでなければ単一選択に切り替える。
        {
            const bool alreadySelected = std::find(
                ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id)
                != ctx.selectedEntities.end();
            if (!alreadySelected || ctx.selectedEntities.size() == 1)
                ctx.selectedEntities = { id };
        }
        const bool multiSelected = ctx.selectedEntities.size() > 1;

        if (ImGui::MenuItem(multiSelected ? "Hide/Show" : (isActive ? "Hide" : "Show"))) {
            if (multiSelected) {
                const std::vector<scene::EntityID> toToggle = ctx.selectedEntities;
                deferred = [&ctx, toToggle]() {
                    bool anyActive = false;
                    for (auto eid : toToggle)
                        if (auto* g = ctx.activeScene->GetGameObject(eid))
                            if (g->activeSelf()) { anyActive = true; break; }
                    const bool newState = !anyActive;
                    for (auto eid : toToggle)
                        if (auto* g = ctx.activeScene->GetGameObject(eid))
                            g->SetActive(newState);
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                };
            } else {
                deferred = [&ctx, id]() {
                    if (auto* g = ctx.activeScene->GetGameObject(id)) {
                        const std::string instanceId = g->instanceId;
                        const bool before = g->activeSelf();
                        const bool after = !before;
                        g->SetActive(after);
                        scene::Scene* scene = ctx.activeScene;
                        const auto markDirty = ctx.markSceneDirty;
                        if (ctx.undoStack) {
                            auto apply = [scene, instanceId, markDirty](bool active) {
                                if (auto* target = scene->FindByGuid(instanceId)) {
                                    target->SetActive(active);
                                    if (markDirty) markDirty();
                                }
                            };
                            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                                after ? "Show GameObject" : "Hide GameObject",
                                [apply, after]() { apply(after); },
                                [apply, before]() { apply(before); }));
                        }
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }
                };
            }
        }
        if (ImGui::MenuItem(isLocked ? "Unlock" : "Lock"))
            ctx.ToggleLock(id);
        ImGui::Separator();

        if (ImGui::BeginMenu("Create")) {
            DrawCreateObjectMenu(ctx, deferred);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Create Child")) {
            std::function<void()> childDeferred;
            DrawCreateObjectMenu(ctx, childDeferred);
            if (childDeferred) {
                // childDeferred が選択セットした GO を parentId の子にする
                deferred = [&ctx, id, childDeferred = std::move(childDeferred)]() {
                    childDeferred();
                    if (!ctx.selectedEntities.empty()) {
                        if (auto* newGo = ctx.activeScene->GetGameObject(ctx.selectedEntities.back()))
                            if (auto* parent = ctx.activeScene->GetGameObject(id))
                                newGo->SetParent(parent);
                    }
                };
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate")) {
            if (multiSelected) {
                const std::vector<scene::EntityID> toDup = ctx.selectedEntities;
                deferred = [&ctx, toDup]() {
                    std::vector<scene::EntityID> newIds;
                    for (auto eid : toDup) {
                        auto* src = ctx.activeScene->GetGameObject(eid);
                        const scene::EntityID parentId = src && src->GetParent()
                            ? src->GetParent()->GetID() : scene::EntityID{};
                        const scene::EntityID newId =
                            DuplicateHierarchyRecursive(ctx, eid, parentId, true);
                        if (newId != scene::EntityID::INVALID)
                            newIds.push_back(newId);
                    }
                    if (!newIds.empty()) ctx.selectedEntities = newIds;
                };
            } else {
                const scene::EntityID parentId =
                    go.GetParent() ? go.GetParent()->GetID() : scene::EntityID{};
                deferred = [&ctx, id, parentId]() {
                    const scene::EntityID newId =
                        DuplicateHierarchyRecursive(ctx, id, parentId, true);
                    if (newId != scene::EntityID::INVALID)
                        ctx.selectedEntities = { newId };
                };
            }
        }
        if (multiSelected) {
            if (ImGui::MenuItem("Group Selection")) {
                const std::vector<scene::EntityID> toGroup = ctx.selectedEntities;
                deferred = [&ctx, toGroup]() {
                    if (!ctx.activeScene) return;
                    // 共通親 (全員同じ親を持つ場合) を探す
                    scene::EntityID commonParentId{};
                    bool firstItem = true;
                    for (auto eid : toGroup) {
                        if (auto* g = ctx.activeScene->GetGameObject(eid)) {
                            const scene::EntityID pid = g->GetParent()
                                ? g->GetParent()->GetID() : scene::EntityID{};
                            if (firstItem) { commonParentId = pid; firstItem = false; }
                            else if (commonParentId != pid) { commonParentId = {}; break; }
                        }
                    }
                    auto& group = ctx.activeScene->CreateGameObject("Group");
                    if (commonParentId.IsValid())
                        if (auto* cp = ctx.activeScene->GetGameObject(commonParentId))
                            group.SetParent(cp);
                    for (auto eid : toGroup)
                        if (auto* child = ctx.activeScene->GetGameObject(eid))
                            child->SetParent(&group);
                    const scene::EntityID groupId = group.GetID();
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                    if (ctx.undoStack) {
                        scene::Scene* scene = ctx.activeScene;
                        const std::string groupGuid = group.instanceId;
                        const auto markDirty = ctx.markSceneDirty;
                        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                            "Group Selection",
                            []() {},   // redo: 再実行には deferred が必要で複雑なため省略
                            [scene, groupGuid, markDirty]() {
                                // undo: 子を解除してグループを削除
                                if (auto* g = scene->FindByGuid(groupGuid)) {
                                    while (g->GetChildCount() > 0)
                                        if (auto* c = g->GetChild(0)) c->ClearParent();
                                    scene->DestroyGameObject(g->GetID());
                                }
                                if (markDirty) markDirty();
                            }));
                    }
                    ctx.selectedEntities = { groupId };
                };
            }
        }
        if (ImGui::MenuItem("Save As Prefab")) {
            SaveSelectedAsPrefab(ctx, go.name);
        }
        if (ImGui::BeginMenu("Hierarchy")) {
            const bool hasParent = go.GetParent() != nullptr;
            if (ImGui::MenuItem("Set As Root", nullptr, false, hasParent))
                deferred = [&ctx, id]() {
                    SetParentWithUndo(ctx, id, {}, "Set GameObject As Root");
                };
            ImGui::EndMenu();
        }
        if (isRoot && ImGui::BeginMenu("Order")) {
            const bool canMoveUp   = rootIndex > 0;
            const bool canMoveDown = rootIndex + 1 < rootCount;
            if (ImGui::MenuItem("Move Up", nullptr, false, canMoveUp))
                deferred = [&ctx, id]() { ctx.activeScene->MoveGameObject(id, -1); };
            if (ImGui::MenuItem("Move Down", nullptr, false, canMoveDown))
                deferred = [&ctx, id]() { ctx.activeScene->MoveGameObject(id, 1); };
            ImGui::Separator();
            if (ImGui::MenuItem("Move To Top", nullptr, false, canMoveUp))
                deferred = [&ctx, id]() { ctx.activeScene->MoveGameObjectToIndex(id, 0); };
            if (ImGui::MenuItem("Move To Bottom", nullptr, false, canMoveDown))
                deferred = [&ctx, id]() {
                    ctx.activeScene->MoveGameObjectToIndex(id, ctx.activeScene->GameObjectCount() - 1);
                };
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            const std::vector<scene::EntityID> toDelete = ctx.selectedEntities;
            deferred = [&ctx, toDelete]() { DestroySelected(ctx, toDelete); };
        }
        ImGui::EndPopup();
    }

    if (hasChildren) {
        if (opened) {
            for (int i = 0; i < go.GetChildCount(); ++i) {
                if (auto* child = go.GetChild(i))
                    DrawHierarchyNode(ctx, *child, 0, rootCount, visited, deferred, pendingExpand, lastClicked, outVisible, prevVisible);
            }
            ImGui::TreePop();
        } else {
            // 閉じていても子孫を visited に入れる。
            // これをしないと第2ループが子を root レベルで誤描画する。
            MarkDescendantsVisited(go, visited);
        }
    }

    ImGui::PopID();
}

void DuplicateAllSelected(EditorContext& ctx, std::function<void()>& deferred)
{
    if (ctx.selectedEntities.empty() || !ctx.activeScene) return;
    const std::vector<scene::EntityID> toDup = ctx.selectedEntities;
    deferred = [&ctx, toDup]() {
        std::vector<scene::EntityID> newIds;
        for (auto eid : toDup) {
            auto* src = ctx.activeScene->GetGameObject(eid);
            const scene::EntityID parentId = src && src->GetParent()
                ? src->GetParent()->GetID() : scene::EntityID{};
            const scene::EntityID newId = DuplicateHierarchyRecursive(ctx, eid, parentId, true);
            if (newId != scene::EntityID::INVALID)
                newIds.push_back(newId);
        }
        if (!newIds.empty()) ctx.selectedEntities = newIds;
    };
}

} // namespace

void SceneHierarchyPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        return;
    }

    if (ctx.mapEditingMode) {
        ImGui::TextColored({ 0.35f, 0.88f, 0.48f, 1.0f }, "MAP MODE");
        ImGui::SameLine();
        ImGui::Checkbox("Map Objects Only", &ctx.mapHierarchyFilter);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Show only Terrain, Water, Detail, Foliage and their stamp children\nUncheck to browse all objects in Map Mode");
    }

    // FoliageBakeSystem が新規子 GO を生成したら親ノードを自動展開する
    for (scene::EntityID eid : ctx.activeScene->GetEntities<scene::FoliageComponent>()) {
        auto* fc = ctx.activeScene->GetComponent<scene::FoliageComponent>(eid);
        if (fc && fc->needsHierarchyExpand) {
            m_pendingExpand = eid;
            fc->needsHierarchyExpand = false;
        }
    }

    // --- 検索バー ---
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##hierarchy_search", "Search...", m_searchFilter, sizeof(m_searchFilter));

    // --- F2 リネームポップアップ ---
    if (ImGui::BeginPopup("##hierarchy_rename")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(220.0f);
        const bool confirmed = ImGui::InputText("##ri", m_renameBuffer, sizeof(m_renameBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (confirmed) {
            const std::string newName = m_renameBuffer;
            const scene::EntityID rid = m_renamingId;
            ExecuteSceneEditWithUndo(ctx, "Rename GameObject", [&ctx, rid, newName]() {
                if (auto* g = ctx.activeScene->GetGameObject(rid))
                    g->name = newName;
            });
            m_renamingId = {};
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_renamingId = {};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 検索フィルタが有効なときはフラットリストで一致オブジェクトだけ表示する
    if (m_searchFilter[0] != '\0') {
        std::function<void()> deferred;

        for (auto& go : ctx.activeScene->GameObjects()) {
            if (!util::StringUtils::ContainsCI(go.name, m_searchFilter)) continue;
            if (ctx.mapEditingMode && ctx.mapHierarchyFilter) {
                const bool isMapObject =
                    go.GetComponent<scene::TerrainComponent>()
                    || go.GetComponent<scene::WaterComponent>()
                    || go.GetComponent<scene::TerrainDetailComponent>()
                    || go.GetComponent<scene::FoliageComponent>();
                auto* parentGO = go.GetParent();
                const bool isFoliageChild = parentGO
                    && (parentGO->GetComponent<scene::FoliageComponent>()
                        || parentGO->GetComponent<scene::TerrainComponent>());
                if (!isMapObject && !isFoliageChild)
                    continue;
            }
            const scene::EntityID id = go.GetID();
            const bool selected = ContainsEntity(ctx.selectedEntities, id);
            ImGui::PushID(static_cast<int>(id.index));
            if (ImGui::Selectable(go.name.c_str(), selected)) {
                if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
                if (selected)
                    RemoveSelection(ctx, id);
                else
                    ctx.selectedEntities.push_back(id);
            }
            if (ImGui::BeginPopupContextItem()) {
                ctx.selectedEntities = { id };
                if (ImGui::MenuItem("Save As Prefab"))
                    SaveSelectedAsPrefab(ctx, go.name);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) {
                    deferred = [&ctx, id]() {
                        ctx.activeScene->DestroyGameObject(id);
                        RemoveSelection(ctx, id);
                        PruneSelection(ctx);
                    };
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        if (!deferred && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Delete) && !ctx.selectedEntities.empty()) {
            std::vector<scene::EntityID> ids = ctx.selectedEntities;
            deferred = [&ctx, ids]() { DestroySelected(ctx, ids); };
        }
        if (!deferred && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !ctx.selectedEntities.empty())
            DuplicateAllSelected(ctx, deferred);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_F2) && ctx.selectedEntities.size() == 1) {
            if (auto* g = ctx.activeScene->GetGameObject(ctx.selectedEntities[0])) {
                m_renamingId = ctx.selectedEntities[0];
                std::strncpy(m_renameBuffer, g->name.c_str(), sizeof(m_renameBuffer) - 1);
                m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
                ImGui::OpenPopup("##hierarchy_rename");
            }
        }
        if (deferred)
            ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", deferred);
        return;
    }

    // Map Mode: Terrain/Water/Detail/Foliage のルート GO のみをツリー表示。
    // WHY: フラットリストでは stamp 子 GO が親から切り離されて見えるため、
    //      ツリー表示にして子 GO を Terrain ノード下に自然に見せる。
    if (ctx.mapEditingMode && ctx.mapHierarchyFilter) {
        std::function<void()> deferred;
        std::vector<scene::EntityID> visited;
        visited.reserve(ctx.activeScene->GameObjectCount());

        const auto roots = ctx.activeScene->GetRootGameObjects();
        const size_t rootCount = roots.size();
        std::vector<scene::EntityID> mapVisible;
        for (auto* go : roots) {
            if (!go) continue;
            if (!go->GetComponent<scene::TerrainComponent>()
                && !go->GetComponent<scene::WaterComponent>()
                && !go->GetComponent<scene::TerrainDetailComponent>()
                && !go->GetComponent<scene::FoliageComponent>())
                continue;
            DrawHierarchyNode(ctx, *go, 0, rootCount, visited, deferred, &m_pendingExpand,
                m_lastClickedEntity, mapVisible, m_visibleOrder);
        }

        if (!deferred && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Delete) && !ctx.selectedEntities.empty()) {
            std::vector<scene::EntityID> ids = ctx.selectedEntities;
            deferred = [&ctx, ids]() { DestroySelected(ctx, ids); };
        }
        if (!deferred && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !ctx.selectedEntities.empty())
            DuplicateAllSelected(ctx, deferred);
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_F2) && ctx.selectedEntities.size() == 1) {
            if (auto* g = ctx.activeScene->GetGameObject(ctx.selectedEntities[0])) {
                m_renamingId = ctx.selectedEntities[0];
                std::strncpy(m_renameBuffer, g->name.c_str(), sizeof(m_renameBuffer) - 1);
                m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
                ImGui::OpenPopup("##hierarchy_rename");
            }
        }
        if (deferred)
            ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", deferred);
        return;
    }

    // deferred: ノード描画ループ内でシーンを変更すると、その後のイテレーションで
    // ポインタが無効になる。変更操作 (生成/削除/親付け) はすべてラムダに包み、
    // ループ終了後にまとめて実行する。
    std::function<void()> deferred;
    std::vector<scene::EntityID> visited;
    visited.reserve(ctx.activeScene->GameObjectCount());
    std::vector<scene::EntityID> outVisible;
    outVisible.reserve(ctx.activeScene->GameObjectCount());
    const ImVec2 hierarchyMin = ImGui::GetWindowPos();
    const ImVec2 hierarchyMax = {
        hierarchyMin.x + ImGui::GetWindowSize().x,
        hierarchyMin.y + ImGui::GetWindowSize().y
    };
    const ImGuiID hierarchyDropId = ImGui::GetID("##hierarchy_drop_target");

    // root オブジェクトだけを起点にツリーを描画する。
    // DrawHierarchyNode 内で子孫も全て visited に登録される（collapsed でも）。
    const auto   roots     = ctx.activeScene->GetRootGameObjects();
    const size_t rootCount = roots.size();
    for (size_t i = 0; i < roots.size(); ++i)
        if (roots[i]) DrawHierarchyNode(ctx, *roots[i], i, rootCount, visited, deferred,
            &m_pendingExpand, m_lastClickedEntity, outVisible, m_visibleOrder);

    // 親がいないのに GetRootGameObjects に含まれなかった孤立オブジェクトを救済する。
    // 正常なシーンでは実行されない。
    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!ContainsEntity(visited, go.GetID()) && go.GetParent() == nullptr)
            DrawHierarchyNode(ctx, go, 0, 0, visited, deferred,
                &m_pendingExpand, m_lastClickedEntity, outVisible, m_visibleOrder);
    }

    m_visibleOrder = std::move(outVisible);

    if (ImGui::BeginDragDropTargetCustom(ImRect(hierarchyMin, hierarchyMax), hierarchyDropId)) {
        scene::EntityID draggedId;
        if (ReadEntityPayload(ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), draggedId)) {
            deferred = [&ctx, draggedId]() {
                if (auto* dragged = ctx.activeScene->GetGameObject(draggedId)) {
                    dragged->ClearParent();
                    ctx.selectedEntities = { draggedId };
                }
            };
        }
        std::string assetPath;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath)) {
            const std::string ext = util::FileSystem::GetExtension(assetPath);
            if (ext == ".asset") {
                deferred = [&ctx, assetPath]() {
                    const scene::EntityID rootId =
                        SpawnFzAssetHierarchy(*ctx.activeScene, assetPath);
                    if (rootId != scene::EntityID::INVALID) {
                        ctx.selectedEntities = { rootId };
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }
                };
            } else if (ext == ".fbzzprefab") {
                deferred = [&ctx, assetPath]() {
                    std::vector<scene::EntityID> roots;
                    if (PrefabSerializer::Instantiate(*ctx.activeScene, assetPath, roots))
                        ctx.selectedEntities = roots;
                };
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (!deferred && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete) && !ctx.selectedEntities.empty()) {
        std::vector<scene::EntityID> ids = ctx.selectedEntities;
        deferred = [&ctx, ids]() { DestroySelected(ctx, ids); };
    }
    if (!deferred && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !ctx.selectedEntities.empty())
        DuplicateAllSelected(ctx, deferred);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_F2) && ctx.selectedEntities.size() == 1) {
        if (auto* g = ctx.activeScene->GetGameObject(ctx.selectedEntities[0])) {
            m_renamingId = ctx.selectedEntities[0];
            std::strncpy(m_renameBuffer, g->name.c_str(), sizeof(m_renameBuffer) - 1);
            m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
            ImGui::OpenPopup("##hierarchy_rename");
        }
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered()) {
        ctx.selectedEntities.clear();
        m_lastClickedEntity = {};
    }

    if (ImGui::BeginPopupContextWindow("##scene_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        DrawCreateObjectMenu(ctx, deferred);
        ImGui::EndPopup();
    }

    if (deferred) {
        ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", deferred);
    }
}

} // namespace fbzz::editor
