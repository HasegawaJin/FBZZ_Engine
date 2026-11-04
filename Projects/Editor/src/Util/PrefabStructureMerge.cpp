/// @file    PrefabStructureMerge.cpp
/// @brief   Three-way Prefab topology merge, conflict review and recoverable editor command.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Editor/Util/PrefabStructureMerge.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace fbzz::editor {
namespace {
using Objects = std::map<std::string, toml::table>;

std::string Text(const toml::node& node)
{
    std::ostringstream stream;
    stream << toml::toml_formatter{node};
    return stream.str();
}

void Assign(toml::table& table, std::string_view key, const toml::node* value)
{
    table.erase(key);
    if (value) value->visit([&](const auto& item) { table.insert(key, item); });
}

void RemoveReferenceHints(toml::node& node)
{
    if (auto* text = node.as_string()) {
        auto& value = text->get();
        if (asset::AssetDatabase::IsGuidRef(value)) {
            const auto separator = value.find(asset::AssetDatabase::kRefHintSeparator);
            if (separator != std::string::npos) value.resize(separator);
        }
    } else if (auto* table = node.as_table()) {
        for (auto& [key, value] : *table) { (void)key; RemoveReferenceHints(value); }
    } else if (auto* array = node.as_array()) {
        for (auto& value : *array) RemoveReferenceHints(value);
    }
}

bool Equal(const toml::node* left, const toml::node* right)
{
    if (!left || !right) return left == right;
    if (left->type() != right->type()) return false;
    toml::table lhs, rhs;
    Assign(lhs, "value", left);
    Assign(rhs, "value", right);
    asset::EncodeGuidRefs(lhs);
    asset::EncodeGuidRefs(rhs);
    RemoveReferenceHints(lhs);
    RemoveReferenceHints(rhs);
    return Text(lhs) == Text(rhs);
}

void Conflict(PrefabStructureMergeResult& result, const std::string& id,
              const std::string& path, const std::string& reason)
{
    result.conflicts.push_back({id, path, reason});
}

std::string Parent(const toml::table& object)
{
    return object["parentInstanceId"].value_or(std::string{});
}

bool Identity(std::string_view key)
{
    return key == "instanceId" || key == "parentInstanceId" || key == "parent"
        || key == "prefabSourceId" || key == "prefabAssetPath" || key == "prefabSourceSnapshot"
        || key == "name";
}

bool AutomaticName(const std::string& name, const std::string& sourceName)
{
    if (name == sourceName) return true;
    const std::string prefix = sourceName + " (";
    if (!name.starts_with(prefix) || name.size() <= prefix.size() + 1 || name.back() != ')') return false;
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end() - 1,
        [](unsigned char value) { return std::isdigit(value) != 0; });
}

bool ReadObjects(const toml::table& document, Objects& out, std::string& root,
                 PrefabStructureMergeResult& result, const char* side)
{
    const auto* objects = document["gameobjects"].as_array();
    if (!objects || objects->empty()) {
        Conflict(result, {}, side, "A nonempty gameobjects array is required");
        return false;
    }
    for (const auto& item : *objects) {
        const auto* object = item.as_table();
        if (!object) { Conflict(result, {}, side, "Every gameobject must be a table"); continue; }
        const std::string id = (*object)["instanceId"].value_or(std::string{});
        if (id.empty() || !out.emplace(id, *object).second) {
            Conflict(result, id, side, "Missing or duplicate stable source ID");
            continue;
        }
        if (!(*object)["prefabAssetPath"].value_or(std::string{}).empty()
            || !(*object)["prefabSourceSnapshot"].value_or(std::string{}).empty())
            Conflict(result, id, side, "Nested Prefab boundary requires a separate review");
    }
    if (!result.conflicts.empty()) return false;
    std::map<std::string, std::string> names;
    std::set<std::string> ambiguous;
    for (const auto& [id, object] : out) {
        const auto name = object["name"].value_or(std::string{});
        if (!names.emplace(name, id).second) ambiguous.insert(name);
    }
    for (auto& [id, object] : out) {
        std::string parent = Parent(object);
        const auto parentName = object["parent"].value_or(std::string{});
        if (parent.empty() && !parentName.empty()) {
            if (!names.contains(parentName) || ambiguous.contains(parentName)) {
                Conflict(result, id, side, "Legacy parent name is missing or ambiguous");
                continue;
            }
            parent = names.at(parentName);
            object.insert_or_assign("parentInstanceId", parent);
        }
        object.insert_or_assign("parent", std::string{});
        if (parent.empty()) {
            if (!root.empty()) Conflict(result, id, side, "Structural merge requires exactly one root");
            root = id;
        }
        std::set<std::string> visited;
        std::string cursor = id;
        while (!cursor.empty()) {
            if (!visited.insert(cursor).second) { Conflict(result, id, side, "Parent cycle"); break; }
            const auto found = out.find(cursor);
            if (found == out.end()) { Conflict(result, id, side, "Missing parent ID"); break; }
            cursor = Parent(found->second);
            if (cursor.empty()) {
                const auto legacy = found->second["parent"].value_or(std::string{});
                if (!legacy.empty() && names.contains(legacy) && !ambiguous.contains(legacy)) cursor = names.at(legacy);
            }
        }
    }
    if (root.empty()) Conflict(result, {}, side, "Missing root");
    return result.conflicts.empty();
}

bool LocalEdits(const toml::table& baseline, const toml::table& instance, bool root)
{
    if (Parent(baseline) != Parent(instance)) return true;
    if (!AutomaticName(instance["name"].value_or(std::string{}), baseline["name"].value_or(std::string{}))) return true;
    toml::table before = baseline, after = instance;
    for (auto it = before.begin(); it != before.end();) {
        const auto key = it->first.str();
        if (Identity(key) || (root && key == "transform")) it = before.erase(it);
        else ++it;
    }
    for (auto it = after.begin(); it != after.end();) {
        const auto key = it->first.str();
        if (Identity(key) || (root && key == "transform")) it = after.erase(it);
        else ++it;
    }
    return !Equal(&before, &after);
}

void MergeProperties(const toml::table& baseline, const toml::table& instance,
                     toml::table& incoming, const std::string& id, const std::string& prefix,
                     bool root, PrefabStructureMergeResult& result)
{
    std::set<std::string> keys;
    for (const auto& [key, value] : baseline) { (void)value; keys.emplace(key.str()); }
    for (const auto& [key, value] : instance) { (void)value; keys.emplace(key.str()); }
    for (const auto& key : keys) {
        if (prefix.empty() && (Identity(key) || (root && key == "transform"))) continue;
        const auto* before = baseline.get(key);
        const auto* local = instance.get(key);
        auto* next = incoming.get(key);
        if (Equal(before, local)) continue;
        const std::string path = prefix.empty() ? key : prefix + "." + key;
        if (before && local && next && before->is_table() && local->is_table() && next->is_table()) {
            MergeProperties(*before->as_table(), *local->as_table(), *next->as_table(), id, path, false, result);
        } else if (before && !local && next && !Equal(before, next)) {
            Conflict(result, id, path, "Local removal conflicts with a changed template value");
        } else if (before && local && !next) {
            Conflict(result, id, path, "Template removal conflicts with a local property override");
        } else {
            Assign(incoming, key, local);
        }
    }
}

void NormalizeReferenceNames(toml::table& object, const Objects& sources)
{
    const auto normalize = [&](toml::table& table, const char* guidKey, const char* nameKey) {
        const auto found = sources.find(table[guidKey].value_or(std::string{}));
        if (found != sources.end() && table.contains(nameKey))
            table.insert_or_assign(nameKey, found->second["name"].value_or(std::string{}));
    };
    if (auto* ik = object["IKSolverComponent"].as_table())
        if (auto* chains = (*ik)["chains"].as_array())
            for (auto& item : *chains)
                if (auto* chain = item.as_table()) {
                    normalize(*chain, "targetGuid", "targetName");
                    normalize(*chain, "poleGuid", "poleName");
                }
    if (auto* bone = object["BoneComponent"].as_table()) normalize(*bone, "skinnedMeshOwnerGuid", "skinnedMeshOwner");
    if (auto* skin = object["SkinnedMeshRenderer"].as_table()) normalize(*skin, "skeletonRootGuid", "skeletonRootName");
}

void ValidateRemovedReferences(const toml::node& node, const std::set<std::string>& known,
                               const Objects& merged, const std::string& owner, const std::string& path,
                               PrefabStructureMergeResult& result)
{
    if (const auto* value = node.as_string()) {
        const auto& id = value->get();
        if (known.contains(id) && !merged.contains(id))
            Conflict(result, owner, path, "A retained property references an object removed by the merge");
    } else if (const auto* table = node.as_table()) {
        for (const auto& [key, value] : *table)
            ValidateRemovedReferences(value, known, merged, owner, path + "." + std::string(key.str()), result);
    } else if (const auto* array = node.as_array()) {
        for (std::size_t index = 0; index < array->size(); ++index)
            ValidateRemovedReferences((*array)[index], known, merged, owner, path + "[" + std::to_string(index) + "]", result);
    }
}

bool Gather(scene::GameObject& object, std::set<std::string>& guids)
{
    if (object.runtimeGenerated || object.instanceId.empty() || !guids.insert(object.instanceId).second) return false;
    for (int index = 0; index < object.GetChildCount(); ++index)
        if (auto* child = object.GetChild(index); child && !Gather(*child, guids)) return false;
    return true;
}

std::string Capture(scene::Scene& scene, std::string& reason)
{
    return scene::SceneSerializer::CapturePassiveClone(scene, reason);
}

bool RestorableMeshes(scene::Scene& scene, std::string& reason)
{
    for (auto& object : scene.GameObjects())
        if (const auto* mesh = object.GetComponent<scene::MeshRenderer>(); mesh && mesh->mesh && mesh->meshPath.empty()) {
            reason = "Scene recovery cannot restore MeshRenderer '" + object.instanceId + "' with a live mesh and no meshPath";
            return false;
        }
    return true;
}

bool PreparedSnapshotMatches(scene::Scene& prepared, const std::string& requested, std::string& reason)
{
    const auto actual = Capture(prepared, reason);
    const auto expectedDocument = toml::parse(requested), actualDocument = toml::parse(actual);
    if (actual.empty() || !expectedDocument || !actualDocument
        || Text(expectedDocument.table()) != Text(actualDocument.table())) {
        reason = "Prepared recovery snapshot differs from the requested authoring state after restore callbacks";
        return false;
    }
    return RestorableMeshes(prepared, reason);
}

std::size_t ObjectCount(const std::string& snapshot)
{
    auto parsed = toml::parse(snapshot);
    const auto* objects = parsed ? parsed.table()["gameobjects"].as_array() : nullptr;
    return objects ? objects->size() : 0;
}

bool RestoreSnapshot(EditorContext& context, const std::string& expected,
                     const std::string& replacement, std::string& reason)
{
    if (!context.activeScene || context.InPrefabEditMode()
        || (context.editScene && context.editScene != context.activeScene)
        || (context.playMode && !context.playMode->IsInEditor())) {
        reason = "Prefab history requires the Edit Scene"; return false;
    }
    auto& scene = *context.activeScene;
    if (!RestorableMeshes(scene, reason)) return false;
    if (scene.GameObjectCount() != ObjectCount(expected)) {
        reason = "Scene has objects outside the saved snapshot; history was retained"; return false;
    }
    const auto current = Capture(scene, reason);
    if (current.empty() || current != expected) {
        reason = "Scene changed since this Prefab command; history was retained"; return false;
    }
    auto prepared = scene::SceneSerializer::LoadFromText(replacement,
        context.resources, context.currentScenePath);
    if (!prepared || prepared->GameObjectCount() != ObjectCount(replacement)) {
        reason = "Could not prepare the complete Prefab history snapshot; Scene unchanged"; return false;
    }
    if (!PreparedSnapshotMatches(*prepared, replacement, reason)) return false;
    const auto verified = Capture(scene, reason);
    if (verified.empty() || verified != expected || scene.GameObjectCount() != ObjectCount(expected)) {
        reason = "Scene changed during snapshot preparation; Scene unchanged"; return false;
    }
    if (!RestorableMeshes(scene, reason)) return false;
    scene = std::move(*prepared);
    ClearEntitySelection(context);
    context.activeUICanvas = scene::EntityID::INVALID;
    if (context.markSceneDirty) context.markSceneDirty();
    reason.clear();
    return true;
}
} /// @note namespace

PrefabStructureMergeResult MergePrefabStructure(const toml::table& baseline,
                                                const toml::table& instance,
                                                const toml::table& incoming)
{
    PrefabStructureMergeResult result;
    Objects before, local, next;
    std::string beforeRoot, localRoot, nextRoot;
    if (!ReadObjects(baseline, before, beforeRoot, result, "baseline")
        || !ReadObjects(instance, local, localRoot, result, "instance")
        || !ReadObjects(incoming, next, nextRoot, result, "template")) return result;
    if (beforeRoot != localRoot || beforeRoot != nextRoot) {
        Conflict(result, beforeRoot, "root", "Root source identity changed"); return result;
    }
    Objects merged;
    std::set<std::string> ids;
    for (const auto& [id, object] : before) { (void)object; ids.insert(id); }
    for (const auto& [id, object] : local) { (void)object; ids.insert(id); }
    for (const auto& [id, object] : next) { (void)object; ids.insert(id); }
    for (const auto& id : ids) {
        const auto b = before.find(id), l = local.find(id), n = next.find(id);
        if (b == before.end()) {
            if (l != local.end() && n != next.end()) {
                Conflict(result, id, "instanceId", "Template addition collides with an instance-only ID");
            } else if (n != next.end()) {
                merged.emplace(id, n->second);
                result.changes.push_back({id, "Add from template", {}, Parent(n->second)});
            } else {
                merged.emplace(id, l->second);
                result.changes.push_back({id, "Keep local addition", {}, Parent(l->second)});
            }
            continue;
        }
        if (l == local.end()) {
            if (n != next.end() && LocalEdits(b->second, n->second, id == beforeRoot))
                Conflict(result, id, "object", "Local deletion conflicts with template edits");
            else result.changes.push_back({id, "Keep local deletion", Parent(b->second), {}});
            continue;
        }
        if (n == next.end()) {
            if (LocalEdits(b->second, l->second, id == beforeRoot))
                Conflict(result, id, "object", "Template deletion would discard local edits");
            else result.changes.push_back({id, "Remove from template", Parent(b->second), {}});
            continue;
        }
        toml::table object = n->second;
        const auto oldParent = Parent(b->second), localParent = Parent(l->second), newParent = Parent(n->second);
        if (localParent != oldParent && newParent != oldParent && localParent != newParent)
            Conflict(result, id, "parentInstanceId", "Instance and template reparented this object differently");
        const auto parent = localParent != oldParent ? localParent : newParent;
        object.insert_or_assign("parentInstanceId", parent);
        object.insert_or_assign("parent", std::string{});
        if (parent != localParent) result.changes.push_back({id, "Reparent from template", localParent, parent});
        else if (localParent != oldParent) result.changes.push_back({id, "Keep local parent", oldParent, parent});
        MergeProperties(b->second, l->second, object, id, {}, id == beforeRoot, result);
        const auto localName = l->second["name"].value_or(std::string{});
        if (id == beforeRoot || !AutomaticName(localName, b->second["name"].value_or(std::string{})))
            object.insert_or_assign("name", localName);
        if (id == beforeRoot) Assign(object, "transform", l->second.get("transform"));
        merged.emplace(id, std::move(object));
    }
    for (auto& [id, object] : merged) {
        NormalizeReferenceNames(object, merged);
        for (const auto& [key, value] : object)
            if (value.is_table() || value.is_array())
                ValidateRemovedReferences(value, ids, merged, id, std::string(key.str()), result);
        object.erase("prefabSourceId");
        object.erase("prefabAssetPath");
        object.erase("prefabSourceSnapshot");
    }
    toml::array objects;
    std::set<std::string> emitted;
    const auto appendInDocumentOrder = [&](const toml::table& document) {
        if (const auto* ordered = document["gameobjects"].as_array())
            for (const auto& item : *ordered) {
                const auto id = (*item.as_table())["instanceId"].value_or(std::string{});
                const auto found = merged.find(id);
                if (found != merged.end() && emitted.insert(id).second) objects.push_back(found->second);
            }
    };
    /// @note Retained objects keep local array order; incoming-only objects append in template order. Sibling reorder merging is separate.
    appendInDocumentOrder(instance);
    appendInDocumentOrder(incoming);
    result.document = incoming;
    result.document.insert_or_assign("gameobjects", std::move(objects));
    Objects validated;
    std::string mergedRoot;
    if (result.conflicts.empty()) ReadObjects(result.document, validated, mergedRoot, result, "merged hierarchy");
    if (result.conflicts.empty() && mergedRoot != beforeRoot)
        Conflict(result, beforeRoot, "root", "The merged hierarchy changed its root");
    return result;
}

bool PreviewPrefabStructureMerge(scene::Scene& scene, scene::EntityID rootEntity,
                                 const std::string& projectRoot, PrefabStructureMergePlan& out)
{
    out = {};
    auto* root = scene.GetGameObject(rootEntity);
    if (!root || root->prefabAssetPath.empty()) {
        Conflict(out.merge, {}, "root", "Select a connected Prefab root"); return false;
    }
    out.rootGuid = root->instanceId;
    out.projectRoot = projectRoot;
    out.assetReference = root->prefabAssetPath;
    out.diskPath = scene::ResolvePrefabAssetPath(out.assetReference, projectRoot);
    std::string recoveryReason;
    if (!RestorableMeshes(scene, recoveryReason)) {
        Conflict(out.merge, {}, "recovery", recoveryReason); return false;
    }
    if (root->prefabSourceSnapshot.empty()) {
        Conflict(out.merge, root->prefabSourceId, "baseline", "No saved baseline; structural changes cannot be inferred safely"); return false;
    }
    if (out.diskPath.empty() || !util::FileSystem::ReadText(out.diskPath, out.sourceText)) {
        Conflict(out.merge, {}, "template", "Source Prefab is missing or unreadable"); return false;
    }
    auto baseline = toml::parse(root->prefabSourceSnapshot), incoming = toml::parse(out.sourceText);
    if (!baseline || !incoming) {
        Conflict(out.merge, {}, "template", "Baseline or source Prefab is malformed"); return false;
    }
    Objects baselineObjects;
    std::string sourceRoot;
    if (!ReadObjects(baseline.table(), baselineObjects, sourceRoot, out.merge, "baseline")) return false;
    std::set<std::string> guids;
    if (!Gather(*root, guids)) {
        Conflict(out.merge, {}, "instance", "Runtime-generated objects, missing IDs or duplicate instance GUIDs require a separate review"); return false;
    }
    std::string diagnostic;
    out.sceneBefore = Capture(scene, diagnostic);
    if (out.sceneBefore.empty()) {
        Conflict(out.merge, {}, "instance", "Passive snapshot failed: " + diagnostic); return false;
    }
    auto current = toml::parse(out.sceneBefore);
    const auto* serialized = current ? current.table()["gameobjects"].as_array() : nullptr;
    if (!serialized) { Conflict(out.merge, {}, "instance", "Scene snapshot has no objects"); return false; }
    std::unordered_map<std::string, std::string> keyByGuid;
    std::set<std::string> keys;
    for (const auto& item : *serialized) {
        const auto* object = item.as_table();
        if (!object) continue;
        const auto guid = (*object)["instanceId"].value_or(std::string{});
        if (!guids.contains(guid)) continue;
        const auto sourceId = (*object)["prefabSourceId"].value_or(std::string{});
        const auto key = sourceId.empty() ? "local-instance:" + guid : sourceId;
        if (!keys.insert(key).second) { Conflict(out.merge, key, "instanceId", "Duplicate instance source ID"); return false; }
        keyByGuid.emplace(guid, key);
        out.preserveGuids.emplace(key, guid);
        out.originalSourceIds.emplace(key, sourceId);
        if (guid != out.rootGuid && !(*object)["prefabAssetPath"].value_or(std::string{}).empty()) {
            Conflict(out.merge, key, "instance", "Nested Prefab boundary requires a separate review"); return false;
        }
    }
    if (keyByGuid.size() != guids.size()) {
        Conflict(out.merge, {}, "instance", "Some instance objects are absent from the snapshot"); return false;
    }
    toml::array objects;
    for (const auto& item : *serialized) {
        const auto* object = item.as_table();
        if (!object) continue;
        const auto guid = (*object)["instanceId"].value_or(std::string{});
        if (!guids.contains(guid)) continue;
        toml::table normalized = *object;
        scene::RemapPrefabObjectReferences(normalized, keyByGuid);
        normalized.insert_or_assign("instanceId", keyByGuid.at(guid));
        const auto parentGuid = (*object)["parentInstanceId"].value_or(std::string{});
        if (guid != out.rootGuid && !keyByGuid.contains(parentGuid)) {
            Conflict(out.merge, keyByGuid.at(guid), "parentInstanceId", "Instance child has an external parent"); return false;
        }
        normalized.insert_or_assign("parentInstanceId", guid == out.rootGuid ? std::string{} : keyByGuid.at(parentGuid));
        normalized.insert_or_assign("parent", std::string{});
        normalized.erase("prefabAssetPath");
        normalized.erase("prefabSourceSnapshot");
        normalized.erase("prefabSourceId");
        NormalizeReferenceNames(normalized, baselineObjects);
        objects.push_back(std::move(normalized));
    }
    out.instanceDocument.insert("gameobjects", std::move(objects));
    out.merge = MergePrefabStructure(baseline.table(), out.instanceDocument, incoming.table());
    return out.merge.CanApply();
}

bool ApplyPrefabStructureMerge(scene::Scene& scene, const PrefabStructureMergePlan& plan,
                               std::vector<scene::EntityID>& outRoots, std::string& reason)
{
    outRoots.clear();
    if (!plan.merge.CanApply()) {
        reason = plan.merge.conflicts.empty() ? "Resolve the reported Prefab conflicts before Apply" : plan.merge.conflicts.front().reason;
        return false;
    }
    auto* root = scene.FindByGuid(plan.rootGuid);
    PrefabStructureMergePlan current;
    if (!root) { reason = "The reviewed Prefab root no longer exists"; return false; }
    if (!PreviewPrefabStructureMerge(scene, root->GetID(), plan.projectRoot, current)) {
        reason = current.merge.conflicts.empty() ? "Prefab update could not be prepared" : current.merge.conflicts.front().reason;
        return false;
    }
    if (current.sceneBefore != plan.sceneBefore || current.sourceText != plan.sourceText
        || current.diskPath != plan.diskPath || Text(current.merge.document) != Text(plan.merge.document)) {
        reason = "Scene, source template or review changed; preview again"; return false;
    }
    const auto parent = root->GetParent() ? root->GetParent()->GetID() : scene::EntityID::INVALID;
    if (!scene::ReplacePrefabInstanceDocument(scene, root->GetID(), current.diskPath,
        current.merge.document, current.sourceText, current.preserveGuids, outRoots)) {
        reason = "Prefab preparation or replacement failed; original hierarchy retained"; return false;
    }
    if (auto* replacedRoot = scene.GetGameObject(outRoots.front())) {
        if (auto* externalParent = scene.GetGameObject(parent)) replacedRoot->SetParent(*externalParent);
        scene::FlushWorldTransforms(*replacedRoot);
    }
    for (const auto& [key, guid] : current.preserveGuids)
        if (auto* object = scene.FindByGuid(guid)) object->prefabSourceId = current.originalSourceIds.at(key);
    reason.clear();
    return true;
}

std::unique_ptr<ICommand> MakePrefabStructureMergeCommand(EditorContext& context,
                                                         const PrefabStructureMergePlan& plan)
{
    struct State { bool appliedOnce = false; std::string before; std::string after; std::uint64_t sceneGeneration = 0; };
    auto state = std::make_shared<State>();
    state->before = plan.sceneBefore;
    if (context.activeScene) state->sceneGeneration = context.activeScene->GetRenderSceneGeneration();
    auto execute = [&context, plan, state](std::string& reason) {
        if (!context.activeScene || context.InPrefabEditMode()
            || (context.editScene && context.editScene != context.activeScene)
            || (context.playMode && !context.playMode->IsInEditor())) {
            reason = "Prefab Apply requires Edit mode"; return false;
        }
        if (state->appliedOnce) return RestoreSnapshot(context, state->before, state->after, reason);
        if (context.activeScene->GetRenderSceneGeneration() != state->sceneGeneration) {
            reason = "Scene was replaced after preview"; return false;
        }
        if (!RestorableMeshes(*context.activeScene, reason)) return false;
        if (context.activeScene->GameObjectCount() != ObjectCount(state->before)) {
            reason = "Scene has objects outside the recovery snapshot; Scene unchanged"; return false;
        }
        auto recovery = scene::SceneSerializer::LoadFromText(state->before,
            context.resources, context.currentScenePath);
        if (!recovery || recovery->GameObjectCount() != ObjectCount(state->before)) {
            reason = "Could not prepare the recovery snapshot; Scene unchanged"; return false;
        }
        if (!PreparedSnapshotMatches(*recovery, state->before, reason)) return false;
        std::vector<scene::EntityID> roots;
        if (!ApplyPrefabStructureMerge(*context.activeScene, plan, roots, reason)) return false;
        state->after = Capture(*context.activeScene, reason);
        if (state->after.empty() || context.activeScene->GameObjectCount() != ObjectCount(state->after)) {
            *context.activeScene = std::move(*recovery);
            ClearEntitySelection(context);
            context.activeUICanvas = scene::EntityID::INVALID;
            reason = "Final snapshot failed; original Scene recovered"; return false;
        }
        state->appliedOnce = true;
        SelectEntities(context, roots, SelectionReveal::Skip);
        if (context.markSceneDirty) context.markSceneDirty();
        return true;
    };
    auto undo = [&context, state](std::string& reason) {
        return state->appliedOnce && RestoreSnapshot(context, state->after, state->before, reason);
    };
    return std::make_unique<CheckedLambdaCommand>("Merge Prefab Structure", std::move(execute), std::move(undo));
}

bool DrawPrefabStructureMergeReview(EditorContext& context, scene::EntityID root)
{
    if (!context.activeScene || (context.playMode && !context.playMode->IsInEditor())) return false;
    auto* object = context.activeScene->GetGameObject(root);
    if (!object || object->prefabAssetPath.empty()) return false;
    static PrefabStructureMergePlan review;
    static std::string diagnostic;
    const auto rootGuid = object->instanceId;
    ImGui::PushID(rootGuid.c_str());
    if (ImGui::SmallButton("Preview Structure Update")) {
        PreviewPrefabStructureMerge(*context.activeScene, root, context.projectRoot, review);
        diagnostic.clear();
    }
    if (review.rootGuid == rootGuid) {
        ImGui::TextWrapped("Update this instance from its source. Local additions, deletions and parent overrides are preserved when compatible.");
        for (const auto& change : review.merge.changes)
            ImGui::BulletText("%s: %s (%s -> %s)", change.action.c_str(), change.sourceId.c_str(),
                change.beforeParent.c_str(), change.afterParent.c_str());
        for (const auto& conflict : review.merge.conflicts)
            ImGui::TextWrapped("Conflict [%s / %s]: %s", conflict.sourceId.c_str(), conflict.path.c_str(), conflict.reason.c_str());
        ImGui::BeginDisabled(!review.merge.CanApply() || !context.undoStack || !context.undoStack->IsRecordingEnabled());
        if (ImGui::Button("Apply Structure Update")) {
            auto command = MakePrefabStructureMergeCommand(context, review);
            if (context.undoStack->Execute(std::move(command))) {
                review = {};
                ImGui::EndDisabled();
                ImGui::PopID();
                return true;
            }
            diagnostic = context.undoStack->LastDiagnostic();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Dismiss Review")) { review = {}; diagnostic.clear(); }
        if (!diagnostic.empty()) ImGui::TextWrapped("%s", diagnostic.c_str());
    }
    ImGui::PopID();
    return false;
}
} /// @note namespace fbzz::editor
