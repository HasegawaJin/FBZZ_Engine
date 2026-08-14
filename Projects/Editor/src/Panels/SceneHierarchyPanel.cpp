// FBZZ Engine
// SceneHierarchyPanel.cpp | fbzz::editor
// Scene GameObject hierarchy and selection editing
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ColliderFit.hpp>
#include <Editor/Util/ModelPlacement.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/SelectionVisuals.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
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
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

namespace {

// ExecuteSceneEditWithUndo は SceneEditUtils.hpp へ移動 (Scene Viewport と共有するため)

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

// GameObject 生成テンプレートは Editor/Util/ObjectPresets.hpp へ移動 (AI の preset.create と共有するため)
// RemoveSelection / PruneSelection / DestroySelected / DuplicateHierarchyRecursive は
// SceneEditUtils.hpp へ移動 (Scene Viewport の Delete / Ctrl+D と共有するため)

// parentId が有効な場合は新規 GO を parentId の子として生成する。
//
// メニューの中身は ObjectPresets.hpp の登録表から組み立てる。
// WHY: 以前はここに ImGui::MenuItem とコンポーネント構成が直接書かれていたため、
//      (1) プリセットを増やすたびにネスト構造を手で書き足す、(2) AI からは同じものを
//      1 個も作れない (Command Bus はこの関数を通れない)、という状態だった。
//      表から描くことで、追加したプリセットがメニューと AI の両方へ同時に現れる。
void DrawCreateObjectMenu(EditorContext& ctx, std::function<void()>& deferred,
                          scene::EntityID parentId = {})
{
    std::string_view openCategory;   // 今 BeginMenu している見出し (空 = 開いていない)
    bool             categoryOpen = false;

    auto closeCategory = [&]() {
        if (categoryOpen) { ImGui::EndMenu(); categoryOpen = false; }
        openCategory = {};
    };

    for (const ObjectPreset& preset : ObjectPresetCatalog()) {
        if (preset.category != openCategory) {
            closeCategory();
            openCategory = preset.category;
            // カテゴリ空文字は Add Object 直下へ置く (Empty / Camera)。
            categoryOpen = !preset.category.empty()
                && ImGui::BeginMenu(std::string(preset.category).c_str());
            // BeginMenu が false = 折り畳まれている。この見出しの項目はまとめて描かない。
            if (!preset.category.empty() && !categoryOpen) continue;
        } else if (!preset.category.empty() && !categoryOpen) {
            continue; // 閉じている見出しの続き
        }

        if (ImGui::MenuItem(std::string(preset.label).c_str())) {
            const std::string presetId(preset.id);
            deferred = [&ctx, presetId, parentId]() {
                CreateObjectFromPreset(ctx, presetId, parentId);
            };
        }
        if (!preset.description.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", std::string(preset.description).c_str());
    }
    closeCategory();

    // Assets/Prefabs にある .prefab ファイルをメニューからインスタンス化できる。
    // WHY: AssetBrowser からのドラッグ操作なしで Prefab を配置できる動線を用意する。
    //      ListAll は存在しないディレクトリに対して空リストを返すため、事前チェック不要。
    if (ImGui::BeginMenu("Prefab")) {
        const std::string assetRoot = ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
        const std::string prefabDir = assetRoot + "/Prefabs";

        bool anyFound = false;
        for (const auto& path : util::FileSystem::ListAll(prefabDir)) {
            // 拡張子を小文字で比較して .prefab だけを列挙する
            const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
            if (ext != ".prefab") continue;

            anyFound = true;
            const std::string name = util::FileSystem::GetFilename(path);
            if (ImGui::MenuItem(name.c_str())) {
                deferred = [&ctx, path]() {
                    ExecuteSceneEditWithUndo(ctx, "Instantiate Prefab", [&ctx, path]() {
                        std::vector<scene::EntityID> roots;
                        if (PrefabSerializer::Instantiate(*ctx.activeScene, path, roots))
                            ctx.selectedEntities = roots;
                    });
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
    std::string path = base + ".prefab";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".prefab";
    return path;
}

void SaveSelectedAsPrefab(EditorContext& ctx, const std::string& objectName)
{
    if (!ctx.activeScene || ctx.selectedEntities.empty()) return;
    scene::Scene& scene = *ctx.activeScene;

    // 保存 + インスタンス接続をまとめて実行する (Unity 互換の Create Prefab)。
    // 接続したルート GO は rootSelection に返り、Undo でファイル削除と接続解除に使う。
    const std::string path = UniquePrefabPath(ctx, objectName);
    std::vector<scene::EntityID> rootSelection;
    if (!PrefabSerializer::SaveSelectionAndConnect(scene, ctx.selectedEntities, path, rootSelection))
        return;
    const std::string relPath = NormalizeAssetPath(path);

    if (ctx.undoStack) {
        std::string content;
        util::FileSystem::ReadText(path, content);
        EditorContext* context = &ctx;
        const std::vector<scene::EntityID> roots = rootSelection;
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Create Prefab",
            [path, content, relPath, roots, context]() {
                util::FileSystem::WriteText(path, content);
                if (context->activeScene)
                    for (scene::EntityID id : roots)
                        if (auto* go = context->activeScene->GetGameObject(id))
                            go->prefabAssetPath = relPath;
                context->requestAssetBrowserRefresh = true;
            },
            [path, roots, context]() {
                util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(path));
                if (context->activeScene)
                    for (scene::EntityID id : roots)
                        if (auto* go = context->activeScene->GetGameObject(id))
                            go->prefabAssetPath.clear();
                context->requestAssetBrowserRefresh = true;
            }));
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
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

// ランタイム生成 GO を Hierarchy から隠すか。ctx.showGeneratedObjects が真なら全部見せる。
// WHY: 隠す判断を 1 箇所に集約する。ツリー・検索リスト・展開矢印の 3 箇所が
//      別々の条件を持つと、「矢印は出るが開いても空」のような不整合が必ず起きる。
bool IsHiddenGenerated(const EditorContext& ctx, const scene::GameObject& go)
{
    return go.runtimeGenerated && !ctx.showGeneratedObjects;
}

// 実際に描画される子の数。生成物を隠しているときは、それを除いた数を返す。
// WHY: GetChildCount() をそのまま展開矢印の有無に使うと、子が生成物だけのノードで
//      「開けるのに何も出ない」状態になる。
int VisibleChildCount(const EditorContext& ctx, const scene::GameObject& go)
{
    int count = 0;
    for (int i = 0; i < go.GetChildCount(); ++i) {
        auto* child = go.GetChild(i);
        if (child && !IsHiddenGenerated(ctx, *child)) ++count;
    }
    return count;
}

// 隠している生成物の総数 (子孫すべて)。行末のバッジに出し、
// 「Hierarchy に出ていない = 存在しない」と誤解させないための表示。
int HiddenGeneratedCount(const EditorContext& ctx, const scene::GameObject& go)
{
    int count = 0;
    for (int i = 0; i < go.GetChildCount(); ++i) {
        auto* child = go.GetChild(i);
        if (!child) continue;
        if (IsHiddenGenerated(ctx, *child)) {
            ++count;
            count += child->GetChildCount(); // 生成物の下はまとめて 1 段だけ数える
        } else {
            count += HiddenGeneratedCount(ctx, *child);
        }
    }
    return count;
}

// F2 インラインリネームの状態一式 (パネルメンバーへの参照)
struct InlineRenameState {
    scene::EntityID& id;        // リネーム対象 (無効 = リネーム中でない)
    char*            buffer;
    size_t           bufferSize;
    bool&            focusPending;
};

void DrawHierarchyNode(EditorContext& ctx,
                       scene::GameObject& go,
                       size_t rootIndex,          // roots 配列内のインデックス（Order メニュー用, root のみ有効）
                       size_t rootCount,
                       std::vector<scene::EntityID>& visited,
                       std::function<void()>& deferred,
                       scene::EntityID* pendingExpand,
                       scene::EntityID& lastClicked,
                       std::vector<scene::EntityID>& outVisible,
                       const std::vector<scene::EntityID>& prevVisible,
                       InlineRenameState rename)
{
    const scene::EntityID id = go.GetID();
    if (ContainsEntity(visited, id)) return;
    // ランタイム生成物は既定で描かない。visited へは入れて、第2ループが
    // これを「孤立オブジェクト」と誤認して root レベルへ描くのを防ぐ。
    if (IsHiddenGenerated(ctx, go)) {
        visited.push_back(id);
        MarkDescendantsVisited(go, visited);
        return;
    }
    visited.push_back(id);
    outVisible.push_back(id);

    // --- F2 インラインリネーム: 行をそのまま入力欄に置き換える (Unity 互換) ---
    // WHY: 以前は画面隅のポップアップで名前を編集していて、視線移動とマウス移動が
    //      毎回発生していた。その場で書き換えられる方が圧倒的に速い。
    if (rename.id == id) {
        ImGui::PushID(static_cast<int>(id.index));
        ImGui::SetNextItemWidth(-1.0f);
        if (rename.focusPending) {
            ImGui::SetKeyboardFocusHere();
            rename.focusPending = false;
        }
        const bool confirmed = ImGui::InputText("##inline_rename", rename.buffer,
            rename.bufferSize,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            rename.id = scene::EntityID{};
        } else if (confirmed || ImGui::IsItemDeactivated()) {
            // Enter またはフォーカス喪失で確定する
            const std::string newName = rename.buffer;
            if (!newName.empty() && newName != go.name) {
                const scene::EntityID rid = id;
                ExecuteSceneEditWithUndo(ctx, "Rename GameObject", [&ctx, rid, newName]() {
                    if (auto* g = ctx.activeScene->GetGameObject(rid))
                        g->name = newName;
                });
            }
            rename.id = scene::EntityID{};
        }
        // リネーム中は子ツリーを描かない (visited へは登録して二重描画を防ぐ)
        MarkDescendantsVisited(go, visited);
        ImGui::PopID();
        return;
    }

    const bool hasChildren    = VisibleChildCount(ctx, go) > 0;
    const bool selected       = ContainsEntity(ctx.selectedEntities, id);
    const bool isRoot         = go.GetParent() == nullptr;
    const bool isActive       = go.activeInHierarchy();
    const bool isLocked       = ctx.IsLocked(id);
    const bool isEditorHidden  = ctx.editorHiddenGuids.count(go.instanceId) > 0;
    const bool isPrimarySelected = ctx.PrimarySelected() == id;
    // WHY: Prefab インスタンスを青色で識別することで、通常 GO とプレファブ出来の GO を
    //      視覚的に区別できる (Unity の Hierarchy 表示と同等の UX)。
    const bool isPrefabInstance = !go.prefabAssetPath.empty();

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

    // エディタ専用非表示はシアン（runtime 非アクティブより優先）、非アクティブはグレー、
    // ロック中はオレンジ、プレファブインスタンスは水色
    ui::PushHierarchySelectionColors();
    if (isEditorHidden)         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Info));
    else if (!isActive)         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
    else if (isLocked)          ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
    else if (isPrefabInstance)  ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Accent));
    const bool opened = ImGui::TreeNodeEx(go.name.c_str(), flags);
    if (isEditorHidden || !isActive || isLocked || isPrefabInstance) ImGui::PopStyleColor();
    ui::PopHierarchySelectionColors();

    // ノード本体のアイテム状態は TreeNodeEx 直後に確定させ、以降で使い回す。
    // WHY: この後に描く「隠し生成物」バッジ (TextDisabled) が新しい item として
    //      ImGui の LastItemData を上書きしてしまい、それより後段で呼ぶ
    //      IsItemClicked()/GetItemRectMin() 等がバッジ側を指してしまっていた。
    //      バッジを持つノード(=ランタイム生成物を子孫に持つノード)ほど
    //      「クリックしても選択されない/矩形がずれてアイコン誤動作する」不具合の原因だった。
    const ImVec2 nodeMin         = ImGui::GetItemRectMin();
    const ImVec2 nodeMax         = ImGui::GetItemRectMax();
    const bool   nodeHovered     = ImGui::IsItemHovered();
    const bool   nodeClicked     = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool   nodeToggledOpen = ImGui::IsItemToggledOpen();

    // 隠している生成物の件数バッジ。「Hierarchy に出ていない = 存在しない」ではないことを示す。
    // WHY: VFX を再生すると裏では十数個の GameObject が動いている。行が増えないのは
    //      正しい既定だが、何も出さないと「エフェクトが生成されていない」と誤診断させる。
    if (const int hiddenCount = HiddenGeneratedCount(ctx, go); hiddenCount > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(+%d)", hiddenCount);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%d generated object(s) hidden\nView > Show Generated Objects to reveal",
                              hiddenCount);
    }

    // --- 右端 visibility/editor-hide/lock アイコン (DrawList で直接描画) ---
    {
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
        ui::DrawSelectionAccent(dl, nodeMin, nodeMax, selected, isPrimarySelected);

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

        // prefab インスタンスバッジ (右端 4 番目スロット, 常時表示の水色ダイヤ)
        // WHY: 青いテキストだけだと非アクティブ(グレー)や選択ハイライトと重なった際に
        //      プレファブ由来か判別しづらい。Unity のプレファブアイコンに相当する常設マーカーを
        //      置き、ホバーで参照パスをツールチップ表示して出所を即座に確認できるようにする。
        if (isPrefabInstance) {
            const ImVec2 pfMin = { rx - btnW * 4.0f, nodeMin.y };
            const ImVec2 pfMax = { rx - btnW * 3.0f, nodeMax.y };
            const ImVec2 pfCenter = { (pfMin.x + pfMax.x) * 0.5f, (pfMin.y + pfMax.y) * 0.5f };
            const float  pfRadius = h * 0.22f;
            // 外周を少し濃く、内側を塗って小さなダイヤ(菱形)にする
            dl->AddNgonFilled(pfCenter, pfRadius, IM_COL32(100, 180, 255, 235), 4);
            dl->AddNgon(pfCenter, pfRadius, IM_COL32(40, 90, 150, 235), 4, 1.0f);
            if (ImGui::IsMouseHoveringRect(pfMin, pfMax, false))
                ImGui::SetTooltip("Prefab instance\n%s", go.prefabAssetPath.c_str());
        }

        // ノードのクリック/ダブルクリック判定 (アイコン領域は除外)
        const bool iconAreaClick = ehClick || visClick || lockClick;
        if (nodeClicked && !nodeToggledOpen
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
        if (nodeHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
            && !isLocked && !iconAreaClick) {
            // WHY: メッシュバウンズ基準で注視点・距離を決める (Unity の Frame Selected 互換)
            math::Vector3 focusCenter = go.transform.position;
            float focusRadius = 0.0f;
            ComputeGameObjectBounds(go, focusCenter, focusRadius);
            ctx.focusTargetPosition    = focusCenter;
            ctx.focusTargetRadius      = focusRadius;
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
            const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(assetPath));
            if (ext == ".fbx") {
                deferred = [&ctx, assetPath, id, pendingExpand]() {
                    // .fbx は未インポートでもその場で自動インポートして配置する (Unity 流)。
                    const std::string modelPath = ResolveOrImportFbxModel(assetPath);
                    if (modelPath.empty()) return;
                    const scene::EntityID rootId = SpawnModelAssetHierarchy(ctx, modelPath, nullptr, id);
                    if (rootId == scene::EntityID::INVALID) return;
                    ctx.selectedEntities = { rootId };
                    if (pendingExpand) *pendingExpand = id;
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                };
            } else if (ext == ".prefab") {
                deferred = [&ctx, assetPath, id, pendingExpand]() {
                    ExecuteSceneEditWithUndo(ctx, "Instantiate Prefab", [&ctx, assetPath, id]() {
                        std::vector<scene::EntityID> roots;
                        if (!PrefabSerializer::Instantiate(*ctx.activeScene, assetPath, roots)) return;
                        for (scene::EntityID rootId : roots) {
                            auto* root = ctx.activeScene->GetGameObject(rootId);
                            auto* parent = ctx.activeScene->GetGameObject(id);
                            if (root && parent) root->SetParent(parent);
                        }
                        ctx.selectedEntities = roots;
                    });
                    if (pendingExpand) *pendingExpand = id;
                };
            } else if (ext == ".mat") {
                // Viewport 上のドロップと同じく、ドロップ先の GameObject (このノード自身) へ
                // マテリアルを直接割り当てる (Unity と同じ操作感)。
                deferred = [&ctx, assetPath, id]() {
                    auto* go = ctx.activeScene->GetGameObject(id);
                    if (!go) return;
                    auto* mc = go->GetComponent<scene::MaterialComponent>();
                    if (!mc) mc = &go->AddComponent<scene::MaterialComponent>();
                    mc->materialPath = assetPath;
                    mc->materialAsset = {};  // 次フレームの SyncMaterial に新パスを再解決させる
                    ctx.selectedEntities = { id };
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                };
            }
        }
        ImGui::EndDragDropTarget();
    }

    // --- 兄弟間並べ替え: ノード下端の細い帯を「この直後に挿入」ドロップ先にする ---
    // WHY: ノード本体へのドロップは「子にする」操作。Unity と同じ境界線ドロップが無いと、
    //      並び順の変更に Order メニュー (ルート限定) を往復する羽目になる。
    if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
        dragging && dragging->IsDataType("FBZZ_HIERARCHY_ENTITY")) {
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        constexpr float kBandHalf = 3.0f;  // 挿入帯の半分の高さ (px)
        const ImRect band({ rMin.x, rMax.y - kBandHalf }, { rMax.x, rMax.y + kBandHalf });
        const ImGuiID bandId = ImGui::GetID("##reorder_after");
        if (ImGui::BeginDragDropTargetCustom(band, bandId)) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    "FBZZ_HIERARCHY_ENTITY",
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                // ドロップ位置プレビューの挿入ライン
                ImGui::GetWindowDrawList()->AddLine(
                    { rMin.x, rMax.y }, { rMax.x, rMax.y }, IM_COL32(100, 180, 255, 255), 2.0f);

                scene::EntityID draggedId;
                if (payload->IsDelivery() && ReadEntityPayload(payload, draggedId) && draggedId != id) {
                    deferred = [&ctx, draggedId, id]() {
                        auto* dragged = ctx.activeScene->GetGameObject(draggedId);
                        auto* anchor  = ctx.activeScene->GetGameObject(id);
                        if (!dragged || !anchor) return;
                        if (anchor->IsDescendantOf(*dragged)) return;  // 自分の子孫の隣には置けない

                        // anchor と同じ親に揃えてから、anchor の直後へ挿入する
                        scene::GameObject* parent = anchor->GetParent();
                        if (dragged->GetParent() != parent) {
                            const bool ok = parent ? dragged->SetParent(parent)
                                                   : dragged->ClearParent();
                            if (!ok) return;
                        }
                        // 一旦末尾へ送って index 計算を単純化する
                        dragged->SetSiblingIndex(1 << 30);
                        dragged->SetSiblingIndex(anchor->GetSiblingIndex() + 1);
                        ctx.selectedEntities = { draggedId };
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    };
                }
            }
            ImGui::EndDragDropTarget();
        }
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
            // 親付けは CreateObjectFromPreset が生成直後に行う。
            // WHY: 以前は「生成後の選択」を頼りに親付けしていたため、複数 GO を作る
            //      プリセット (Button + Label) では選択されている方が親になり得た。
            DrawCreateObjectMenu(ctx, deferred, id);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
            CopySelectedToClipboard(ctx);
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, HasGameObjectClipboard()))
            deferred = [&ctx]() { PasteClipboardWithUndo(ctx); };
        if (ImGui::MenuItem("Paste As Child", "Ctrl+Shift+V", false, HasGameObjectClipboard()))
            deferred = [&ctx, id]() { PasteClipboardWithUndo(ctx, id); };
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
        // プレファブインスタンスには Apply / Revert を提供する。
        // WHY: Unity 互換の Prefab 操作性。Apply はディスクへの書き戻しのみで
        //      シーンは変わらないため Undo なし。Revert はシーンを変更するため
        //      ExecuteSceneEditWithUndo が自動的にスナップショット Undo を生成する。
        if (!go.prefabAssetPath.empty()) {
            ImGui::Separator();
            if (ImGui::MenuItem("Apply to Prefab")) {
                deferred = [&ctx, id]() {
                    if (!ctx.activeScene) return;
                    auto* target = ctx.activeScene->GetGameObject(id);
                    if (!target) return;
                    const std::string prefabPath = target->prefabAssetPath;
                    if (!PrefabSerializer::Apply(*ctx.activeScene, id, ctx.projectRoot)) return;
                    // WHY: アセットを書き戻しただけでは配置済みの他インスタンスが
                    //      古い定義のまま残る。Inspector の Apply と挙動を揃える。
                    //      各インスタンスの個別調整 (override) は保持される。
                    const int updated = PrefabSerializer::PropagateToInstances(
                        *ctx.activeScene, prefabPath, id, ctx.projectRoot);
                    if (updated > 0) {
                        // 作り直しで EntityID が変わるため選択は捨てる。
                        ctx.selectedEntities.clear();
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }
                    ctx.requestAssetBrowserRefresh = true;
                };
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Write instance state back to the .prefab asset,\n"
                                  "then update every other instance in this scene");
            if (ImGui::MenuItem("Revert from Prefab")) {
                deferred = [&ctx, id]() {
                    if (!ctx.activeScene) return;
                    ExecuteSceneEditWithUndo(ctx, "Revert Prefab", [&ctx, id]() {
                        std::vector<scene::EntityID> newRoots;
                        if (PrefabSerializer::Revert(*ctx.activeScene, id, newRoots, ctx.projectRoot))
                            if (!newRoots.empty()) ctx.selectedEntities = newRoots;
                    });
                };
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Discard instance changes and restore from the source .prefab asset");
            if (ImGui::MenuItem("Open Prefab")) {
                const std::string prefabPath = go.prefabAssetPath;
                deferred = [&ctx, prefabPath]() { ctx.requestOpenPrefabEdit = prefabPath; };
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Edit the .prefab itself in isolation — every instance follows on save");
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
                    DrawHierarchyNode(ctx, *child, 0, rootCount, visited, deferred, pendingExpand, lastClicked, outVisible, prevVisible, rename);
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

void SceneHierarchyPanel::PublishFocusAndHandleRequests(EditorContext& ctx)
{
    // 次フレームの HotkeyManager が Hierarchy 用のキーを受け付けるかの判定材料。
    ctx.hierarchyFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (!ctx.requestRenameSelected) return;
    ctx.requestRenameSelected = false;

    if (!ctx.activeScene || ctx.selectedEntities.size() != 1) return;
    auto* go = ctx.activeScene->GetGameObject(ctx.selectedEntities[0]);
    if (!go) return;

    m_renamingId = ctx.selectedEntities[0];
    std::strncpy(m_renameBuffer, go->name.c_str(), sizeof(m_renameBuffer) - 1);
    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
    m_renameFocusPending = true;
}

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
            ImGui::SetTooltip("Show only Terrain Grid, Terrain, Water, Detail, Foliage and their children\nUncheck to browse all objects in Map Mode");
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
    // 生成物トグルを右端へ置き、検索欄はその残り幅を使う。
    // WHY: 「見えているのが全部か」は Hierarchy を読むうえでの前提なので、
    //      メニューの奥ではなく常に目に入る位置へ出す。
    {
        const float toggleWidth = ImGui::CalcTextSize("Generated").x
                                + ImGui::GetFrameHeight()
                                + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SetNextItemWidth(-toggleWidth);
        ImGui::InputTextWithHint("##hierarchy_search", "Search...", m_searchFilter, sizeof(m_searchFilter));
        ImGui::SameLine();
        ImGui::Checkbox("Generated", &ctx.showGeneratedObjects);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Show objects created at runtime by systems\n"
                              "(VFX Graph nodes, foliage bake, water splash).\n"
                              "These are never saved into the scene.");
    }

    // 空状態ガイド: シーンにオブジェクトが無いときは作成導線を案内する。
    if (ctx.activeScene->GameObjectCount() == 0 && m_searchFilter[0] == '\0') {
        ImGui::Spacing();
        ImGui::TextDisabled("Scene is empty");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Right-click here to create objects, or drag a model / prefab from the Asset Browser into the Scene.");
        ImGui::PopTextWrapPos();
    }

    // F2 リネームはツリー内インライン編集 (DrawHierarchyNode / 検索リスト側) で行う

    // 検索フィルタが有効なときはフラットリストで一致オブジェクトだけ表示する
    if (m_searchFilter[0] != '\0') {
        std::function<void()> deferred;

        for (auto& go : ctx.activeScene->GameObjects()) {
            if (!util::StringUtils::ContainsCI(go.name, m_searchFilter)) continue;
            // ツリー表示と同じ基準で隠す。検索したときだけ生成物が現れると、
            // 「同じ名前の GO が 2 つある」ように見えて混乱する。
            if (IsHiddenGenerated(ctx, go)) continue;
            if (ctx.mapEditingMode && ctx.mapHierarchyFilter) {
                const bool isMapObject =
                    go.GetComponent<scene::TerrainGridComponent>()
                    || go.GetComponent<scene::TerrainComponent>()
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

            // F2 インラインリネーム (検索リスト側)
            if (m_renamingId == id) {
                ImGui::SetNextItemWidth(-1.0f);
                if (m_renameFocusPending) {
                    ImGui::SetKeyboardFocusHere();
                    m_renameFocusPending = false;
                }
                const bool confirmed = ImGui::InputText("##inline_rename", m_renameBuffer,
                    sizeof(m_renameBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_renamingId = {};
                } else if (confirmed || ImGui::IsItemDeactivated()) {
                    const std::string newName = m_renameBuffer;
                    if (!newName.empty() && newName != go.name) {
                        const scene::EntityID rid = id;
                        ExecuteSceneEditWithUndo(ctx, "Rename GameObject", [&ctx, rid, newName]() {
                            if (auto* g = ctx.activeScene->GetGameObject(rid))
                                g->name = newName;
                        });
                    }
                    m_renamingId = {};
                }
                ImGui::PopID();
                continue;
            }

            ui::PushHierarchySelectionColors();
            if (ImGui::Selectable(go.name.c_str(), selected)) {
                if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
                if (selected)
                    RemoveSelection(ctx, id);
                else
                    ctx.selectedEntities.push_back(id);
            }
            ui::PopHierarchySelectionColors();
            ui::DrawSelectionAccent(ImGui::GetWindowDrawList(),
                                    ImGui::GetItemRectMin(),
                                    ImGui::GetItemRectMax(),
                                    selected,
                                    ctx.PrimarySelected() == id);
            if (ImGui::BeginPopupContextItem()) {
                ctx.selectedEntities = { id };
                if (ImGui::MenuItem("Copy", "Ctrl+C"))
                    CopySelectedToClipboard(ctx);
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, HasGameObjectClipboard()))
                    deferred = [&ctx]() { PasteClipboardWithUndo(ctx); };
                if (ImGui::MenuItem("Paste As Child", "Ctrl+Shift+V", false, HasGameObjectClipboard()))
                    deferred = [&ctx, id]() { PasteClipboardWithUndo(ctx, id); };
                ImGui::Separator();
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

        PublishFocusAndHandleRequests(ctx);
        if (deferred)
            ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", deferred);
        return;
    }

    // Map Mode: TerrainGrid/Terrain/Water/Detail/Foliage のルート GO のみをツリー表示。
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
            if (!go->GetComponent<scene::TerrainGridComponent>()
                && !go->GetComponent<scene::TerrainComponent>()
                && !go->GetComponent<scene::WaterComponent>()
                && !go->GetComponent<scene::TerrainDetailComponent>()
                && !go->GetComponent<scene::FoliageComponent>())
                continue;
            DrawHierarchyNode(ctx, *go, 0, rootCount, visited, deferred, &m_pendingExpand,
                m_lastClickedEntity, mapVisible, m_visibleOrder,
                InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });
        }

        PublishFocusAndHandleRequests(ctx);
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
            &m_pendingExpand, m_lastClickedEntity, outVisible, m_visibleOrder,
            InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });

    // 親がいないのに GetRootGameObjects に含まれなかった孤立オブジェクトを救済する。
    // 正常なシーンでは実行されない。
    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!ContainsEntity(visited, go.GetID()) && go.GetParent() == nullptr)
            DrawHierarchyNode(ctx, go, 0, 0, visited, deferred,
                &m_pendingExpand, m_lastClickedEntity, outVisible, m_visibleOrder,
                InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });
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
            const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(assetPath));
            if (ext == ".fbx") {
                deferred = [&ctx, assetPath]() {
                    // .fbx は未インポートでもその場で自動インポートして配置する (Unity 流)。
                    const std::string modelPath = ResolveOrImportFbxModel(assetPath);
                    if (modelPath.empty()) return;
                    const scene::EntityID rootId = SpawnModelAssetHierarchy(ctx, modelPath);
                    if (rootId != scene::EntityID::INVALID) {
                        ctx.selectedEntities = { rootId };
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }
                };
            } else if (ext == ".prefab") {
                deferred = [&ctx, assetPath]() {
                    ExecuteSceneEditWithUndo(ctx, "Instantiate Prefab", [&ctx, assetPath]() {
                        std::vector<scene::EntityID> roots;
                        if (PrefabSerializer::Instantiate(*ctx.activeScene, assetPath, roots))
                            ctx.selectedEntities = roots;
                    });
                };
            }
        }
        ImGui::EndDragDropTarget();
    }

    PublishFocusAndHandleRequests(ctx);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered()) {
        ctx.selectedEntities.clear();
        m_lastClickedEntity = {};
    }

    if (ImGui::BeginPopupContextWindow("##scene_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            ctx.selectedEntities.clear();
            for (auto& go : ctx.activeScene->GameObjects())
                if (!ctx.IsLocked(go.GetID()))
                    ctx.selectedEntities.push_back(go.GetID());
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, HasGameObjectClipboard()))
            deferred = [&ctx]() { PasteClipboardWithUndo(ctx); };
        ImGui::Separator();
        DrawCreateObjectMenu(ctx, deferred);
        ImGui::EndPopup();
    }

    if (deferred) {
        ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", deferred);
    }
}

} // namespace fbzz::editor
