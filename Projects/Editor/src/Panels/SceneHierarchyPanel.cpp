// FBZZ Engine
// SceneHierarchyPanel.cpp | fbzz::editor
// Scene GameObject hierarchy and selection editing
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
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
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

enum class PrimitiveTemplate {
    Cube,
    Sphere,
    Plane,
    Cylinder,
    Cone,
    Torus,
    Capsule
};

std::shared_ptr<renderer::Mesh> CreatePrimitiveMesh(PrimitiveTemplate type)
{
    auto* resources = renderer::ResourceManager::Active();
    if (!resources) return {};

    switch (type) {
    case PrimitiveTemplate::Cube:     return renderer::PrimitiveMesh::Cube(*resources);
    case PrimitiveTemplate::Sphere:   return renderer::PrimitiveMesh::Sphere(*resources);
    case PrimitiveTemplate::Plane:    return renderer::PrimitiveMesh::Plane(*resources);
    case PrimitiveTemplate::Cylinder: return renderer::PrimitiveMesh::Cylinder(*resources);
    case PrimitiveTemplate::Cone:     return renderer::PrimitiveMesh::Cone(*resources);
    case PrimitiveTemplate::Torus:    return renderer::PrimitiveMesh::Torus(*resources);
    case PrimitiveTemplate::Capsule:  return renderer::PrimitiveMesh::Capsule(*resources);
    }
    return {};
}

const char* GetPrimitivePath(PrimitiveTemplate type)
{
    switch (type) {
    case PrimitiveTemplate::Cube:     return "primitive:cube";
    case PrimitiveTemplate::Sphere:   return "primitive:sphere";
    case PrimitiveTemplate::Plane:    return "primitive:plane";
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
    collider.collider = std::make_shared<physics::OBBCollider>(halfExtents);
    return collider;
}

scene::SphereColliderComponent CreateTemplateSphereCollider()
{
    scene::SphereColliderComponent collider;
    collider.radius = 0.5f;
    collider.collider = std::make_shared<physics::SphereCollider>(0.5f);
    return collider;
}

scene::CapsuleColliderComponent CreateTemplateCapsuleCollider()
{
    scene::CapsuleColliderComponent collider;
    collider.radius = 0.25f;
    collider.halfHeight = 0.25f;
    collider.collider = std::make_shared<physics::CapsuleCollider>(0.25f, 0.25f);
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
    go.AddComponent<scene::MaterialComponent>(mc);

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
    go.transform.localPosition = { 0.0f, 2.0f, -5.0f };
    go.AddComponent<scene::CameraComponent>();
    ctx.selectedEntities = { go.GetID() };
}

// デカール投影ボリューム: X/Z が投影面サイズ、Y が投影深度
void CreateDecalObject(EditorContext& ctx, const char* name, float sizeXZ, float depth)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    go.transform.localScale = { sizeXZ, depth, sizeXZ };
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

// UIImage のみのシンプルな画像要素。サイズは transform.localScale.xy で制御する。
void CreateUIImageObject(EditorContext& ctx, const char* name,
                         const math::Vector4& color, float w, float h)
{
    auto& go = ctx.activeScene->CreateGameObject(name);
    go.transform.localScale = { w, h, 1.0f };
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
    go.transform.localScale = { 160.0f, 40.0f, 1.0f };

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

void DrawCreateObjectMenu(EditorContext& ctx, std::function<void()>& deferred)
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
    if (PrefabSerializer::SaveSelection(*ctx.activeScene, ctx.selectedEntities, path))
        ctx.requestAssetBrowserRefresh = true;
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
                       scene::EntityID* pendingExpand)
{
    const scene::EntityID id = go.GetID();
    if (ContainsEntity(visited, id)) return;
    visited.push_back(id);

    const bool hasChildren = go.GetChildCount() > 0;
    const bool selected    = ContainsEntity(ctx.selectedEntities, id);
    const bool isRoot      = go.GetParent() == nullptr;
    const bool isActive    = go.activeSelf();
    const bool isLocked    = ctx.IsLocked(id);

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

    // 非アクティブはグレーアウト、ロック中はオレンジ
    if (!isActive)     ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(130, 130, 130, 255));
    else if (isLocked) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 175, 80, 255));
    const bool opened = ImGui::TreeNodeEx(go.name.c_str(), flags);
    if (!isActive || isLocked) ImGui::PopStyleColor();

    // --- 右端 visibility/lock アイコン (DrawList で直接描画) ---
    {
        const ImVec2 nodeMin = ImGui::GetItemRectMin();
        const ImVec2 nodeMax = ImGui::GetItemRectMax();
        const float  h       = nodeMax.y - nodeMin.y;
        const float  btnW    = h + 2.0f;
        // SpanAvailWidth のためnodeMax.x = ウィンドウコンテンツ右端
        const float  rx      = nodeMax.x;

        // ヒット判定 (DrawList ボタンは ImGui のアイテム系から独立して判定)
        const ImVec2 visMin  = { rx - btnW * 2.0f, nodeMin.y };
        const ImVec2 visMax  = { rx - btnW,         nodeMax.y };
        const ImVec2 lockMin = { rx - btnW,          nodeMin.y };
        const ImVec2 lockMax = { rx,                 nodeMax.y };

        const bool visHov  = ImGui::IsMouseHoveringRect(visMin,  visMax,  false);
        const bool lockHov = ImGui::IsMouseHoveringRect(lockMin, lockMax, false);
        const bool visClick  = visHov  && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool lockClick = lockHov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        if (visClick)  deferred = [&ctx, id]() {
            if (auto* g = ctx.activeScene->GetGameObject(id)) g->SetActive(!g->activeSelf());
        };
        if (lockClick) ctx.ToggleLock(id);

        ImDrawList* dl = ImGui::GetWindowDrawList();

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
        const bool iconAreaClick = visClick || lockClick;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()
            && !isLocked && !iconAreaClick) {
            ctx.selectedAssetPath.clear();
            if (!ImGui::GetIO().KeyCtrl)
                ctx.selectedEntities.clear();
            auto it = std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id);
            if (it != ctx.selectedEntities.end())
                ctx.selectedEntities.erase(it);
            else
                ctx.selectedEntities.push_back(id);
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
                if (dragged && target && dragged->SetParent(target)) {
                    ctx.selectedEntities = { draggedId };
                    if (pendingExpand) *pendingExpand = id;  // 次フレームで親を open
                }
            };
        }
        std::string assetPath;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath) &&
            util::FileSystem::GetExtension(assetPath) == ".fbzzprefab") {
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
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem()) {
        ctx.selectedEntities = { id };

        if (ImGui::MenuItem(isActive ? "Hide" : "Show"))
            deferred = [&ctx, id]() {
                if (auto* g = ctx.activeScene->GetGameObject(id)) g->SetActive(!g->activeSelf());
            };
        if (ImGui::MenuItem(isLocked ? "Unlock" : "Lock"))
            ctx.ToggleLock(id);
        ImGui::Separator();

        if (ImGui::BeginMenu("Create")) {
            DrawCreateObjectMenu(ctx, deferred);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate")) {
            std::string      srcName      = go.name;
            std::string      srcTag       = go.tag;
            int              srcLayer     = go.layer;
            scene::Transform srcTransform = go.transform;
            deferred = [&ctx, id, srcName, srcTag, srcLayer, srcTransform]() {
                auto& dst     = ctx.activeScene->CreateGameObject(srcName + " (Clone)");
                dst.tag       = srcTag;
                dst.layer     = srcLayer;
                dst.transform = srcTransform;
                ctx.activeScene->DuplicateComponents(id, dst.GetID());
                ctx.selectedEntities = { dst.GetID() };
            };
        }
        if (ImGui::MenuItem("Save As Prefab")) {
            SaveSelectedAsPrefab(ctx, go.name);
        }
        if (ImGui::BeginMenu("Hierarchy")) {
            const bool hasParent = go.GetParent() != nullptr;
            if (ImGui::MenuItem("Set As Root", nullptr, false, hasParent))
                deferred = [&ctx, id]() {
                    if (auto* target = ctx.activeScene->GetGameObject(id))
                        target->ClearParent();
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
            deferred = [&ctx, id]() {
                ctx.activeScene->DestroyGameObject(id);
                RemoveSelection(ctx, id);
                PruneSelection(ctx);
            };
        }
        ImGui::EndPopup();
    }

    if (hasChildren) {
        if (opened) {
            for (int i = 0; i < go.GetChildCount(); ++i) {
                if (auto* child = go.GetChild(i))
                    DrawHierarchyNode(ctx, *child, 0, rootCount, visited, deferred, pendingExpand);
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

} // namespace

void SceneHierarchyPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        return;
    }

    // --- 検索バー ---
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##hierarchy_search", "Search...", m_searchFilter, sizeof(m_searchFilter));

    // フィルタが有効なときはフラットリストで一致オブジェクトだけ表示する
    if (m_searchFilter[0] != '\0') {
        std::function<void()> deferred;

        // 大文字小文字を無視した部分一致
        auto contains = [&](const std::string& name) {
            return util::StringUtils::ContainsCI(name, m_searchFilter);
        };

        for (auto& go : ctx.activeScene->GameObjects()) {
            if (!contains(go.name)) continue;
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
                // 検索結果でも Prefab 化できるよう、通常モードと同じ操作を提供する
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

        if (deferred) {
            deferred();
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        return;
    }

    // deferred: ノード描画ループ内でシーンを変更すると、その後のイテレーションで
    // ポインタが無効になる。変更操作 (生成/削除/親付け) はすべてラムダに包み、
    // ループ終了後にまとめて実行する。
    std::function<void()> deferred;
    std::vector<scene::EntityID> visited;
    visited.reserve(ctx.activeScene->GameObjectCount());
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
        if (roots[i]) DrawHierarchyNode(ctx, *roots[i], i, rootCount, visited, deferred, &m_pendingExpand);

    // 親がいないのに GetRootGameObjects に含まれなかった孤立オブジェクトを救済する。
    // 正常なシーンでは実行されない。
    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!ContainsEntity(visited, go.GetID()) && go.GetParent() == nullptr)
            DrawHierarchyNode(ctx, go, 0, 0, visited, deferred, &m_pendingExpand);
    }

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
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath) &&
            util::FileSystem::GetExtension(assetPath) == ".fbzzprefab") {
            deferred = [&ctx, assetPath]() {
                std::vector<scene::EntityID> roots;
                if (PrefabSerializer::Instantiate(*ctx.activeScene, assetPath, roots))
                    ctx.selectedEntities = roots;
            };
        }
        ImGui::EndDragDropTarget();
    }

    if (!deferred && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete) && !ctx.selectedEntities.empty()) {
        std::vector<scene::EntityID> ids = ctx.selectedEntities;
        deferred = [&ctx, ids]() { DestroySelected(ctx, ids); };
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered())
        ctx.selectedEntities.clear();

    if (ImGui::BeginPopupContextWindow("##scene_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        DrawCreateObjectMenu(ctx, deferred);
        ImGui::EndPopup();
    }

    if (deferred) {
        deferred();
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }
}

} // namespace fbzz::editor
