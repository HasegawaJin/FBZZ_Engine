/// @file    ObjectCreation.cpp
/// @brief   GameObject 生成コマンドの実装。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Editor/Util/ObjectCreation.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/ScriptObjectFactory.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Environment/SceneEnvironment.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace fbzz::editor {

namespace {

/// @brief 1 回の生成の結果。Redo で同じ instanceId を名乗り直すために持ち越す。
struct CreationRecord {
    std::vector<std::string> destroyGuids; ///< Undo で消す最上位 (自動で足した Canvas を含む)
    std::vector<std::string> primaryGuids; ///< 要求どおりに作ったルート
    std::vector<std::string> allGuids;     ///< destroyGuids 以下の全 GameObject (深さ優先)
    /// @brief GameObject を作らずシーン設定だけを変えるプリセット (env.ambientWind) だったか。
    bool                     settingsOnly = false;
    /// @brief settingsOnly のとき Undo で戻す値。
    scene::SceneEnvironment  environmentBefore{};
};

void CollectCreationSubtree(scene::GameObject& go, std::vector<scene::GameObject*>& out)
{
    out.push_back(&go);
    for (int i = 0; i < go.GetChildCount(); ++i)
        if (auto* child = go.GetChild(i)) CollectCreationSubtree(*child, out);
}

/// @brief " (N)" 接尾辞を外した名前。
std::string StripCreationIndexSuffix(const std::string& name)
{
    if (name.size() < 4 || name.back() != ')') return name;
    const std::size_t open = name.rfind(" (");
    if (open == std::string::npos || open + 2 >= name.size() - 1) return name;
    for (std::size_t i = open + 2; i + 1 < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return name;
    return name.substr(0, open);
}

/// @return Prefab の実ファイルのパス。見つからなければ空。
std::string ResolveCreationPrefabPath(const EditorContext& ctx, const std::string& key)
{
    if (key.empty()) return {};
    if (util::FileSystem::Exists(key)) return key;
    if (!ctx.projectRoot.empty()) {
        const std::string candidate = ctx.projectRoot + "/" + key;
        if (util::FileSystem::Exists(candidate)) return candidate;
    }
    return {};
}

PresetPlacement PlacementOf(const CreateObjectRequest& request)
{
    if (request.source != CreateObjectSource::Preset) return PresetPlacement::World;
    const ObjectPreset* preset = FindObjectPreset(request.key);
    return preset != nullptr ? preset->placement : PresetPlacement::World;
}

scene::GameObject* FindCanvasInAncestors(scene::GameObject* go)
{
    for (; go != nullptr; go = go->GetParent())
        if (go->GetComponent<scene::UICanvas>() != nullptr) return go;
    return nullptr;
}

/// @brief UI 要素の入れ先を決める。
/// @param outCreatedCanvas 新しく Canvas を作ったらその ID。
/// @note 祖先に Canvas があればそのまま。無ければ activeUICanvas、それも無ければルートに Canvas を作る。
///       Canvas を持たない 3D の親の下へ UI を入れても描かれないので、指定された親より Canvas を優先する。
scene::GameObject* ResolveUIParent(EditorContext& ctx, scene::GameObject* requestedParent,
                                   scene::EntityID& outCreatedCanvas)
{
    if (FindCanvasInAncestors(requestedParent) != nullptr) return requestedParent;

    scene::Scene& targetScene = *ctx.activeScene;
    if (ctx.activeUICanvas.IsValid()) {
        scene::GameObject* active = targetScene.GetGameObject(ctx.activeUICanvas);
        if (active != nullptr && active->GetComponent<scene::UICanvas>() != nullptr) return active;
    }

    const ObjectPreset* canvasPreset = FindObjectPreset("ui.canvas");
    scene::GameObject* canvas = canvasPreset != nullptr ? canvasPreset->create(ctx) : nullptr;
    if (canvas == nullptr) return requestedParent;
    canvas->name = MakeUniqueSiblingName(targetScene, nullptr, canvas->name, canvas);
    outCreatedCanvas = canvas->GetID();
    return canvas;
}

/// @return 作ったルートの ID。失敗なら空。
std::vector<scene::EntityID> SpawnCreationRoots(EditorContext& ctx, const CreateObjectRequest& request)
{
    std::vector<scene::EntityID> roots;
    switch (request.source) {
    case CreateObjectSource::Preset: {
        const ObjectPreset* preset = FindObjectPreset(request.key);
        if (preset == nullptr || preset->create == nullptr) break;
        if (scene::GameObject* go = preset->create(ctx)) roots.push_back(go->GetID());
        break;
    }
    case CreateObjectSource::Prefab: {
        const std::string path = ResolveCreationPrefabPath(ctx, request.key);
        if (path.empty()) break;
        PrefabSerializer::Instantiate(*ctx.activeScene, path, roots);
        break;
    }
    case CreateObjectSource::Script:
        if (scene::GameObject* go = CreateScriptObject(ctx, request.key)) roots.push_back(go->GetID());
        break;
    }
    return roots;
}

/// @brief 生成を 1 回実行する。
/// @param record 前回の結果があれば同じ instanceId を名乗り直し、今回の結果で上書きする。
/// @return 何も作れなければ false (シーンは変えない)。
bool RunCreation(EditorContext& ctx, const CreateObjectRequest& request, CreationRecord& record)
{
    scene::Scene* activeScene = ctx.activeScene;
    if (activeScene == nullptr) return false;

    scene::GameObject* parent = nullptr;
    if (!request.parentGuid.empty()) {
        parent = activeScene->FindByGuid(request.parentGuid);
        if (parent == nullptr) return false;
    }

    const PresetPlacement placement = PlacementOf(request);
    scene::EntityID createdCanvas{};
    if (placement == PresetPlacement::UIElement) parent = ResolveUIParent(ctx, parent, createdCanvas);
    const scene::EntityID parentId = parent != nullptr ? parent->GetID() : scene::EntityID{};

    const scene::SceneEnvironment environmentBefore = activeScene->Environment();
    const std::vector<scene::EntityID> roots = SpawnCreationRoots(ctx, request);
    if (roots.empty()) {
        if (createdCanvas.IsValid()) {
            activeScene->DestroyGameObject(createdCanvas);
            if (ctx.activeUICanvas == createdCanvas) ctx.activeUICanvas = scene::EntityID::INVALID;
        }
        /// @note プリセットが nullptr を返すのは «GameObject を作らずシーン設定を変える» 契約 (ObjectPresets.hpp)。
        if (request.source != CreateObjectSource::Preset || placement != PresetPlacement::World) return false;
        record = {};
        record.settingsOnly      = true;
        record.environmentBefore = environmentBefore;
        return true;
    }

    const bool inView = request.placeInView && ctx.editorCamera != nullptr;
    for (scene::EntityID rootId : roots) {
        scene::GameObject* root = activeScene->GetGameObject(rootId);
        if (root == nullptr) continue;
        scene::GameObject* rootParent = parentId.IsValid() ? activeScene->GetGameObject(parentId) : nullptr;
        if (rootParent != nullptr) root->SetParent(rootParent);

        if (request.hasPosition) {
            root->transform.position = request.position;
        } else if (placement == PresetPlacement::World) {
            if (rootParent != nullptr) root->transform.position = math::Vector3{};
            else if (inView)           root->transform.position = ctx.editorCameraPivot + root->transform.position;
        }

        root->name = request.name.empty()
            ? MakeUniqueSiblingName(*activeScene, rootParent, root->name, root)
            : request.name;
    }

    std::vector<scene::EntityID> destroyIds;
    if (createdCanvas.IsValid()) destroyIds.push_back(createdCanvas);
    else                         destroyIds = roots;

    std::vector<scene::GameObject*> created;
    for (scene::EntityID id : destroyIds)
        if (auto* go = activeScene->GetGameObject(id)) CollectCreationSubtree(*go, created);

    /// @note 生成は決定的なので、個数が一致すれば同じ順に同じ GameObject が並ぶ。
    if (!record.allGuids.empty() && record.allGuids.size() == created.size()) {
        for (std::size_t i = 0; i < created.size(); ++i) created[i]->instanceId = record.allGuids[i];
    }

    record = {};
    for (scene::GameObject* go : created) record.allGuids.push_back(go->instanceId);
    for (scene::EntityID id : destroyIds)
        if (auto* go = activeScene->GetGameObject(id)) record.destroyGuids.push_back(go->instanceId);
    for (scene::EntityID id : roots)
        if (auto* go = activeScene->GetGameObject(id)) record.primaryGuids.push_back(go->instanceId);

    SelectEntities(ctx, roots);
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    return true;
}

} // namespace

bool ValidateCreateObjectRequest(const EditorContext& ctx, const CreateObjectRequest& request,
                                 std::string& outCode, std::string& outMessage)
{
    if (ctx.activeScene == nullptr) {
        outCode = "NO_SCENE";
        outMessage = "アクティブシーンがありません";
        return false;
    }
    if (!request.parentGuid.empty() && ctx.activeScene->FindByGuid(request.parentGuid) == nullptr) {
        outCode = "NODE_NOT_FOUND";
        outMessage = "parent が見つかりません: " + request.parentGuid;
        return false;
    }
    switch (request.source) {
    case CreateObjectSource::Preset:
        if (FindObjectPreset(request.key) == nullptr) {
            outCode = "UNKNOWN_PRESET";
            outMessage = "未知のプリセットです: " + request.key + " (preset_catalog で一覧を確認してください)";
            return false;
        }
        break;
    case CreateObjectSource::Prefab: {
        const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(request.key));
        if (!IsInstantiableAssetExtension(ext)) {
            outCode = "BAD_ARG";
            outMessage = "Prefab は .prefab / .vfx を指定してください: " + request.key;
            return false;
        }
        if (ResolveCreationPrefabPath(ctx, request.key).empty()) {
            outCode = "PREFAB_NOT_FOUND";
            outMessage = "Prefab が見つかりません: " + request.key;
            return false;
        }
        break;
    }
    case CreateObjectSource::Script: {
        const std::vector<std::string> types = scene::ScriptFactory::RegisteredTypeNames();
        if (std::find(types.begin(), types.end(), request.key) == types.end()) {
            outCode = "UNKNOWN_SCRIPT";
            outMessage = "登録されていないスクリプト型です: " + request.key + " (スクリプト DLL はビルド済みですか)";
            return false;
        }
        break;
    }
    }
    return true;
}

std::string CreateObjectLabel(const CreateObjectRequest& request)
{
    switch (request.source) {
    case CreateObjectSource::Preset:
        if (const ObjectPreset* preset = FindObjectPreset(request.key))
            return "Create " + std::string(preset->label);
        return "Create GameObject";
    case CreateObjectSource::Prefab: {
        std::string file = util::FileSystem::GetFilename(request.key);
        const std::size_t dot = file.rfind('.');
        if (dot != std::string::npos) file.resize(dot);
        return "Instantiate " + file;
    }
    case CreateObjectSource::Script:
        return "Create " + ScriptObjectName(request.key);
    }
    return "Create GameObject";
}

std::unique_ptr<ICommand> MakeCreateObjectCommand(
    EditorContext& ctx, const CreateObjectRequest& request, std::string label, bool applyNow,
    std::function<void(const std::vector<std::string>&)> onCreated)
{
    std::string code;
    std::string message;
    if (!ValidateCreateObjectRequest(ctx, request, code, message)) return nullptr;

    auto record = std::make_shared<CreationRecord>();
    EditorContext* context = &ctx;
    const std::vector<scene::EntityID> previousSelection = ctx.selectedEntities;
    const scene::EntityID previousCanvasId = ctx.activeUICanvas;

    auto execute = [context, request, record, onCreated]() -> bool {
        if (!RunCreation(*context, request, *record)) return false;
        if (onCreated) onCreated(record->primaryGuids);
        return true;
    };

    if (applyNow && !execute()) return nullptr;

    auto undo = [context, record, previousSelection, previousCanvasId]() {
        scene::Scene* activeScene = context->activeScene;
        if (activeScene == nullptr) return;
        if (record->settingsOnly) {
            activeScene->Environment() = record->environmentBefore;
            if (context->markSceneDirty) context->markSceneDirty();
            return;
        }
        for (const std::string& guid : record->destroyGuids)
            if (scene::GameObject* go = activeScene->FindByGuid(guid)) activeScene->DestroyGameObject(go->GetID());

        if (context->activeUICanvas.IsValid() && activeScene->GetGameObject(context->activeUICanvas) == nullptr)
            context->activeUICanvas = activeScene->GetGameObject(previousCanvasId) != nullptr
                ? previousCanvasId : scene::EntityID::INVALID;

        SelectEntities(*context, previousSelection, SelectionReveal::Skip);
        PruneSelection(*context);
        if (context->markSceneDirty) context->markSceneDirty();
    };

    return std::make_unique<LambdaCommand>(
        std::move(label),
        [execute]() { execute(); },
        std::move(undo));
}

std::string MakeUniqueSiblingName(const scene::Scene& targetScene, const scene::GameObject* parent,
                                  const std::string& base, const scene::GameObject* self)
{
    const std::string stem = StripCreationIndexSuffix(base.empty() ? std::string("GameObject") : base);

    std::vector<std::string> taken;
    if (parent != nullptr) {
        for (int i = 0; i < parent->GetChildCount(); ++i)
            if (const auto* child = parent->GetChild(i); child != nullptr && child != self)
                taken.push_back(child->name);
    } else {
        for (const auto* root : targetScene.GetRootGameObjects())
            if (root != nullptr && root != self) taken.push_back(root->name);
    }

    const auto isTaken = [&taken](const std::string& candidate) {
        return std::find(taken.begin(), taken.end(), candidate) != taken.end();
    };
    if (!isTaken(stem)) return stem;
    for (int index = 1; index < 10000; ++index) {
        std::string candidate = stem + " (" + std::to_string(index) + ")";
        if (!isTaken(candidate)) return candidate;
    }
    return stem;
}

bool IsInsidePrefabInstance(const scene::GameObject* go)
{
    for (; go != nullptr; go = go->GetParent())
        if (!go->prefabAssetPath.empty() || !go->prefabSourceId.empty()) return true;
    return false;
}

const char* PrefabInstanceChildWarning()
{
    return "The parent is part of a prefab instance. Added objects are not tracked as overrides:\n"
           "Revert from Prefab and prefab updates remove them; Apply to Prefab writes them into the asset.";
}

} // namespace fbzz::editor
