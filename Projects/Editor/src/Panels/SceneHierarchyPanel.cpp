/// @file    SceneHierarchyPanel.cpp
/// @brief   Scene GameObject hierarchy and selection editing.
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Editor/Panels/SceneHierarchyPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ColliderFit.hpp>
#include <Editor/Util/CreateObjectMenu.hpp>
#include <Editor/Util/DragDropSet.hpp>
#include <Editor/Util/ModelPlacement.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/SelectionVisuals.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
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
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

namespace {

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

/// @brief ドラッグで運ばれている GameObject のうち、祖先が同じ集合に居ないものを行の並び順で返す。
/// @param order 行の並び (前フレームの表示順)。載っていないものは末尾へ回す。
/// @note 祖先ごと動くものまで個別に付け替えると、選択内の親子関係が平らに崩れる。
std::vector<scene::EntityID> DraggedRoots(scene::Scene& scene, scene::EntityID payloadId,
                                          const std::vector<scene::EntityID>& order)
{
    const std::vector<scene::EntityID> all = dragdrop::DraggedEntities(payloadId);
    std::vector<scene::EntityID> roots;
    for (scene::EntityID id : all) {
        const scene::GameObject* go = scene.GetGameObject(id);
        if (!go) continue;
        bool ancestorDragged = false;
        for (const scene::GameObject* p = go->GetParent(); p && !ancestorDragged; p = p->GetParent())
            ancestorDragged = ContainsEntity(all, p->GetID());
        if (!ancestorDragged) roots.push_back(id);
    }
    const auto rank = [&order](scene::EntityID id) {
        return static_cast<size_t>(std::find(order.begin(), order.end(), id) - order.begin());
    };
    std::stable_sort(roots.begin(), roots.end(),
                     [&rank](scene::EntityID a, scene::EntityID b) { return rank(a) < rank(b); });
    return roots;
}

/// @brief 掴んだ 1 件を先頭 (主選択) にして選択を置き換える。
void SelectMoved(EditorContext& ctx, std::vector<scene::EntityID> moved, scene::EntityID grabbed)
{
    if (moved.empty()) return;
    if (const auto it = std::find(moved.begin(), moved.end(), grabbed); it != moved.end())
        std::rotate(moved.begin(), it, it + 1);
    SelectEntities(ctx, std::move(moved), SelectionReveal::Skip);
}

/// @note 子孫を再帰的に visited に追加するだけ（ImGui 呼び出しなし）。
/// @note 親が閉じているとき子が第2ループで誤って root 描画されるのを防ぐ。
std::string SanitizeAssetName(const std::string& name)
{
    std::string result = name.empty() ? "Prefab" : name;
    for (char& c : result) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok) c = '_';
    }
    return result;
}

/// @note .vfx はプレハブと同じ中身だが、AssetBrowser の色分け・サムネイル・VFXRef のドロップ受理・
/// @note 開いたときの振る舞いがすべて拡張子で分岐する。同じ保存経路を通しつつ «これは演出だ»
/// @note という区別だけ呼び出し側が決められるよう拡張子と置き場所を引数にする。
std::string UniqueAssetPath(const EditorContext& ctx, const std::string& objectName,
                            const char* subDirectory, const char* extension)
{
    const std::string assetRoot = ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
    const std::string directory = assetRoot + "/" + subDirectory;
    util::FileSystem::EnsureDirectory(directory);

    const std::string base = directory + "/" + SanitizeAssetName(objectName);
    std::string path = base + extension;
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + extension;
    return path;
}

void SaveSelectedAsAsset(EditorContext& ctx, const std::string& objectName,
                         const char* subDirectory, const char* extension)
{
    if (!ctx.activeScene || ctx.selectedEntities.empty()) return;
    scene::Scene& scene = *ctx.activeScene;

    /// @note 保存 + インスタンス接続をまとめて実行する (Unity 互換の Create Prefab)。
    /// @note 接続したルート GO は rootSelection に返り、Undo で接続解除に使う。
    const std::string path = UniqueAssetPath(ctx, objectName, subDirectory, extension);
    std::vector<scene::EntityID> rootSelection;
    if (!PrefabSerializer::SaveSelectionAndConnect(scene, ctx.selectedEntities, path, rootSelection))
        return;
    const std::string relPath = scene::CanonicalPrefabAssetRef(path);

    /// @note Undo 対象はシーン側のリンク (prefabAssetPath) だけ。.prefab ファイルは残す。
    /// @note ファイル削除まで Undo すると、無関係な作業のあと Ctrl+Z を重ねたときに編集ぶんごと
    /// @note ファイルが消える。消したいときは Asset Browser の Delete でごみ箱へ送る。
    if (ctx.undoStack) {
        EditorContext* context = &ctx;
        const std::vector<scene::EntityID> roots = rootSelection;
        auto applyLink = [context, roots](const std::string& assetPath) {
            if (!context->activeScene) return;
            for (scene::EntityID id : roots)
                if (auto* go = context->activeScene->GetGameObject(id))
                    go->prefabAssetPath = assetPath;
            context->requestAssetBrowserRefresh = true;
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Link Prefab Instance",
            [applyLink, relPath]() { applyLink(relPath); },
            [applyLink]()          { applyLink({}); }));
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

/// @brief アセットのドロップを、遅延編集と生成予約へ振り分ける。複数アセットはまとめて置く。
/// @param parentId 無効ならルートへ置く。.mat はドロップ先の GameObject が要るので有効なときだけ受ける。
/// @param pendingExpand 置いた後に開くノードの書き先。null 可。
void QueueAssetDrop(EditorContext& ctx, const std::string& payloadPath,
                    scene::EntityID parentId, const std::string& parentGuid,
                    std::function<void()>& deferred, PendingObjectCreate& pendingCreate,
                    scene::EntityID* pendingExpand)
{
    std::vector<std::string> fbxPaths;
    std::vector<OpArgs>      instantiate;
    std::string              materialPath;
    for (const std::string& path : dragdrop::DraggedAssetPaths(payloadPath)) {
        const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
        if (ext == ".fbx") {
            fbxPaths.push_back(path);
        } else if (IsInstantiableAssetExtension(ext)) {
            /// @note ノードへのドロップはその子、背景へのドロップはルート。メニューの Create Child / Create at Root と同じ規則。
            OpArgs args;
            args.Set("path", path);
            if (parentId.IsValid()) args.Set("parent", parentGuid);
            else                    args.Set("placeInView", true);
            instantiate.push_back(std::move(args));
        } else if (ext == ".mat" && parentId.IsValid() && materialPath.empty()) {
            /// @note 1 つの GameObject に割り当てられるのは 1 枚なので、複数なら先頭だけ。
            materialPath = path;
        }
    }

    if (!instantiate.empty()) {
        pendingCreate.operatorId  = "prefab.instantiate";
        pendingCreate.beginRename = false;
        pendingCreate.args        = std::move(instantiate.front());
        pendingCreate.moreArgs.assign(std::make_move_iterator(instantiate.begin() + 1),
                                      std::make_move_iterator(instantiate.end()));
    }
    if (fbxPaths.empty() && materialPath.empty()) return;

    deferred = [&ctx, fbxPaths, materialPath, parentId, pendingExpand]() {
        std::vector<scene::EntityID> placed;
        /// @note .fbx は未インポートでもその場で自動インポートして配置する。
        for (const std::string& fbxPath : fbxPaths) {
            const std::string modelPath = ResolveOrImportFbxModel(fbxPath);
            if (modelPath.empty()) continue;
            const scene::EntityID rootId = SpawnModelAssetHierarchy(ctx, modelPath, nullptr, parentId);
            if (rootId != scene::EntityID::INVALID) placed.push_back(rootId);
        }
        if (!materialPath.empty()) {
            if (auto* target = ctx.activeScene->GetGameObject(parentId)) {
                /// @note Viewport へのドロップと同じく、ドロップ先の GameObject へ直接割り当てる。
                auto* mc = target->GetComponent<scene::MaterialComponent>();
                if (!mc) mc = &target->AddComponent<scene::MaterialComponent>();
                mc->materialPath = materialPath;
                /// @note 空にすると次フレームの SyncMaterial が新パスを解決し直す。
                mc->materialAsset = {};
                if (placed.empty()) placed.push_back(parentId);
            }
        }
        if (placed.empty()) return;
        SelectEntities(ctx, std::move(placed), SelectionReveal::Skip);
        if (pendingExpand && parentId.IsValid() && !fbxPaths.empty()) *pendingExpand = parentId;
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    };
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

/// @note ランタイム生成 GO を Hierarchy から隠すか。ctx.showGeneratedObjects が真なら全部見せる。
/// @note 隠す判断を 1 箇所に集約する。ツリー・検索リスト・展開矢印が別々の条件を持つと
/// @note 「矢印は出るが開いても空」のような不整合が必ず起きる。
bool IsHiddenGenerated(const EditorContext& ctx, const scene::GameObject& go)
{
    return go.runtimeGenerated && !ctx.showGeneratedObjects;
}

/// @note 実際に描画される子の数。生成物を隠しているときは、それを除いた数を返す。
/// @note GetChildCount() をそのまま使うと、子が生成物だけのノードで「開けるのに何も出ない」状態になる。
int VisibleChildCount(const EditorContext& ctx, const scene::GameObject& go)
{
    int count = 0;
    for (int i = 0; i < go.GetChildCount(); ++i) {
        auto* child = go.GetChild(i);
        if (child && !IsHiddenGenerated(ctx, *child)) ++count;
    }
    return count;
}

/// @note 隠している生成物の総数 (子孫すべて)。行末のバッジに出し、
/// @note 「Hierarchy に出ていない = 存在しない」と誤解させないための表示。
int HiddenGeneratedCount(const EditorContext& ctx, const scene::GameObject& go)
{
    int count = 0;
    for (int i = 0; i < go.GetChildCount(); ++i) {
        auto* child = go.GetChild(i);
        if (!child) continue;
        if (IsHiddenGenerated(ctx, *child)) {
            ++count;
            /// @note 生成物の下はまとめて 1 段だけ数える
            count += child->GetChildCount();
        } else {
            count += HiddenGeneratedCount(ctx, *child);
        }
    }
    return count;
}

/// @brief F2 インラインリネームの状態一式 (パネルメンバーへの参照)。
struct InlineRenameState {
    scene::EntityID& id;        /// @note リネーム対象 (無効 = リネーム中でない)
    char*            buffer;
    size_t           bufferSize;
    bool&            focusPending;
};

/// @brief ツリーを「開いて見せる」ための状態一式 (パネルメンバーへの参照)。
struct HierarchyRevealState {
    scene::EntityID&              pendingExpand; /// @note SetParent 直後に開くノード
    std::vector<scene::EntityID>& openChain;     /// @note 他の面から選ばれた対象の祖先
    scene::EntityID&              scrollTo;      /// @note 行までスクロールする対象
};

/// @param rootIndex roots 配列内の位置。Order メニュー用で、ルートのときだけ意味を持つ。
/// @param deferred 描画ループの後でスナップショット Undo に包んで実行する編集。
/// @param pendingCreate 描画ループの後で呼ぶ生成 Operator (Undo は Operator が積む)。
void DrawHierarchyNode(EditorContext& ctx,
                       scene::GameObject& go,
                       size_t rootIndex,
                       size_t rootCount,
                       std::vector<scene::EntityID>& visited,
                       std::function<void()>& deferred,
                       PendingObjectCreate& pendingCreate,
                       HierarchyRevealState reveal,
                       scene::EntityID& lastClicked,
                       scene::EntityID& pendingClick,
                       bool& hierarchyDragStarted,
                       bool& hierarchyNodeHovered,
                       std::vector<scene::EntityID>& outVisible,
                       const std::vector<scene::EntityID>& prevVisible,
                       InlineRenameState rename)
{
    scene::EntityID* const pendingExpand = &reveal.pendingExpand;
    const scene::EntityID id = go.GetID();
    if (ContainsEntity(visited, id)) return;
    /// @note ランタイム生成物は既定で描かない。visited へは入れて、第2ループが
    /// @note これを「孤立オブジェクト」と誤認して root レベルへ描くのを防ぐ。
    if (IsHiddenGenerated(ctx, go)) {
        visited.push_back(id);
        MarkDescendantsVisited(go, visited);
        return;
    }
    visited.push_back(id);
    outVisible.push_back(id);

    /// @name F2 インラインリネーム: 行をそのまま入力欄に置き換える (Unity 互換)
    /// @note 画面隅のポップアップだと視線移動とマウス移動が毎回発生するため、その場で書き換える。
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
            /// @note Enter またはフォーカス喪失で確定する
            const std::string newName = rename.buffer;
            if (!newName.empty() && newName != go.name) {
                /// @note operator (node.rename) 経由にする: 旧経路の ExecuteSceneEditWithUndo は
                /// @note 名前を 1 つ変えるだけでシーン全体を 2 回 TOML シリアライズしていた。
                OpArgs args;
                args.Set("node", go.instanceId);
                args.Set("name", newName);
                InvokeOperator(ctx, "node.rename", args);
            }
            rename.id = scene::EntityID{};
        }
        /// @note リネーム中は子ツリーを描かない (visited へは登録して二重描画を防ぐ)
        MarkDescendantsVisited(go, visited);
        ImGui::PopID();
        return;
    }

    const bool hasChildren    = VisibleChildCount(ctx, go) > 0;
    const bool selected       = ContainsEntity(ctx.selectedEntities, id);
    const bool isRoot         = go.GetParent() == nullptr;
    const bool isActive       = go.activeInHierarchy();
    /// @note 目アイコンとメニューが切り替えるのは自分の値 (activeSelf)。表示もそれに合わせ、親のせいで無効なときだけ薄く描く。
    const bool isActiveSelf   = go.activeSelf();
    const bool isLocked       = ctx.IsLocked(id);
    const bool isEditorHidden  = ctx.editorHiddenGuids.count(go.instanceId) > 0;
    const bool isPrimarySelected = ctx.PrimarySelected() == id;
    /// @note Prefab インスタンスを青色で識別し、通常 GO と視覚的に区別する (Unity 互換の UX)。
    const bool isPrefabInstance = !go.prefabAssetPath.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_FramePadding;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected)     flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(static_cast<int>(id.index));

    /// @note SetParent 後の次フレームで強制 open
    if (pendingExpand && *pendingExpand == id) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        *pendingExpand = scene::EntityID{};
    }
    /// @note 他の面で選ばれた対象までの祖先を開く。開いた状態は StateStorage に残るので、
    /// @note 要求はこのフレームの描画が終わった時点で捨ててよい (畳み直しを妨げない)。
    if (ContainsEntity(reveal.openChain, id))
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    /// @note エディタ専用非表示はシアン（runtime 非アクティブより優先）、非アクティブはグレー、
    /// @note ロック中はオレンジ、プレファブインスタンスは水色
    ui::PushHierarchySelectionColors();
    if (isEditorHidden)         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Info));
    else if (!isActive)         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
    else if (isLocked)          ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
    else if (isPrefabInstance)  ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Accent));
    const bool opened = ImGui::TreeNodeEx(go.name.c_str(), flags);
    if (isEditorHidden || !isActive || isLocked || isPrefabInstance) ImGui::PopStyleColor();
    ui::PopHierarchySelectionColors();

    /// @note ノード本体のアイテム状態は TreeNodeEx 直後に確定させ、以降で使い回す。この後に描く
    /// @note 「隠し生成物」バッジ (TextDisabled) が新しい item として LastItemData を上書きし、
    /// @note 後段の IsItemClicked()/GetItemRectMin() がバッジ側を指してしまうため。
    const ImVec2 nodeMin         = ImGui::GetItemRectMin();
    const ImVec2 nodeMax         = ImGui::GetItemRectMax();
    const bool   nodeHovered     = ImGui::IsItemHovered();
    const bool   nodeClicked     = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool   nodeToggledOpen = ImGui::IsItemToggledOpen();
    /// @note 右クリックメニュー用のホバー。別ノードのメニューを開いたまま行を移れるよう、
    /// @note BeginPopupContextItem() 内部と同じ AllowWhenBlockedByPopup を使う。
    const bool   nodeHoveredForMenu =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    hierarchyNodeHovered = hierarchyNodeHovered || nodeHovered;

    /// @note 祖先を開いた結果この行が現れたので、そこまでスクロールする。
    if (reveal.scrollTo == id) {
        ImGui::SetScrollHereY(0.5f);
        reveal.scrollTo = scene::EntityID{};
    }

    /// @note 隠している生成物の件数バッジ。「Hierarchy に出ていない = 存在しない」ではないことを示す。
    /// @note VFX 再生中は裏で十数個の GameObject が動くが、何も出さないと「生成されていない」と誤診断させる。
    if (const int hiddenCount = HiddenGeneratedCount(ctx, go); hiddenCount > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(+%d)", hiddenCount);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%d generated object(s) hidden\nView > Show Generated Objects to reveal",
                              hiddenCount);
    }

    /// @name 右端 visibility/editor-hide/lock アイコン (DrawList で直接描画)
    {
        const float  h       = nodeMax.y - nodeMin.y;
        const float  btnW    = h + 2.0f;
        /// @note SpanAvailWidth のためnodeMax.x = ウィンドウコンテンツ右端
        const float  rx      = nodeMax.x;

        /// @note アイコン配置: lock | vis | editorHide (右から)
        const ImVec2 ehMin   = { rx - btnW * 3.0f, nodeMin.y };
        const ImVec2 ehMax   = { rx - btnW * 2.0f, nodeMax.y };
        const ImVec2 visMin  = { rx - btnW * 2.0f, nodeMin.y };
        const ImVec2 visMax  = { rx - btnW,         nodeMax.y };
        const ImVec2 lockMin = { rx - btnW,          nodeMin.y };
        const ImVec2 lockMax = { rx,                 nodeMax.y };

        /// @note ヒット判定
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
                    /// @note 解除: 非表示前の activeSelf を復元
                    g->SetActive(it->second);
                    ctx.editorHiddenGuids.erase(it);
                } else {
                    /// @note 非表示: 現在の activeSelf を保存してから SetActive(false)
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

        /// @note エディタ専用非表示アイコン (editor-hide: ホバー時/非表示中のみ表示)
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

        /// @note visibility アイコン
        if (visHov) dl->AddRectFilled(visMin, visMax, IM_COL32(80, 80, 80, 160), 2.0f);
        const char* visChar = isActiveSelf ? "o" : "-";
        ImVec2 vts = ImGui::CalcTextSize(visChar);
        dl->AddText({ visMin.x + (btnW - vts.x) * 0.5f, visMin.y + (h - vts.y) * 0.5f },
                    isActive ? IM_COL32(200, 200, 200, 200) : IM_COL32(100, 100, 100, 200), visChar);
        if (visHov && isActiveSelf && !isActive)
            ImGui::SetTooltip("親の GameObject が無効なため、この GameObject も無効です");

        /// @note lock アイコン (常時描画: ロック中はオレンジ、非ロック+ホバーは薄く)
        if (lockHov || isLocked) {
            if (lockHov) dl->AddRectFilled(lockMin, lockMax, IM_COL32(80, 80, 80, 160), 2.0f);
            ImVec2 lts = ImGui::CalcTextSize("L");
            dl->AddText({ lockMin.x + (btnW - lts.x) * 0.5f, lockMin.y + (h - lts.y) * 0.5f },
                        isLocked ? IM_COL32(255, 175, 50, 240) : IM_COL32(120, 120, 120, 140), "L");
        }

        /// @note prefab インスタンスバッジ (右端 4 番目スロット, 常時表示の水色ダイヤ)。青いテキスト
        /// @note だけだと非アクティブや選択ハイライトと重なった際に判別しづらいため常設マーカーを置き、
        /// @note ホバーで参照パスをツールチップ表示する。
        if (isPrefabInstance) {
            const ImVec2 pfMin = { rx - btnW * 4.0f, nodeMin.y };
            const ImVec2 pfMax = { rx - btnW * 3.0f, nodeMax.y };
            const ImVec2 pfCenter = { (pfMin.x + pfMax.x) * 0.5f, (pfMin.y + pfMax.y) * 0.5f };
            const float  pfRadius = h * 0.22f;
            /// @note 外周を少し濃く、内側を塗って小さなダイヤ(菱形)にする
            dl->AddNgonFilled(pfCenter, pfRadius, IM_COL32(100, 180, 255, 235), 4);
            dl->AddNgon(pfCenter, pfRadius, IM_COL32(40, 90, 150, 235), 4, 1.0f);
            if (ImGui::IsMouseHoveringRect(pfMin, pfMax, false))
                ImGui::SetTooltip("Prefab instance\n%s", go.prefabAssetPath.c_str());
        }

        /// @note ノードのクリック/ダブルクリック判定 (アイコン領域は除外)
        const bool iconAreaClick = ehClick || visClick || lockClick;
        if (nodeClicked && !nodeToggledOpen
            && !isLocked && !iconAreaClick) {
            /// @note ここではまだ選択を変更しない。MouseClicked はドラッグ開始より先に来るため、
            /// @note 先に選択すると「つかんだ瞬間に Inspector が別 GO へ切り替わる」。
            /// @note クリックかドラッグか確定する MouseReleased まで保留する。
            pendingClick = id;
        }
        if (nodeHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
            && !isLocked && !iconAreaClick) {
            /// @note メッシュバウンズ基準で注視点・距離を決める (Unity の Frame Selected 互換)
            math::Vector3 focusCenter = go.transform.position;
            float focusRadius = 0.0f;
            ComputeGameObjectBounds(go, focusCenter, focusRadius);
            ctx.focusTargetPosition    = focusCenter;
            ctx.focusTargetRadius      = focusRadius;
            ctx.requestFocusOnSelected = true;
        }
    }

    if (!isLocked && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        hierarchyDragStarted = true;
        pendingClick = {};
        ImGui::SetDragDropPayload("FBZZ_HIERARCHY_ENTITY", &id, sizeof(id));
        /// @note 掴んだ行が選択に含まれていれば選択全体を運ぶ。payload は掴んだ 1 体のまま。
        dragdrop::SetEntityDrag(id, ctx.selectedEntities);
        const size_t dragCount = dragdrop::DraggedEntities(id).size();
        if (dragCount > 1) ImGui::Text("%s (+%d)", go.name.c_str(), static_cast<int>(dragCount - 1));
        else               ImGui::TextUnformatted(go.name.c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        scene::EntityID draggedId;
        if (ReadEntityPayload(ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), draggedId)) {
            std::vector<scene::EntityID> dragged = DraggedRoots(*ctx.activeScene, draggedId, prevVisible);
            deferred = [&ctx, dragged, draggedId, id, pendingExpand]() {
                auto* target = ctx.activeScene->GetGameObject(id);
                if (!target) return;
                std::vector<scene::EntityID> moved;
                for (scene::EntityID childId : dragged) {
                    auto* child = ctx.activeScene->GetGameObject(childId);
                    /// @note 自分自身と、落とし先の祖先は子にできない
                    if (!child || childId == id || target->IsDescendantOf(*child)) continue;
                    SetParentWithUndo(ctx, childId, id, "Reparent GameObject");
                    moved.push_back(childId);
                }
                if (moved.empty()) return;
                SelectMoved(ctx, std::move(moved), draggedId);
                if (pendingExpand) *pendingExpand = id;
            };
        }
        std::string assetPath;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath))
            QueueAssetDrop(ctx, assetPath, id, go.instanceId, deferred, pendingCreate, pendingExpand);
        ImGui::EndDragDropTarget();
    }

    /// @name 兄弟間並べ替え: ノード下端の細い帯を「この直後に挿入」ドロップ先にする
    /// @note ノード本体へのドロップは「子にする」操作。境界線ドロップが無いと並び順の変更に
    /// @note Order メニュー (ルート限定) を往復する羽目になる。
    if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
        dragging && dragging->IsDataType("FBZZ_HIERARCHY_ENTITY")) {
        /// @note 帯は行の下端に置く。ここで GetItemRect を読み直すと、バッジを持つノードでは
        /// @note バッジの矩形 (行の一部) だけが挿入先になり、行のどこを狙っても入らなくなる。
        /// @note 挿入帯の半分の高さ (px)
        constexpr float kBandHalf = 3.0f;
        const ImRect band({ nodeMin.x, nodeMax.y - kBandHalf },
                          { nodeMax.x, nodeMax.y + kBandHalf });
        const ImGuiID bandId = ImGui::GetID("##reorder_after");
        if (ImGui::BeginDragDropTargetCustom(band, bandId)) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    "FBZZ_HIERARCHY_ENTITY",
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                /// @note ドロップ位置プレビューの挿入ライン
                ImGui::GetWindowDrawList()->AddLine(
                    { nodeMin.x, nodeMax.y }, { nodeMax.x, nodeMax.y },
                    IM_COL32(100, 180, 255, 255), 2.0f);

                scene::EntityID draggedId;
                if (payload->IsDelivery() && ReadEntityPayload(payload, draggedId)) {
                    std::vector<scene::EntityID> dragged =
                        DraggedRoots(*ctx.activeScene, draggedId, prevVisible);
                    deferred = [&ctx, dragged, draggedId, id]() {
                        auto* anchor = ctx.activeScene->GetGameObject(id);
                        if (!anchor) return;
                        scene::GameObject* parent = anchor->GetParent();
                        /// @note 運ぶものを行の並び順のまま anchor の後ろへ 1 つずつ継ぎ足す
                        scene::GameObject* insertAfter = anchor;
                        std::vector<scene::EntityID> moved;
                        for (scene::EntityID movedId : dragged) {
                            auto* item = ctx.activeScene->GetGameObject(movedId);
                            /// @note 自分自身と、自分の子孫の隣には置けない
                            if (!item || movedId == id || anchor->IsDescendantOf(*item)) continue;

                            /// @note anchor と同じ親に揃えてから挿入する
                            if (item->GetParent() != parent) {
                                const bool ok = parent ? item->SetParent(parent) : item->ClearParent();
                                if (!ok) continue;
                            }
                            /// @note 一旦末尾へ送って index 計算を単純化する
                            item->SetSiblingIndex(1 << 30);
                            item->SetSiblingIndex(insertAfter->GetSiblingIndex() + 1);
                            insertAfter = item;
                            moved.push_back(movedId);
                        }
                        if (moved.empty()) return;
                        SelectMoved(ctx, std::move(moved), draggedId);
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    };
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    if (pendingClick == id && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
        && nodeHovered && !hierarchyDragStarted) {
        const bool shiftHeld = ImGui::GetIO().KeyShift;
        const bool ctrlHeld  = ImGui::GetIO().KeyCtrl;
        /// @note ここでの選択は既に見えている行なので、見せに行く要求は出さない (Skip)。
        if (shiftHeld && lastClicked.IsValid()) {
            /// @note Shift+クリック: prevVisible の順番で lastClicked〜id の範囲を選択
            auto it1 = std::find(prevVisible.begin(), prevVisible.end(), lastClicked);
            auto it2 = std::find(prevVisible.begin(), prevVisible.end(), id);
            if (it1 != prevVisible.end() && it2 != prevVisible.end()) {
                std::vector<scene::EntityID> range =
                    ctrlHeld ? ctx.selectedEntities : std::vector<scene::EntityID>{};
                if (it1 > it2) std::swap(it1, it2);
                for (auto it = it1; it <= it2; ++it)
                    if (!ContainsEntity(range, *it)) range.push_back(*it);
                SelectEntities(ctx, std::move(range), SelectionReveal::Skip);
            }
        } else {
            if (ctrlHeld) ToggleSelection(ctx, id, SelectionReveal::Skip);
            else          SelectEntity(ctx, id, SelectionReveal::Skip);
            lastClicked = id;
        }
        pendingClick = {};
    }

    /// @note 引数なしの BeginPopupContextItem() は使わない: あれは LastItemData を対象に取るが、
    /// @note 上の「隠し生成物」バッジ (TextDisabled) は ID を持たない item のため、バッジが出る
    /// @note ノードでは id == 0 になり ImGui が assert する。開く判定も TreeNodeEx 直後のホバーを使う。
    static constexpr const char* kNodeMenuId = "##node_context";
    if (nodeHoveredForMenu && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup(kNodeMenuId);
    if (ImGui::BeginPopup(kNodeMenuId)) {
        /// @note 右クリックした GO が既に複数選択中なら選択を維持する。
        /// @note そうでなければ単一選択に切り替える。
        {
            const bool alreadySelected = std::find(
                ctx.selectedEntities.begin(), ctx.selectedEntities.end(), id)
                != ctx.selectedEntities.end();
            if (!alreadySelected || ctx.selectedEntities.size() == 1)
                SelectEntity(ctx, id, SelectionReveal::Skip);
        }
        const bool multiSelected = ctx.selectedEntities.size() > 1;

        if (ImGui::MenuItem(multiSelected ? "Hide/Show" : (isActiveSelf ? "Hide" : "Show"))) {
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

        /// @note 親は生成コマンドが guid で付ける。生成後の選択に頼ると Button + Label のような複数生成で親を取り違える。
        if (ImGui::BeginMenu("Create Child")) {
            DrawCreateObjectMenuItems(ctx, go.instanceId, pendingCreate);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Create at Root")) {
            DrawCreateObjectMenuItems(ctx, {}, pendingCreate);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        /// @note operator 経由にする: 同じ Copy / Paste はホットキー・コマンドパレット・AI からも
        /// @note 呼ばれる。ヘルパーを直接叩くと実行可否の判定がこのファイルにも書かれ面ごとにずれていく。
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, CanInvokeOperator(ctx, "edit.copy")))
            InvokeOperator(ctx, "edit.copy");
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, CanInvokeOperator(ctx, "edit.paste")))
            deferred = [&ctx]() { InvokeOperator(ctx, "edit.paste"); };
        if (ImGui::MenuItem("Paste As Child", "Ctrl+Shift+V", false,
                            CanInvokeOperator(ctx, "edit.paste_as_child")))
            deferred = [&ctx]() { InvokeOperator(ctx, "edit.paste_as_child"); };
        ImGui::Separator();
        /// @note operator 経由にする: 旧実装はメニューからの複製が Undo に載っていなかった
        /// @note (ExecuteSceneEditWithUndo で包んでいなかったため。Ctrl+D は載る)。
        if (ImGui::MenuItem("Duplicate", nullptr, false,
                            CanInvokeOperator(ctx, "edit.duplicate"))) {
            const std::vector<scene::EntityID> toDup =
                multiSelected ? ctx.selectedEntities : std::vector<scene::EntityID>{ id };
            deferred = [&ctx, toDup]() {
                SelectEntities(ctx, toDup, SelectionReveal::Skip);
                InvokeOperator(ctx, "edit.duplicate");
            };
        }
        if (multiSelected) {
            if (ImGui::MenuItem("Group Selection")) {
                const std::vector<scene::EntityID> toGroup = ctx.selectedEntities;
                deferred = [&ctx, toGroup]() {
                    if (!ctx.activeScene) return;
                    /// @note 共通親 (全員同じ親を持つ場合) を探す
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
                            /// @note redo: 再実行には deferred が必要で複雑なため省略
                            []() {},
                            [scene, groupGuid, markDirty]() {
                                /// @note undo: 子を解除してグループを削除
                                if (auto* g = scene->FindByGuid(groupGuid)) {
                                    while (g->GetChildCount() > 0)
                                        if (auto* c = g->GetChild(0)) c->ClearParent();
                                    scene->DestroyGameObject(g->GetID());
                                }
                                if (markDirty) markDirty();
                            }));
                    }
                    SelectEntity(ctx, groupId, SelectionReveal::Skip);
                };
            }
        }
        if (ImGui::MenuItem("Save As Prefab")) {
            SaveSelectedAsAsset(ctx, go.name, "Prefabs", ".prefab");
        }
        /// @note VFX ルートは .vfx として保存する。中身はプレハブと同じだが、
        /// @note 拡張子で «演出» と分かるようにしておく (VFXRef のドロップ先にもなる)。
        if (go.GetComponent<scene::VFXComponent>() != nullptr) {
            if (ImGui::MenuItem("Save As VFX")) {
                SaveSelectedAsAsset(ctx, go.name, "VFX", ".vfx");
            }
        }
        /// @note プレファブインスタンスには Apply / Revert を提供する (Unity 互換)。Apply は
        /// @note ディスクへの書き戻しのみでシーンは変わらないため Undo なし。Revert はシーンを
        /// @note 変更するため ExecuteSceneEditWithUndo が自動的にスナップショット Undo を生成する。
        if (!go.prefabAssetPath.empty()) {
            ImGui::Separator();
            if (ImGui::MenuItem("Apply to Prefab")) {
                deferred = [&ctx, id]() {
                    if (!ctx.activeScene) return;
                    /// @note Apply と伝播は 1 つの操作として閉じてある。
                    /// @note 各インスタンスの個別調整 (override) は保持される。
                    const int updated = PrefabSerializer::ApplyAndPropagate(
                        *ctx.activeScene, id, ctx.projectRoot);
                    if (updated < 0) return;
                    if (updated > 0) {
                        /// @note 作り直しで EntityID が変わるため選択は捨てる。
                        ClearEntitySelection(ctx);
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
                            if (!newRoots.empty()) SelectEntities(ctx, newRoots, SelectionReveal::Skip);
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
        /// @note operator 経由にする: 旧実装は DestroySelected を直接呼び Undo に載っていなかった。
        /// @note Delete キー経由は DeleteSelectedWithUndo を通るため取り消せる食い違いがあった。
        if (ImGui::MenuItem("Delete", nullptr, false,
                            CanInvokeOperator(ctx, "edit.delete_selected")))
            deferred = [&ctx]() { InvokeOperator(ctx, "edit.delete_selected"); };
        ImGui::EndPopup();
    }

    if (hasChildren) {
        if (opened) {
            for (int i = 0; i < go.GetChildCount(); ++i) {
                if (auto* child = go.GetChild(i))
                    DrawHierarchyNode(ctx, *child, 0, rootCount, visited, deferred, pendingCreate, reveal,
                                      lastClicked, pendingClick, hierarchyDragStarted,
                                      hierarchyNodeHovered, outVisible, prevVisible, rename);
            }
            ImGui::TreePop();
        } else {
            /// @note 閉じていても子孫を visited に入れる。
            /// @note これをしないと第2ループが子を root レベルで誤描画する。
            MarkDescendantsVisited(go, visited);
        }
    }

    ImGui::PopID();
}

/// @brief 行の無い所を右クリックしたときのメニュー。ツリー・検索結果・Map Mode の 3 表示で共有する。
/// @note Select All はロック済みを除く規則を持つので、Ctrl+A と同じ Operator を呼ぶ。
void DrawHierarchyBackgroundMenu(EditorContext& ctx, std::function<void()>& deferred,
                                 PendingObjectCreate& pendingCreate)
{
    if (!ImGui::BeginPopupContextWindow("##scene_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        return;

    if (ImGui::MenuItem("Select All", "Ctrl+A", false, CanInvokeOperator(ctx, "select.all")))
        InvokeOperator(ctx, "select.all");
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, CanInvokeOperator(ctx, "edit.paste")))
        deferred = [&ctx]() { InvokeOperator(ctx, "edit.paste"); };
    ImGui::Separator();
    DrawCreateObjectMenu(ctx, pendingCreate);
    ImGui::EndPopup();
}

} /// @note namespace

void SceneHierarchyPanel::HandlePanelRequests(EditorContext& ctx)
{
    /// @note フォーカスの申告は IPanel::OnRender が GetHotkeyScope() を見て行う。
    /// @note ここに残すのは、パネル内部の状態 (編集バッファ) を要する要求だけ。
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

void SceneHierarchyPanel::HandleKeyboardNavigation(EditorContext& ctx)
{
    if (ctx.activeScene == nullptr || m_visibleOrder.empty()) return;
    /// @note 検索欄やインラインリネームへ打っている間は奪わない。
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (ImGui::GetIO().WantTextInput || m_renamingId.IsValid()) return;

    /// @note IsKeyPressed でなく Shortcut を使う: 矢印キーは ImGui のキーボードナビも使うため、
    /// @note 素のキー読みだと «ナビの枠» と «選択» が別々に動いて 2 つ光る。Shortcut は routing で
    /// @note キーの所有権を取るのでナビ側が同じキーを消費しない。押しっぱなしのリピートも受ける。
    constexpr ImGuiInputFlags kRepeat = ImGuiInputFlags_Repeat;
    int  step   = 0;
    bool extend = false;
    if (ImGui::Shortcut(ImGuiKey_DownArrow, kRepeat)) {
        step = 1;
    } else if (ImGui::Shortcut(ImGuiKey_UpArrow, kRepeat)) {
        step = -1;
    } else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_DownArrow, kRepeat)) {
        step = 1;  extend = true;
    } else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_UpArrow, kRepeat)) {
        step = -1; extend = true;
    }
    const bool toHome = ImGui::Shortcut(ImGuiKey_Home);
    const bool toEnd  = ImGui::Shortcut(ImGuiKey_End);
    if (step == 0 && !toHome && !toEnd) return;

    const int  last    = static_cast<int>(m_visibleOrder.size()) - 1;
    const auto primary = ctx.PrimarySelected();

    int index = -1;
    for (int i = 0; i <= last; ++i)
        if (m_visibleOrder[static_cast<std::size_t>(i)] == primary) { index = i; break; }

    int next = 0;
    if (toHome)          next = 0;
    else if (toEnd)      next = last;
    /// @note 選択が無い / 畳まれて消えた
    else if (index < 0)  next = (step > 0) ? 0 : last;
    else                 next = std::clamp(index + step, 0, last);

    const scene::EntityID target = m_visibleOrder[static_cast<std::size_t>(next)];
    if (target == primary) return;

    /// @note Shift は «アンカーから今の行まで» を選ぶ。アンカーの決め方も範囲の作り方も
    /// @note Shift+クリック (DrawHierarchyNode) と同じにして、経路で結果が変わらないようにする。
    if (extend && m_lastClickedEntity.IsValid()) {
        auto from = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), m_lastClickedEntity);
        auto to   = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), target);
        if (from != m_visibleOrder.end() && to != m_visibleOrder.end()) {
            if (from > to) std::swap(from, to);
            std::vector<scene::EntityID> range;
            for (auto it = from; it <= to; ++it) range.push_back(*it);
            SelectEntities(ctx, std::move(range), SelectionReveal::Skip);
        }
    } else {
        SelectEntity(ctx, target, SelectionReveal::Skip);
        m_lastClickedEntity = target;
    }

    /// @note 行まで運ぶ。ここは描画が終わったあとなので、次フレームの描画が拾う。
    m_revealScrollTo = target;
}

void SceneHierarchyPanel::ConsumeRevealRequest(EditorContext& ctx)
{
    if (!ctx.hierarchyRevealTarget.IsValid() || !ctx.activeScene) return;

    const scene::EntityID target = ctx.hierarchyRevealTarget;
    ctx.hierarchyRevealTarget = scene::EntityID{};

    auto* go = ctx.activeScene->GetGameObject(target);
    if (!go) return;

    /// @note 検索フィルタ中はツリーそのものが出ていない。対象がヒットしないフィルタなら、
    /// @note 開いても見えないままなので外す。
    if (m_searchFilter[0] != '\0' && !util::StringUtils::ContainsCI(go->name, m_searchFilter))
        m_searchFilter[0] = '\0';

    /// @note ランタイム生成物は既定で行が出ない。それを選んだのなら表示を開ける。
    if (go->runtimeGenerated) ctx.showGeneratedObjects = true;

    m_revealOpenChain.clear();
    for (auto* parent = go->GetParent(); parent; parent = parent->GetParent())
        m_revealOpenChain.push_back(parent->GetID());
    m_revealScrollTo = target;
}

void SceneHierarchyPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        return;
    }

    ConsumeRevealRequest(ctx);

    /// @note ドラッグ開始後の選択保留はマウス操作が終わったフレームで解放する。
    /// @note MouseReleased のフレームにはまだドロップ判定が残っているので消さない。
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)
        && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_pendingClickEntity = {};
        m_hierarchyDragStarted = false;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_hierarchyDragStarted = false;
    bool hierarchyNodeHovered = false;

    widgets::UpdateDragAutoScroll();

    if (ctx.mapEditingMode) {
        ImGui::TextColored({ 0.35f, 0.88f, 0.48f, 1.0f }, "MAP MODE");
        ImGui::SameLine();
        ImGui::Checkbox("Map Objects Only", &ctx.mapHierarchyFilter);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Show only Terrain Grid, Terrain, Water and their children\nUncheck to browse all objects in Map Mode");
    }

    /// @note 生成物トグルは «見えているのが全部か» の前提なので、メニューの奥でなく検索欄の右に常に出す。
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
                              "(VFX Graph nodes, water splash).\n"
                              "These are never saved into the scene.");
    }

    if (ctx.activeScene->GameObjectCount() == 0 && m_searchFilter[0] == '\0') {
        ImGui::Spacing();
        ImGui::TextDisabled("Scene is empty");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Right-click here to create objects, or drag a model / prefab from the Asset Browser into the Scene.");
        ImGui::PopTextWrapPos();
    }

    /// @note 生成は Operator が Undo を積むので、スナップショット Undo で包む deferred とは分けて持つ。
    PendingObjectCreate pendingCreate;

    /// @note 検索中はフラットリスト。キーボード移動の並びもこのリストが正本になる。
    if (m_searchFilter[0] != '\0') {
        std::function<void()> deferred;
        std::vector<scene::EntityID> visibleRows;

        for (auto& go : ctx.activeScene->GameObjects()) {
            if (!util::StringUtils::ContainsCI(go.name, m_searchFilter)) continue;
            /// @note ツリーと同じ基準で隠す。検索したときだけ生成物が出ると同名の GO が 2 つあるように見える。
            if (IsHiddenGenerated(ctx, go)) continue;
            if (ctx.mapEditingMode && ctx.mapHierarchyFilter) {
                const bool isMapObject =
                    go.GetComponent<scene::TerrainGridComponent>()
                    || go.GetComponent<scene::TerrainComponent>()
                    || go.GetComponent<scene::WaterComponent>();
                auto* parentGO = go.GetParent();
                const bool isMapChild = parentGO
                    && parentGO->GetComponent<scene::TerrainComponent>();
                if (!isMapObject && !isMapChild)
                    continue;
            }
            const scene::EntityID id = go.GetID();
            const bool selected = ContainsEntity(ctx.selectedEntities, id);
            visibleRows.push_back(id);
            ImGui::PushID(static_cast<int>(id.index));

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
                        OpArgs args;
                        args.Set("node", go.instanceId);
                        args.Set("name", newName);
                        InvokeOperator(ctx, "node.rename", args);
                    }
                    m_renamingId = {};
                }
                ImGui::PopID();
                continue;
            }

            ui::PushHierarchySelectionColors();
            if (ImGui::Selectable(go.name.c_str(), selected)) {
                if (ImGui::GetIO().KeyCtrl) ToggleSelection(ctx, id, SelectionReveal::Skip);
                else                        SelectEntity(ctx, id, SelectionReveal::Skip);
            }
            ui::PopHierarchySelectionColors();
            if (m_revealScrollTo == id) {
                ImGui::SetScrollHereY(0.5f);
                m_revealScrollTo = {};
            }
            ui::DrawSelectionAccent(ImGui::GetWindowDrawList(),
                                    ImGui::GetItemRectMin(),
                                    ImGui::GetItemRectMax(),
                                    selected,
                                    ctx.PrimarySelected() == id);
            if (ImGui::BeginPopupContextItem()) {
                /// @note 選択を対象にする操作 (Copy / Delete) が右クリックした行を指すよう、先に単一選択にする。
                SelectEntity(ctx, id, SelectionReveal::Skip);
                if (ImGui::MenuItem("Copy", "Ctrl+C", false, CanInvokeOperator(ctx, "edit.copy")))
                    InvokeOperator(ctx, "edit.copy");
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, CanInvokeOperator(ctx, "edit.paste")))
                    deferred = [&ctx]() { InvokeOperator(ctx, "edit.paste"); };
                if (ImGui::MenuItem("Paste As Child", "Ctrl+Shift+V", false,
                                    CanInvokeOperator(ctx, "edit.paste_as_child")))
                    deferred = [&ctx]() { InvokeOperator(ctx, "edit.paste_as_child"); };
                ImGui::Separator();
                if (ImGui::BeginMenu("Create Child")) {
                    DrawCreateObjectMenuItems(ctx, go.instanceId, pendingCreate);
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save As Prefab"))
                    SaveSelectedAsAsset(ctx, go.name, "Prefabs", ".prefab");
                if (go.GetComponent<scene::VFXComponent>() != nullptr
                    && ImGui::MenuItem("Save As VFX"))
                    SaveSelectedAsAsset(ctx, go.name, "VFX", ".vfx");
                ImGui::Separator();
                if (ImGui::MenuItem("Delete", nullptr, false,
                                    CanInvokeOperator(ctx, "edit.delete_selected")))
                    deferred = [&ctx]() { InvokeOperator(ctx, "edit.delete_selected"); };
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        m_revealOpenChain.clear();
        m_revealScrollTo = {};
        m_visibleOrder   = std::move(visibleRows);

        HandleKeyboardNavigation(ctx);
        HandlePanelRequests(ctx);
        DrawHierarchyBackgroundMenu(ctx, deferred, pendingCreate);
        if (deferred)
            ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", deferred);
        InvokePendingObjectCreate(ctx, pendingCreate);
        return;
    }

    /// @note Map Mode は Terrain Grid / Terrain / Water のルートだけをツリーで出す (子を親から切り離して見せない)。
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
                && !go->GetComponent<scene::WaterComponent>())
                continue;
            DrawHierarchyNode(ctx, *go, 0, rootCount, visited, deferred, pendingCreate,
                HierarchyRevealState{ m_pendingExpand, m_revealOpenChain, m_revealScrollTo },
                m_lastClickedEntity, m_pendingClickEntity, m_hierarchyDragStarted,
                hierarchyNodeHovered, mapVisible, m_visibleOrder,
                InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });
        }
        m_revealOpenChain.clear();
        m_revealScrollTo = {};
        m_visibleOrder   = std::move(mapVisible);

        HandleKeyboardNavigation(ctx);
        HandlePanelRequests(ctx);
        DrawHierarchyBackgroundMenu(ctx, deferred, pendingCreate);
        if (deferred)
            ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", deferred);
        InvokePendingObjectCreate(ctx, pendingCreate);
        return;
    }

    /// @note 描画ループ中にシーンを変えると後続の反復でポインタが無効になるので、変更はループ後にまとめて実行する。
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

    /// @note ルートから描く。畳まれていても DrawHierarchyNode が子孫を visited へ入れる。
    const auto   roots     = ctx.activeScene->GetRootGameObjects();
    const size_t rootCount = roots.size();
    for (size_t i = 0; i < roots.size(); ++i)
        if (roots[i]) DrawHierarchyNode(ctx, *roots[i], i, rootCount, visited, deferred, pendingCreate,
            HierarchyRevealState{ m_pendingExpand, m_revealOpenChain, m_revealScrollTo },
            m_lastClickedEntity, m_pendingClickEntity,
            m_hierarchyDragStarted, hierarchyNodeHovered, outVisible, m_visibleOrder,
            InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });

    /// @note 親が無いのに GetRootGameObjects に入らなかった孤立オブジェクトの救済。正常なシーンでは実行されない。
    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!ContainsEntity(visited, go.GetID()) && go.GetParent() == nullptr)
            DrawHierarchyNode(ctx, go, 0, 0, visited, deferred, pendingCreate,
                HierarchyRevealState{ m_pendingExpand, m_revealOpenChain, m_revealScrollTo },
                m_lastClickedEntity, m_pendingClickEntity,
                m_hierarchyDragStarted, hierarchyNodeHovered, outVisible, m_visibleOrder,
                InlineRenameState{ m_renamingId, m_renameBuffer, sizeof(m_renameBuffer), m_renameFocusPending });
    }

    /// @note 開いた状態は ImGui 側に残るので、見せに行く要求を持ち続けると畳み直せなくなる。
    m_revealOpenChain.clear();
    m_revealScrollTo = {};

    m_visibleOrder = std::move(outVisible);

    /// @note 行の上では各ノードのドロップ先に任せる。背景まで受け取ると «自分の行で離した» をルート化と誤認する。
    if (!hierarchyNodeHovered
        && ImGui::BeginDragDropTargetCustom(ImRect(hierarchyMin, hierarchyMax), hierarchyDropId)) {
        scene::EntityID draggedId;
        if (ReadEntityPayload(ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), draggedId)) {
            std::vector<scene::EntityID> dragged = DraggedRoots(*ctx.activeScene, draggedId, m_visibleOrder);
            deferred = [&ctx, dragged, draggedId]() {
                std::vector<scene::EntityID> moved;
                for (scene::EntityID movedId : dragged) {
                    if (auto* item = ctx.activeScene->GetGameObject(movedId)) {
                        item->ClearParent();
                        moved.push_back(movedId);
                    }
                }
                SelectMoved(ctx, std::move(moved), draggedId);
            };
        }
        std::string assetPath;
        /// @note 背景へのドロップは選択に関係なくルート (エンティティを背景へ落とすとルート化するのと同じ)。
        /// @note 選択の子に置きたいときはノードへ落とすか、メニューの Prefab を使う。
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), assetPath))
            QueueAssetDrop(ctx, assetPath, scene::EntityID{}, std::string{}, deferred, pendingCreate, nullptr);
        ImGui::EndDragDropTarget();
    }

    HandleKeyboardNavigation(ctx);
    HandlePanelRequests(ctx);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered()) {
        ClearEntitySelection(ctx);
        m_lastClickedEntity = {};
    }

    DrawHierarchyBackgroundMenu(ctx, deferred, pendingCreate);

    if (deferred) {
        ExecuteSceneEditWithUndo(ctx, "Edit Scene Hierarchy", deferred);
    }
    InvokePendingObjectCreate(ctx, pendingCreate);
}

} /// @note namespace fbzz::editor
